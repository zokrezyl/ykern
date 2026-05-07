/*
 * /proc (procfs) transport — three node kinds in one file:
 *
 *   transport   /proc                 the entry-point.  Lazy-populates with
 *                                     a single child: a dir node that maps
 *                                     to the kernel's actual /proc directory.
 *   dir         /proc/proc/...        every directory under procfs.  Lazy
 *                                     populate_children opens the on-disk
 *                                     directory and spawns children for
 *                                     each entry — sub-dir or file.
 *   file        leaf                  invoke() reads the file and returns
 *                                     its contents (text up to 64 KiB).
 *
 * Symlinks are followed implicitly via stat() during populate.  Some /proc
 * files are interactive (e.g. /proc/kmsg blocks on read until messages are
 * available); ykern doesn't yet skip those — caller is on the hook for
 * picking sensible paths.
 *
 * The structure mirrors src/ykern/ysys/ysys.c almost exactly.  Both could
 * be folded into a shared fs-transport helper; for now they stay separate
 * so each transport can grow its own quirks without disturbing the other.
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>
#include <ykern/yproc/yproc.h>

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

struct ykern_yproc_yproc {
    struct ykern_ycore_object base;
};

struct ykern_yproc_dir {
    struct ykern_ycore_object base;
    char *fs_path;
    char *display_name;
};

struct ykern_yproc_file {
    struct ykern_ycore_object base;
    char *fs_path;
    char *display_name;
    int is_symlink;
};

static const struct ykern_ycore_object_ops g_dir_ops;
static const struct ykern_ycore_object_ops g_file_ops;

/*=============================================================================
 * Helpers — child spawners
 *===========================================================================*/

static struct ykern_ycore_void_result spawn_dir(struct ykern_ycore_object *parent,
                                                const char *fs_path,
                                                const char *display_name)
{
    struct ykern_yproc_dir *d = calloc(1, sizeof(*d));
    if (!d) return YKERN_ERR(ykern_ycore_void, "yproc: dir calloc failed");
    d->base.ops = &g_dir_ops;
    d->base.kind = YKERN_YCORE_OBJECT_KIND_NAMESPACE;
    d->fs_path = strdup(fs_path);
    d->display_name = strdup(display_name);
    if (!d->fs_path || !d->display_name) {
        free(d->fs_path);
        free(d->display_name);
        free(d);
        return YKERN_ERR(ykern_ycore_void, "yproc: dir strdup failed");
    }
    struct ykern_ycore_void_result r = ykern_ycore_object_append_child(parent, &d->base);
    if (YKERN_IS_ERR(r)) {
        free(d->fs_path);
        free(d->display_name);
        free(d);
        return YKERN_ERR(ykern_ycore_void, "yproc: dir append failed", r);
    }
    return YKERN_OK_VOID();
}

static struct ykern_ycore_void_result spawn_file(struct ykern_ycore_object *parent,
                                                 const char *fs_path,
                                                 const char *display_name,
                                                 int is_symlink)
{
    struct ykern_yproc_file *f = calloc(1, sizeof(*f));
    if (!f) return YKERN_ERR(ykern_ycore_void, "yproc: file calloc failed");
    f->base.ops = &g_file_ops;
    f->base.kind = YKERN_YCORE_OBJECT_KIND_VALUE;
    f->fs_path = strdup(fs_path);
    f->display_name = strdup(display_name);
    f->is_symlink = is_symlink;
    if (!f->fs_path || !f->display_name) {
        free(f->fs_path);
        free(f->display_name);
        free(f);
        return YKERN_ERR(ykern_ycore_void, "yproc: file strdup failed");
    }
    struct ykern_ycore_void_result r = ykern_ycore_object_append_child(parent, &f->base);
    if (YKERN_IS_ERR(r)) {
        free(f->fs_path);
        free(f->display_name);
        free(f);
        return YKERN_ERR(ykern_ycore_void, "yproc: file append failed", r);
    }
    return YKERN_OK_VOID();
}

/*=============================================================================
 * Directory listing
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
        return YKERN_ERR(ykern_ycore_void, "yproc: opendir failed");
    }

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
                return YKERN_ERR(ykern_ycore_void, "yproc: names realloc");
            }
            names = grown;
            cap = new_cap;
        }
        names[count] = strdup(de->d_name);
        if (!names[count]) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            closedir(dir);
            return YKERN_ERR(ykern_ycore_void, "yproc: name strdup");
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

        struct stat st, lst;
        int is_link = 0;
        if (lstat(child_path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
            is_link = 1;
        }
        if (stat(child_path, &st) < 0) {
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
 * Transport node
 *===========================================================================*/

static struct ykern_ycore_text_result transport_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("proc");
}

static struct ykern_ycore_text_result transport_get_short(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "procfs — kernel state and per-process info as a virtual filesystem");
}

static struct ykern_ycore_text_result transport_get_long(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "procfs is a virtual filesystem mounted at /proc.  Two flavours of\n"
        "content live there:\n\n"
        "  /proc/<pid>/...     per-process info — one directory per running\n"
        "                      process, plus /proc/self pointing at the\n"
        "                      caller's own pid.\n"
        "  /proc/cpuinfo       global kernel-state files: cpuinfo, meminfo,\n"
        "  /proc/meminfo       loadavg, mounts, mdstat, partitions, …\n"
        "  /proc/sys/...       writable runtime tunables (kernel.* /\n"
        "                      net.ipv4.* / vm.* etc.) — sysctl interface.\n\n"
        "Inside ykern:\n"
        "  Browse a directory  -> lists its entries.\n"
        "  Browse a file       -> shows its on-disk path + invoke hint.\n"
        "  Invoke a file       -> reads the file and returns the contents.\n\n"
        "Path convention: the first segment is the transport handler ('proc'),\n"
        "and from segment 2 onward we reproduce the kernel's own path.  So\n"
        "/proc/proc/cpuinfo corresponds to the on-disk /proc/cpuinfo — the\n"
        "doubled 'proc' is intentional.");
}

static struct ykern_ycore_void_result transport_populate(struct ykern_ycore_object *self)
{
    return spawn_dir(self, "/proc", "proc");
}

static void transport_destroy(struct ykern_ycore_object *self) { (void)self; }

static const struct ykern_ycore_object_ops g_transport_ops = {
    .get_name = transport_get_name,
    .get_short_description = transport_get_short,
    .get_long_description = transport_get_long,
    .populate_children = transport_populate,
    .destroy_impl = transport_destroy,
};

struct ykern_yproc_yproc_ptr_result ykern_yproc_yproc_create(void)
{
    struct ykern_yproc_yproc *t = calloc(1, sizeof(*t));
    if (!t) {
        return YKERN_ERR(ykern_yproc_yproc_ptr, "yproc_create: calloc failed");
    }
    t->base.ops = &g_transport_ops;
    t->base.kind = YKERN_YCORE_OBJECT_KIND_TRANSPORT;
    return YKERN_OK(ykern_yproc_yproc_ptr, t);
}

struct ykern_ycore_object *ykern_yproc_yproc_as_object(struct ykern_yproc_yproc *self)
{
    return self ? &self->base : NULL;
}

/*=============================================================================
 * Directory node
 *===========================================================================*/

static struct ykern_ycore_text_result dir_get_name(struct ykern_ycore_object *self)
{
    struct ykern_yproc_dir *d = ykern_container_of(self, struct ykern_yproc_dir, base);
    return ykern_ycore_text_from_cstr(d->display_name);
}

static struct ykern_ycore_text_result dir_get_short(struct ykern_ycore_object *self)
{
    struct ykern_yproc_dir *d = ykern_container_of(self, struct ykern_yproc_dir, base);
    return ykern_ycore_text_format("directory %s", d->fs_path);
}

static struct ykern_ycore_text_result dir_get_long(struct ykern_ycore_object *self)
{
    struct ykern_yproc_dir *d = ykern_container_of(self, struct ykern_yproc_dir, base);
    return ykern_ycore_text_format(
        "Directory **%s** in procfs.\n\n"
        "What it is:\n"
        "  An on-disk path under /proc.  Numeric entries are PIDs (one\n"
        "  directory per process); named entries are global kernel-state\n"
        "  groupings (sys, net, fs, ...).\n\n"
        "How to use it:\n"
        "  ykern browse %s             list the entries (children).\n"
        "  ykern browse <child PATH>   drill into one entry.\n",
        d->fs_path, d->fs_path);
}

static struct ykern_ycore_void_result dir_populate(struct ykern_ycore_object *self)
{
    struct ykern_yproc_dir *d = ykern_container_of(self, struct ykern_yproc_dir, base);
    return populate_from_dir(self, d->fs_path);
}

static void dir_destroy(struct ykern_ycore_object *self)
{
    struct ykern_yproc_dir *d = ykern_container_of(self, struct ykern_yproc_dir, base);
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
 * File node
 *===========================================================================*/

static struct ykern_ycore_text_result file_get_name(struct ykern_ycore_object *self)
{
    struct ykern_yproc_file *f = ykern_container_of(self, struct ykern_yproc_file, base);
    return ykern_ycore_text_from_cstr(f->display_name);
}

static struct ykern_ycore_text_result file_get_short(struct ykern_ycore_object *self)
{
    struct ykern_yproc_file *f = ykern_container_of(self, struct ykern_yproc_file, base);
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
    struct ykern_yproc_file *f = ykern_container_of(self, struct ykern_yproc_file, base);
    return ykern_ycore_text_format(
        "Procfs file **%s**.\n\n"
        "What it is:\n"
        "  A leaf in procfs.  Reading it usually returns text — kernel state\n"
        "  rendered on demand (cpuinfo, meminfo), per-process info under\n"
        "  /proc/<pid>/, or a tunable's current value under /proc/sys/.\n\n"
        "How to use it:\n"
        "  ykern browse %s    show this metadata block (no read).\n"
        "  ykern invoke %s    open the file, read up to 64 KiB, return the text.\n\n"
        "Caveats:\n"
        "  * A few /proc files block on read (e.g. /proc/kmsg waits for new\n"
        "    kernel messages).  ykern doesn't yet skip those.\n"
        "  * Some are root-only (e.g. /proc/<pid>/maps for other users'\n"
        "    processes).  ykern surfaces the kernel's errno verbatim.",
        f->fs_path, f->fs_path, f->fs_path);
}

static struct ykern_ycore_void_result file_populate(struct ykern_ycore_object *self)
{
    (void)self;
    return YKERN_OK_VOID();
}

static struct ykern_ycore_text_result file_invoke(struct ykern_ycore_object *self,
                                                  const struct ykern_ycore_invoke_args *args)
{
    (void)args;
    struct ykern_yproc_file *f = ykern_container_of(self, struct ykern_yproc_file, base);

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

    char *buf = malloc(65536);
    if (!buf) {
        close(fd);
        return YKERN_ERR(ykern_ycore_text, "yproc: read buffer alloc failed");
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

    if (total > 0 && buf[total - 1] == '\n') {
        buf[--total] = '\0';
    }

    if (total == 0) {
        free(buf);
        return ykern_ycore_text_format("%s\n  (empty)\n", f->fs_path);
    }

    struct ykern_ycore_text_result r = ykern_ycore_text_format(
        "%s\n  read %zu byte%s:\n\n%s\n",
        f->fs_path, total, total == 1 ? "" : "s", buf);
    free(buf);
    return r;
}

static void file_destroy(struct ykern_ycore_object *self)
{
    struct ykern_yproc_file *f = ykern_container_of(self, struct ykern_yproc_file, base);
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
