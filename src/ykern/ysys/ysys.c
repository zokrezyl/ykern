/*
 * /sys (sysfs) transport — three node kinds in one file:
 *
 *   transport   /sys                  the entry-point.  Lazy-populates with
 *                                     a single child: a dir node that maps
 *                                     to the kernel's actual /sys directory.
 *   dir         /sys/sys/class/...    every directory under sysfs.  Lazy
 *                                     populate_children opens the on-disk
 *                                     directory and spawns children for
 *                                     each entry — sub-dir or file.
 *   file        leaf                  invoke() reads the file and returns
 *                                     its contents (text up to 64 KiB).
 *
 * Symlinks are followed implicitly via stat() during populate, so the tree
 * shape mirrors what `ls -L /sys/...` would show.  `ykern browse -r /sys`
 * could descend a long way; the caller is in charge of bounding depth.
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>
#include <ykern/ysys/ysys.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*=============================================================================
 * Internal types
 *===========================================================================*/

struct ykern_ysys_ysys {
    struct ykern_ycore_object base;
};

struct ykern_ysys_dir {
    struct ykern_ycore_object base;
    char *fs_path;       /* on-disk path, e.g. "/sys/class/net" */
    char *display_name;  /* basename, e.g. "net" */
};

struct ykern_ysys_file {
    struct ykern_ycore_object base;
    char *fs_path;
    char *display_name;
    int is_symlink;      /* shown in description; doesn't change invoke */
};

/* Forward decls — vtables defined further down. */
static const struct ykern_ycore_object_ops g_dir_ops;
static const struct ykern_ycore_object_ops g_file_ops;

/*=============================================================================
 * Helpers — child spawners
 *===========================================================================*/

static struct ykern_ycore_void_result spawn_dir(struct ykern_ycore_object *parent,
                                                const char *fs_path,
                                                const char *display_name)
{
    struct ykern_ysys_dir *d = calloc(1, sizeof(*d));
    if (!d) return YKERN_ERR(ykern_ycore_void, "ysys: dir calloc failed");
    d->base.ops = &g_dir_ops;
    d->base.kind = YKERN_YCORE_OBJECT_KIND_NAMESPACE;
    d->fs_path = strdup(fs_path);
    d->display_name = strdup(display_name);
    if (!d->fs_path || !d->display_name) {
        free(d->fs_path);
        free(d->display_name);
        free(d);
        return YKERN_ERR(ykern_ycore_void, "ysys: dir strdup failed");
    }
    struct ykern_ycore_void_result r = ykern_ycore_object_append_child(parent, &d->base);
    if (YKERN_IS_ERR(r)) {
        free(d->fs_path);
        free(d->display_name);
        free(d);
        return YKERN_ERR(ykern_ycore_void, "ysys: dir append failed", r);
    }
    return YKERN_OK_VOID();
}

static struct ykern_ycore_void_result spawn_file(struct ykern_ycore_object *parent,
                                                 const char *fs_path,
                                                 const char *display_name,
                                                 int is_symlink)
{
    struct ykern_ysys_file *f = calloc(1, sizeof(*f));
    if (!f) return YKERN_ERR(ykern_ycore_void, "ysys: file calloc failed");
    f->base.ops = &g_file_ops;
    f->base.kind = YKERN_YCORE_OBJECT_KIND_VALUE;
    f->fs_path = strdup(fs_path);
    f->display_name = strdup(display_name);
    f->is_symlink = is_symlink;
    if (!f->fs_path || !f->display_name) {
        free(f->fs_path);
        free(f->display_name);
        free(f);
        return YKERN_ERR(ykern_ycore_void, "ysys: file strdup failed");
    }
    struct ykern_ycore_void_result r = ykern_ycore_object_append_child(parent, &f->base);
    if (YKERN_IS_ERR(r)) {
        free(f->fs_path);
        free(f->display_name);
        free(f);
        return YKERN_ERR(ykern_ycore_void, "ysys: file append failed", r);
    }
    return YKERN_OK_VOID();
}

/*=============================================================================
 * Directory listing — used by both transport_populate and dir_populate
 *===========================================================================*/

static int dirent_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static struct ykern_ycore_void_result populate_from_dir(struct ykern_ycore_object *self,
                                                        const char *fs_path)
{
    DIR *dir = opendir(fs_path);
    if (!dir) {
        return ykern_ycore_text_format("ysys: opendir('%s') failed: %s",
                                       fs_path, strerror(errno)).ok
            ? YKERN_ERR(ykern_ycore_void, "ysys: opendir failed")
            : YKERN_ERR(ykern_ycore_void, "ysys: opendir + format failed");
    }

    /* Read every entry name first, sort alphabetically, then spawn children
     * in deterministic order — readdir() doesn't guarantee any. */
    char **names = NULL;
    size_t count = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 32;
            char **grown = realloc(names, new_cap * sizeof(*names));
            if (!grown) {
                for (size_t i = 0; i < count; i++) free(names[i]);
                free(names);
                closedir(dir);
                return YKERN_ERR(ykern_ycore_void, "ysys: names realloc");
            }
            names = grown;
            cap = new_cap;
        }
        names[count] = strdup(de->d_name);
        if (!names[count]) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            closedir(dir);
            return YKERN_ERR(ykern_ycore_void, "ysys: name strdup");
        }
        count++;
    }
    closedir(dir);
    qsort(names, count, sizeof(*names), dirent_cmp);

    for (size_t i = 0; i < count; i++) {
        char child_path[PATH_MAX];
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", fs_path, names[i]);
        if (n < 0 || (size_t)n >= sizeof(child_path)) {
            free(names[i]);
            continue;
        }

        struct stat st;
        struct stat lst;
        int is_link = 0;
        if (lstat(child_path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
            is_link = 1;
        }
        if (stat(child_path, &st) < 0) {
            /* Broken symlink or permission denied; surface as a file leaf. */
            (void)spawn_file(self, child_path, names[i], is_link);
            free(names[i]);
            continue;
        }

        struct ykern_ycore_void_result r;
        if (S_ISDIR(st.st_mode)) {
            r = spawn_dir(self, child_path, names[i]);
        } else if (S_ISREG(st.st_mode) || S_ISCHR(st.st_mode) ||
                   S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode)) {
            r = spawn_file(self, child_path, names[i], is_link);
        } else {
            /* Sockets etc. — skip silently. */
            free(names[i]);
            continue;
        }
        free(names[i]);
        if (YKERN_IS_ERR(r)) {
            for (size_t j = i + 1; j < count; j++) free(names[j]);
            free(names);
            return r;
        }
    }
    free(names);
    return YKERN_OK_VOID();
}

/*=============================================================================
 * Transport node — the /sys handle.  One child: the dir node for "/sys".
 *===========================================================================*/

static struct ykern_ycore_text_result transport_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("sys");
}

static struct ykern_ycore_text_result transport_get_short(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "sysfs — kernel objects (devices, drivers, subsystems) as a virtual filesystem");
}

static struct ykern_ycore_text_result transport_get_long(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "sysfs is a virtual filesystem mounted at /sys.  Every kernel object\n"
        "(network interface, block device, PCI bus, kobject, ...) shows up as\n"
        "a directory; its tunables and statistics show up as files inside.\n\n"
        "Inside ykern:\n"
        "  Browse a directory  -> lists its entries.\n"
        "  Browse a file       -> shows its on-disk path + invoke hint.\n"
        "  Invoke a file       -> reads the file and returns the contents.\n\n"
        "Path convention: the first segment is the transport handler ('sys'),\n"
        "and from segment 2 onward we reproduce the kernel's own path.  So\n"
        "/sys/sys/class/net/eth0 corresponds to the on-disk /sys/class/net/eth0\n"
        "— the doubled 'sys' is intentional.");
}

static struct ykern_ycore_void_result transport_populate(struct ykern_ycore_object *self)
{
    return spawn_dir(self, "/sys", "sys");
}

static void transport_destroy(struct ykern_ycore_object *self) { (void)self; }

static const struct ykern_ycore_object_ops g_transport_ops = {
    .get_name = transport_get_name,
    .get_short_description = transport_get_short,
    .get_long_description = transport_get_long,
    .populate_children = transport_populate,
    .destroy_impl = transport_destroy,
};

struct ykern_ysys_ysys_ptr_result ykern_ysys_ysys_create(void)
{
    struct ykern_ysys_ysys *t = calloc(1, sizeof(*t));
    if (!t) {
        return YKERN_ERR(ykern_ysys_ysys_ptr, "ysys_create: calloc failed");
    }
    t->base.ops = &g_transport_ops;
    t->base.kind = YKERN_YCORE_OBJECT_KIND_TRANSPORT;
    return YKERN_OK(ykern_ysys_ysys_ptr, t);
}

struct ykern_ycore_object *ykern_ysys_ysys_as_object(struct ykern_ysys_ysys *self)
{
    return self ? &self->base : NULL;
}

/*=============================================================================
 * Directory node
 *===========================================================================*/

static struct ykern_ycore_text_result dir_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ysys_dir *d = ykern_container_of(self, struct ykern_ysys_dir, base);
    return ykern_ycore_text_from_cstr(d->display_name);
}

static struct ykern_ycore_text_result dir_get_short(struct ykern_ycore_object *self)
{
    struct ykern_ysys_dir *d = ykern_container_of(self, struct ykern_ysys_dir, base);
    return ykern_ycore_text_format("directory %s", d->fs_path);
}

static struct ykern_ycore_text_result dir_get_long(struct ykern_ycore_object *self)
{
    struct ykern_ysys_dir *d = ykern_container_of(self, struct ykern_ysys_dir, base);
    return ykern_ycore_text_format(
        "Directory **%s** in sysfs.\n\n"
        "What it is:\n"
        "  An on-disk path under /sys.  Each entry inside is either another\n"
        "  directory (more kernel objects) or a file (a single attribute or\n"
        "  tunable of the parent object).\n\n"
        "How to use it:\n"
        "  ykern browse %s             list the entries (children).\n"
        "  ykern browse <child PATH>   drill into one entry.\n"
        "\nNote: ykern follows symlinks transparently while walking sysfs;\n"
        "many sysfs paths (under /sys/class, /sys/block, ...) ARE symlinks\n"
        "to canonical locations under /sys/devices.",
        d->fs_path, d->fs_path);
}

static struct ykern_ycore_void_result dir_populate(struct ykern_ycore_object *self)
{
    struct ykern_ysys_dir *d = ykern_container_of(self, struct ykern_ysys_dir, base);
    return populate_from_dir(self, d->fs_path);
}

static void dir_destroy(struct ykern_ycore_object *self)
{
    struct ykern_ysys_dir *d = ykern_container_of(self, struct ykern_ysys_dir, base);
    free(d->fs_path);
    free(d->display_name);
    d->fs_path = NULL;
    d->display_name = NULL;
}

static const struct ykern_ycore_object_ops g_dir_ops = {
    .get_name = dir_get_name,
    .get_short_description = dir_get_short,
    .get_long_description = dir_get_long,
    .populate_children = dir_populate,
    .destroy_impl = dir_destroy,
};

/*=============================================================================
 * File node — invoke() does the actual read.
 *===========================================================================*/

static struct ykern_ycore_text_result file_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ysys_file *f = ykern_container_of(self, struct ykern_ysys_file, base);
    return ykern_ycore_text_from_cstr(f->display_name);
}

static struct ykern_ycore_text_result file_get_short(struct ykern_ycore_object *self)
{
    struct ykern_ysys_file *f = ykern_container_of(self, struct ykern_ysys_file, base);
    if (f->is_symlink) {
        char target[PATH_MAX];
        ssize_t n = readlink(f->fs_path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            return ykern_ycore_text_format("symlink -> %s", target);
        }
        return ykern_ycore_text_from_cstr("symlink (target unreadable)");
    }
    return ykern_ycore_text_format("file %s — invoke to read its contents",
                                   f->fs_path);
}

static struct ykern_ycore_text_result file_get_long(struct ykern_ycore_object *self)
{
    struct ykern_ysys_file *f = ykern_container_of(self, struct ykern_ysys_file, base);
    return ykern_ycore_text_format(
        "Sysfs file **%s**.\n\n"
        "What it is:\n"
        "  A leaf in sysfs — usually one attribute or tunable of the parent\n"
        "  kernel object.  Reads come back as text (sometimes a single line,\n"
        "  sometimes a few lines or a small table).\n\n"
        "How to use it:\n"
        "  ykern browse %s    show this metadata block (no read).\n"
        "  ykern invoke %s    open the file, read up to 64 KiB, return the text.\n\n"
        "Some sysfs files are write-only, root-only, or trigger an action on\n"
        "read.  ykern surfaces the kernel's errno verbatim if the read fails.",
        f->fs_path, f->fs_path, f->fs_path);
}

static struct ykern_ycore_void_result file_populate(struct ykern_ycore_object *self)
{
    (void)self; /* leaves */
    return YKERN_OK_VOID();
}

static struct ykern_ycore_text_result file_invoke(struct ykern_ycore_object *self,
                                                  const struct ykern_ycore_invoke_args *args)
{
    (void)args;
    struct ykern_ysys_file *f = ykern_container_of(self, struct ykern_ysys_file, base);

    /* Symlink? Show the target rather than trying to read. */
    if (f->is_symlink) {
        char target[PATH_MAX];
        ssize_t n = readlink(f->fs_path, target, sizeof(target) - 1);
        if (n < 0) {
            return ykern_ycore_text_format("readlink('%s') failed: %s",
                                           f->fs_path, strerror(errno));
        }
        target[n] = '\0';
        return ykern_ycore_text_format("%s -> %s\n", f->fs_path, target);
    }

    int fd = open(f->fs_path, O_RDONLY);
    if (fd < 0) {
        return ykern_ycore_text_format("open('%s') failed: %s",
                                       f->fs_path, strerror(errno));
    }

    /* Read up to 64 KiB.  Most sysfs files are well under a kilobyte; the
     * cap protects us from an unexpectedly chatty file (e.g. some debugfs
     * leaks into sysfs paths). */
    char *buf = malloc(65536);
    if (!buf) {
        close(fd);
        return YKERN_ERR(ykern_ycore_text, "ysys: read buffer alloc failed");
    }

    size_t total = 0;
    while (total < 65535) {
        ssize_t n = read(fd, buf + total, 65535 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            free(buf);
            return ykern_ycore_text_format("read('%s') failed: %s",
                                           f->fs_path, strerror(e));
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';

    /* Trim a single trailing newline so the output isn't visually doubled. */
    if (total > 0 && buf[total - 1] == '\n') {
        buf[--total] = '\0';
    }

    if (total == 0) {
        free(buf);
        return ykern_ycore_text_format("%s\n  (empty)\n", f->fs_path);
    }

    /* Most sysfs content is plain text; render in a labelled block. */
    struct ykern_ycore_text_result r = ykern_ycore_text_format(
        "%s\n  read %zu byte%s:\n\n%s\n",
        f->fs_path, total, total == 1 ? "" : "s", buf);
    free(buf);
    return r;
}

static void file_destroy(struct ykern_ycore_object *self)
{
    struct ykern_ysys_file *f = ykern_container_of(self, struct ykern_ysys_file, base);
    free(f->fs_path);
    free(f->display_name);
    f->fs_path = NULL;
    f->display_name = NULL;
}

static const struct ykern_ycore_object_ops g_file_ops = {
    .get_name = file_get_name,
    .get_short_description = file_get_short,
    .get_long_description = file_get_long,
    .populate_children = file_populate,
    .invoke = file_invoke,
    .destroy_impl = file_destroy,
};
