/*
 * ykern — CLI browser for the kernel-object tree.
 *
 *   ykern [OPTIONS] [PATH]
 *
 * Default behaviour: print info about the object at PATH (root if omitted)
 * and list its immediate children. Use -l to include the long description,
 * -L to skip the object info and only list children, -r to walk the entire
 * subtree.
 */

#include "color.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/root.h>
#include <ykern/ycore/text.h>
#include <ykern/ynetlink/ynetlink.h>
#include <ykern/ysys/ysys.h>
#include <ykern/yproc/yproc.h>

#define YKERN_TOOL_VERSION "0.1.0"

struct opts {
    int verbosity;        /* counts -l flags: 0 default, 1 long desc, 2 + tech, 3 + raw */
    int list_only;
    int recurse;
    int verbose_children; /* show short desc per child in listings */

    /* `invoke` mode */
    int invoke_mode;
    struct ykern_ycore_invoke_arg *args;
    size_t args_count;
    size_t args_capacity;
};

static int opts_push_arg(struct opts *o, const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq) {
        fprintf(stderr, "ykern: --arg expects KEY=VALUE (got '%s')\n", kv);
        return -1;
    }
    if (o->args_count == o->args_capacity) {
        size_t new_cap = o->args_capacity ? o->args_capacity * 2 : 8;
        struct ykern_ycore_invoke_arg *grown =
            realloc(o->args, new_cap * sizeof(*o->args));
        if (!grown) {
            fprintf(stderr, "ykern: out of memory growing args\n");
            return -1;
        }
        o->args = grown;
        o->args_capacity = new_cap;
    }
    /* Split kv at '=' into key + value, both heap-owned. */
    size_t klen = (size_t)(eq - kv);
    char *key = strndup(kv, klen);
    char *value = strdup(eq + 1);
    if (!key || !value) {
        free(key); free(value);
        fprintf(stderr, "ykern: out of memory copying arg\n");
        return -1;
    }
    o->args[o->args_count++] = (struct ykern_ycore_invoke_arg){
        .key = key, .value = value,
    };
    return 0;
}

static void opts_free_args(struct opts *o)
{
    for (size_t i = 0; i < o->args_count; i++) {
        free((char *)o->args[i].key);
        free((char *)o->args[i].value);
    }
    free(o->args);
    o->args = NULL;
    o->args_count = o->args_capacity = 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage:\n"
            "  ykern browse [OPTIONS] PATH              describe a node — no kernel I/O\n"
            "  ykern invoke PATH [--arg K=V ...]        actually call it — sends a request\n"
            "  ykern --help | --version\n"
            "\n"
            "The first positional argument is required and must be one of:\n"
            "  browse   Reads ykern's own metadata about the kernel surface —\n"
            "           names, types, accepted args. Nothing leaves your\n"
            "           process; this can't fail with EPERM, can't trigger\n"
            "           side effects. Works on every node kind.\n"
            "  invoke   Opens a netlink socket, sends a real request, prints\n"
            "           the kernel's reply. Only meaningful on [operation]\n"
            "           nodes — root, transports, namespaces, families,\n"
            "           attributes and multicast groups have nothing to call.\n"
            "\n"
            "Every browse output prints a \"What you can do here\" block so you\n"
            "can drill in step by step. Operations also print an Invoke block\n"
            "with the exact command line for the kernel call.\n"
            "\n"
            "Arguments:\n"
            "  PATH                 Kernel-object path (default \"/\").\n"
            "                       Examples:\n"
            "                         /\n"
            "                         /netlink\n"
            "                         /netlink/genl\n"
            "                         /netlink/genl/ethtool/LINKINFO_GET\n"
            "\n"
            "Browse options:\n"
            "  -l, --long           Increase detail. Repeat for more:\n"
            "                         -l    long description\n"
            "                         -ll   + technical detail (raw ids, flags)\n"
            "                         -lll  + everything we know\n"
            "  -L, --list-only      Skip object info; print children only.\n"
            "  -r, --recurse        Walk the entire subtree from PATH.\n"
            "  -q, --quiet-children List children by name only (no short desc).\n"
            "\n"
            "Invoke options:\n"
            "  -a, --arg KEY=VALUE  Pass an argument to the operation; repeat as\n"
            "                       needed. Example:\n"
            "                         ykern invoke /netlink/genl/ethtool/LINKINFO_GET \\\n"
            "                                      --arg ifname=eth0\n"
            "\n"
            "General:\n"
            "  -h, --help           Show this help and exit.\n"
            "  -V, --version        Show version and exit.\n"
            "\n"
            "Exit status: 0 on success, 2 on usage or path-resolution error,\n"
            "1 on internal error.\n");
}

/*-----------------------------------------------------------------------------
 * Path resolution
 *---------------------------------------------------------------------------*/

static int name_matches(struct ykern_ycore_object *child, const char *want)
{
    struct ykern_ycore_text_result nr = ykern_ycore_object_get_name(child);
    if (YKERN_IS_ERR(nr)) {
        ykern_ycore_error_destroy(nr.error);
        return 0;
    }
    int m = strcmp(nr.value.data, want) == 0;
    ykern_ycore_text_destroy(&nr.value);
    return m;
}

static struct ykern_ycore_object *find_child(struct ykern_ycore_object *parent, const char *name)
{
    struct ykern_ycore_object_list_result kr = ykern_ycore_object_get_children(parent);
    if (YKERN_IS_ERR(kr)) {
        fprintf(stderr, "ykern: get_children failed: %s\n", kr.error.msg ? kr.error.msg : "?");
        ykern_ycore_error_destroy(kr.error);
        return NULL;
    }
    for (size_t i = 0; i < kr.value.count; i++) {
        if (name_matches(kr.value.items[i], name)) {
            return kr.value.items[i];
        }
    }
    return NULL;
}

/* Resolve PATH starting from ROOT. On success returns the target object. On
 * failure returns NULL; *deepest is set to the last successfully reached
 * object (useful for "did you mean" hints) and *missing receives the segment
 * that didn't match (caller frees with free()). */
static struct ykern_ycore_object *resolve_path(struct ykern_ycore_object *root, const char *path,
                                               struct ykern_ycore_object **deepest,
                                               char **missing)
{
    *deepest = root;
    *missing = NULL;
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        return root;
    }

    char *copy = strdup(path);
    if (!copy) {
        return NULL;
    }

    char *saveptr = NULL;
    struct ykern_ycore_object *cur = root;
    char *seg = strtok_r(copy, "/", &saveptr);
    while (seg && cur) {
        struct ykern_ycore_object *next = find_child(cur, seg);
        if (!next) {
            *missing = strdup(seg);
            *deepest = cur;
            free(copy);
            return NULL;
        }
        cur = next;
        *deepest = cur;
        seg = strtok_r(NULL, "/", &saveptr);
    }
    free(copy);
    return cur;
}

/*-----------------------------------------------------------------------------
 * Printing
 *---------------------------------------------------------------------------*/

static void print_long_indented(const char *text)
{
    const char *p = text;
    const char *line = p;
    for (;; p++) {
        if (*p == '\n' || *p == '\0') {
            printf("  %.*s\n", (int)(p - line), line);
            if (*p == '\0') {
                break;
            }
            line = p + 1;
        }
    }
}

/* Returns 1 if the op name (final path segment) looks like an ethtool-style
 * read op for which our generic invoke is wired. */
static int is_ethtool_read_op(const char *path)
{
    if (!path) return 0;
    if (strncmp(path, "/netlink/genl/ethtool/", 22) != 0) return 0;
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    const char *name = slash + 1;
    size_t nlen = strlen(name);
    return (nlen >= 4 && strcmp(name + nlen - 4, "_GET") == 0) ||
           (nlen >= 8 && strcmp(name + nlen - 8, "_GET_CFG") == 0) ||
           (nlen >= 11 && strcmp(name + nlen - 11, "_GET_STATUS") == 0);
}

/*-----------------------------------------------------------------------------
 * print_what_you_can_do — per-node-kind beginner guidance.
 *
 * Goal: never leave the user on a leaf without telling them what to type
 * next.  For every kind, show concrete commands they can copy-paste.
 *---------------------------------------------------------------------------*/
static void print_what_you_can_do(struct ykern_ycore_object *obj, int verbosity)
{
    enum ykern_ycore_object_kind kind = ykern_ycore_object_get_kind(obj);
    struct ykern_ycore_text_result pr = ykern_ycore_object_get_path(obj);
    if (YKERN_IS_ERR(pr)) {
        ykern_ycore_error_destroy(pr.error);
        return;
    }
    const char *path = pr.value.data;
    int has_path = pr.value.size > 0;

    printf("\n%sWhat you can do here:%s\n", col_section(), col_reset());

    /* The root path is just "/"; everything else has a leading slash too,
     * so when we want "<path>/<NAME>" we'd otherwise produce "//<NAME>".
     * Use empty string when path == "/". */
    const char *child_prefix = (has_path && strcmp(path, "/") != 0) ? path : "";
    const char *self_arg = has_path ? path : "/";

#define BULLET "  %s•%s "
    /* Always-applicable browse helpers — except for leaves (attributes,
     * value files) where "drill into a child" is misleading. */
    int is_leaf = kind == YKERN_YCORE_OBJECT_KIND_ATTRIBUTE ||
                  kind == YKERN_YCORE_OBJECT_KIND_VALUE;
    if (!is_leaf) {
        printf(BULLET "Drill into a child:   %sykern browse %s/<NAME>%s\n",
               col_bullet(), col_reset(), col_command(), child_prefix, col_reset());
        printf(BULLET "Recurse the subtree:  %sykern browse -r %s%s\n",
               col_bullet(), col_reset(), col_command(), self_arg, col_reset());
    }
    if (verbosity < 1) {
        printf(BULLET "Get more detail here: %sykern browse -l %s%s\n",
               col_bullet(), col_reset(), col_command(), self_arg, col_reset());
    } else if (verbosity < 2) {
        printf(BULLET "Get even more here:   %sykern browse -ll %s%s\n",
               col_bullet(), col_reset(), col_command(), self_arg, col_reset());
    }

    /* Kind-specific guidance. */
    switch (kind) {
    case YKERN_YCORE_OBJECT_KIND_ROOT:
        printf(BULLET "Each top-level entry is one transport (a way to talk to\n",
               col_bullet(), col_reset());
        printf("    the kernel). Browse one to see what it exposes.\n");
        break;

    case YKERN_YCORE_OBJECT_KIND_TRANSPORT:
        printf(BULLET "This transport groups one or more namespaces (e.g. genl,\n",
               col_bullet(), col_reset());
        printf("    rtnl). Browse a namespace child to see its families.\n");
        break;

    case YKERN_YCORE_OBJECT_KIND_NAMESPACE:
        printf(BULLET "Each child here is a kernel-side family. Pick one and\n",
               col_bullet(), col_reset());
        printf("    browse it to see its operations and notification groups.\n");
        break;

    case YKERN_YCORE_OBJECT_KIND_FAMILY:
        printf(BULLET "Each %s[operation]%s child is something you can call.\n",
               col_bullet(), col_reset(), col_kind(), col_reset());
        printf(BULLET "Each %s[mcast-group]%s child is a notification stream you\n",
               col_bullet(), col_reset(), col_kind(), col_reset());
        printf("    could subscribe to from a netlink socket.\n");
        break;

    case YKERN_YCORE_OBJECT_KIND_OPERATION:
        printf(BULLET "Operations have %stwo modes%s:\n",
               col_bullet(), col_reset(), col_section(), col_reset());
        printf("        Browse (the current command):  describes the op,\n");
        printf("            lists accepted args, no kernel call.\n");
        printf("        Invoke (separate command):     sends the actual\n");
        printf("            netlink request and prints the reply.\n");
        printf("        See the %sInvoke%s section just below for the exact line.\n",
               col_section(), col_reset());
        break;

    case YKERN_YCORE_OBJECT_KIND_ATTRIBUTE: {
        const char *slash = has_path ? strrchr(path, '/') : NULL;
        int parent_len = slash ? (int)(slash - path) : 0;
        const char *attr_name = slash ? slash + 1 : "?";
        printf(BULLET "Pass this attribute as an --arg to the parent operation:\n",
               col_bullet(), col_reset());
        printf("      %sykern invoke %.*s --arg %s%s%s=<VALUE>%s\n",
               col_command(), parent_len, path, col_attr(), attr_name,
               col_command(), col_reset());
        printf("    Numeric VALUE is packed as u32; anything else as string.\n");
        break;
    }

    case YKERN_YCORE_OBJECT_KIND_MCAST_GROUP:
        printf(BULLET "Multicast groups deliver kernel notifications.\n",
               col_bullet(), col_reset());
        printf(BULLET "A program subscribes by calling setsockopt(NETLINK_ADD_MEMBERSHIP)\n",
               col_bullet(), col_reset());
        printf("    on a netlink socket. ykern doesn't yet listen for events itself.\n");
        break;

    case YKERN_YCORE_OBJECT_KIND_VALUE:
        printf(BULLET "This is a leaf — a value the kernel exposes.\n",
               col_bullet(), col_reset());
        printf(BULLET "Read its contents (this is the actual kernel I/O):\n",
               col_bullet(), col_reset());
        printf("        %sykern invoke %s%s\n",
               col_command(), self_arg, col_reset());
        break;
    }
#undef BULLET

    ykern_ycore_text_destroy(&pr.value);
}

static void print_invoke_hint(struct ykern_ycore_object *obj)
{
    if (ykern_ycore_object_get_kind(obj) != YKERN_YCORE_OBJECT_KIND_OPERATION) {
        return;
    }
    struct ykern_ycore_text_result pr = ykern_ycore_object_get_path(obj);
    if (YKERN_IS_ERR(pr)) {
        ykern_ycore_error_destroy(pr.error);
        return;
    }

    printf("\n%sInvoke:%s  (actually sends a netlink request to the kernel)\n",
           col_section(), col_reset());
    printf("  %sykern invoke %s --arg KEY=VALUE [--arg ...]%s\n",
           col_command(), pr.value.data, col_reset());

    /* Show the accepted KEYs inline, lifted from the operation's children
     * (which are the attribute nodes spawned by op_populate_children).  This
     * is the single source of truth — children below show the same list. */
    struct ykern_ycore_object_list_result kr = ykern_ycore_object_get_children(obj);
    if (YKERN_IS_OK(kr) && kr.value.count > 0) {
        printf("\n  Accepted --arg KEYs (%zu, also listed as children below):\n   ",
               kr.value.count);
        size_t col = 4;
        for (size_t i = 0; i < kr.value.count; i++) {
            struct ykern_ycore_text_result nr =
                ykern_ycore_object_get_name(kr.value.items[i]);
            struct ykern_ycore_text_result sr =
                ykern_ycore_object_get_short_description(kr.value.items[i]);
            if (YKERN_IS_ERR(nr)) {
                ykern_ycore_error_destroy(nr.error);
                if (YKERN_IS_OK(sr)) ykern_ycore_text_destroy(&sr.value);
                else ykern_ycore_error_destroy(sr.error);
                continue;
            }
            /* Pull just the short type prefix off the description, e.g.
             * the first token before " — id=" / " (no type info)". */
            char type_buf[32] = {0};
            if (YKERN_IS_OK(sr) && sr.value.size > 0) {
                const char *src = sr.value.data;
                if (strncmp(src, "id=", 3) != 0 &&
                    strncmp(src, "(no type", 8) != 0) {
                    size_t k = 0;
                    while (src[k] && src[k] != ' ' && k < sizeof(type_buf) - 1) {
                        type_buf[k] = src[k];
                        k++;
                    }
                    type_buf[k] = '\0';
                }
            }
            size_t name_len = nr.value.size;
            size_t entry_len = name_len + (type_buf[0] ? strlen(type_buf) + 3 : 0);
            size_t needed = entry_len + 2;
            if (col + needed > 78) {
                printf("\n   ");
                col = 4;
            }
            if (type_buf[0]) {
                printf(" %s%s%s (%s)", col_attr(), nr.value.data, col_reset(),
                       type_buf);
            } else {
                printf(" %s%s%s", col_attr(), nr.value.data, col_reset());
            }
            col += entry_len + 1;
            if (i + 1 < kr.value.count) {
                printf(",");
                col++;
            }
            ykern_ycore_text_destroy(&nr.value);
            if (YKERN_IS_OK(sr)) ykern_ycore_text_destroy(&sr.value);
            else ykern_ycore_error_destroy(sr.error);
        }
        printf("\n\n  No --arg → NLM_F_DUMP (kernel iterates every object).\n");
        printf("  %snest%s attributes contain other attrs — pass values for the\n",
               col_yellow(), col_reset());
        printf("  inner attrs instead. Numeric VALUE is currently packed as\n");
        printf("  u32; anything else as a NUL-terminated string.\n");
    } else if (is_ethtool_read_op(pr.value.data)) {
        printf("\n  Examples (read op, ethtool):\n");
        printf("    ykern invoke %s --arg ifname=<NIC>\n", pr.value.data);
        printf("    ykern invoke %s --arg ifindex=<u32>\n", pr.value.data);
        printf("    ykern invoke %s\n", pr.value.data);
        printf("        (no ifname / ifindex → DUMP every NIC)\n");
    } else {
        printf("\n  No attribute metadata for this op (regen the overlay or\n");
        printf("  hand-curate it). Without keys, only NLM_F_DUMP works:\n");
        printf("    ykern invoke %s\n", pr.value.data);
    }
    if (YKERN_IS_ERR(kr)) {
        ykern_ycore_error_destroy(kr.error);
    }

    ykern_ycore_text_destroy(&pr.value);
}

static void print_what_you_can_do(struct ykern_ycore_object *obj, int verbosity);

static void print_object(struct ykern_ycore_object *obj, int verbosity)
{
    struct ykern_ycore_text_result pr = ykern_ycore_object_get_path(obj);
    struct ykern_ycore_text_result nr = ykern_ycore_object_get_name(obj);
    struct ykern_ycore_text_result sr = ykern_ycore_object_get_short_description(obj);

    if (YKERN_IS_ERR(pr) || YKERN_IS_ERR(nr) || YKERN_IS_ERR(sr)) {
        fprintf(stderr, "ykern: print_object: getter failed\n");
        if (YKERN_IS_OK(pr)) ykern_ycore_text_destroy(&pr.value);
        else ykern_ycore_error_destroy(pr.error);
        if (YKERN_IS_OK(nr)) ykern_ycore_text_destroy(&nr.value);
        else ykern_ycore_error_destroy(nr.error);
        if (YKERN_IS_OK(sr)) ykern_ycore_text_destroy(&sr.value);
        else ykern_ycore_error_destroy(sr.error);
        return;
    }

    printf("%s[%s]%s %s%s%s\n",
           col_kind(),
           ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(obj)),
           col_reset(),
           col_path(),
           pr.value.size > 0 ? pr.value.data : "/",
           col_reset());
    /* Make it unmissable that browsing is metadata-only — no socket opened,
     * no message sent.  Important for operations where the user might
     * otherwise expect this command to "do" something. */
    printf("  %s(browse mode — describes this node only; no kernel call sent)%s\n",
           col_dim(), col_reset());
    printf("  %sname:%s %s\n", col_dim(), col_reset(),
           nr.value.size ? nr.value.data : "(empty)");
    printf("  %sdesc:%s %s\n", col_dim(), col_reset(), sr.value.data);

    if (verbosity >= 1) {
        struct ykern_ycore_text_result lr = ykern_ycore_object_get_long_description(obj);
        if (YKERN_IS_OK(lr)) {
            printf("\n");
            print_long_indented(lr.value.data);
            ykern_ycore_text_destroy(&lr.value);
        } else {
            ykern_ycore_error_destroy(lr.error);
        }
    }

    if (verbosity >= 2) {
        printf("\n%sTechnical:%s\n", col_section(), col_reset());
        printf("  %skind%s        = %s\n", col_dim(), col_reset(),
               ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(obj)));
        printf("  %spath%s        = %s\n", col_dim(), col_reset(), pr.value.data);
        printf("  %sfull name%s   = %s\n", col_dim(), col_reset(),
               nr.value.size ? nr.value.data : "(empty)");
        if (verbosity >= 3) {
            printf("  %s(level 3 reserved for protocol-level details — coming when\n",
                   col_dim());
            printf("   per-attribute type info from CTRL_CMD_GETPOLICY is wired)%s\n",
                   col_reset());
        }
    }

    /* What you can do here — printed for every browse so beginners know
     * the next move. */
    print_what_you_can_do(obj, verbosity);

    ykern_ycore_text_destroy(&pr.value);
    ykern_ycore_text_destroy(&nr.value);
    ykern_ycore_text_destroy(&sr.value);
}

static void print_children(struct ykern_ycore_object *obj, int verbose)
{
    struct ykern_ycore_object_list_result kr = ykern_ycore_object_get_children(obj);
    if (YKERN_IS_ERR(kr)) {
        fprintf(stderr, "ykern: get_children failed: %s\n", kr.error.msg ? kr.error.msg : "?");
        ykern_ycore_error_destroy(kr.error);
        return;
    }

    printf("\n%schildren (%zu):%s\n", col_section(), kr.value.count, col_reset());
    if (kr.value.count == 0) {
        printf("  (none)\n");
        return;
    }

    for (size_t i = 0; i < kr.value.count; i++) {
        struct ykern_ycore_object *c = kr.value.items[i];
        struct ykern_ycore_text_result nr = ykern_ycore_object_get_name(c);
        const char *kind = ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(c));
        const char *name = YKERN_IS_OK(nr) ? nr.value.data : "?";

        if (verbose) {
            struct ykern_ycore_text_result sr = ykern_ycore_object_get_short_description(c);
            if (YKERN_IS_OK(sr)) {
                printf("  %s[%-9s]%s %s%s%s: %s\n",
                       col_kind(), kind, col_reset(),
                       col_attr(), name, col_reset(),
                       sr.value.data);
                ykern_ycore_text_destroy(&sr.value);
            } else {
                printf("  %s[%-9s]%s %s%s%s\n",
                       col_kind(), kind, col_reset(),
                       col_attr(), name, col_reset());
                ykern_ycore_error_destroy(sr.error);
            }
        } else {
            printf("  %s[%-9s]%s %s%s%s\n",
                   col_kind(), kind, col_reset(),
                   col_attr(), name, col_reset());
        }

        if (YKERN_IS_OK(nr)) ykern_ycore_text_destroy(&nr.value);
        else ykern_ycore_error_destroy(nr.error);
    }
}

static void recurse_subtree(struct ykern_ycore_object *obj, int with_long, int depth)
{
    for (int i = 0; i < depth; i++) {
        fputs("  ", stdout);
    }
    /* Inline minimal version to keep recursion output dense. */
    struct ykern_ycore_text_result pr = ykern_ycore_object_get_path(obj);
    struct ykern_ycore_text_result sr = ykern_ycore_object_get_short_description(obj);
    const char *kind = ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(obj));
    if (YKERN_IS_OK(pr)) {
        printf("[%s] %s\n", kind, pr.value.size > 0 ? pr.value.data : "/");
        if (YKERN_IS_OK(sr)) {
            for (int i = 0; i < depth; i++) fputs("  ", stdout);
            printf("    %s\n", sr.value.data);
        }
        if (with_long) {
            struct ykern_ycore_text_result lr = ykern_ycore_object_get_long_description(obj);
            if (YKERN_IS_OK(lr)) {
                print_long_indented(lr.value.data);
                ykern_ycore_text_destroy(&lr.value);
            } else {
                ykern_ycore_error_destroy(lr.error);
            }
        }
        ykern_ycore_text_destroy(&pr.value);
    } else {
        ykern_ycore_error_destroy(pr.error);
    }
    if (YKERN_IS_OK(sr)) ykern_ycore_text_destroy(&sr.value);
    else ykern_ycore_error_destroy(sr.error);

    struct ykern_ycore_object_list_result kr = ykern_ycore_object_get_children(obj);
    if (YKERN_IS_ERR(kr)) {
        ykern_ycore_error_destroy(kr.error);
        return;
    }
    for (size_t i = 0; i < kr.value.count; i++) {
        recurse_subtree(kr.value.items[i], with_long, depth + 1);
    }
}

/*-----------------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    color_init();
    struct opts opts = {
        .verbosity = 0,
        .list_only = 0,
        .recurse = 0,
        .verbose_children = 1,
    };

    /* The first positional argument is the verb — required.  This avoids the
     * trap where `ykern /some/op/path` looks like it might call the kernel
     * but really only prints metadata.  Two top-level flags (`-h`/`--help`
     * and `-V`/`--version`) are also accepted without a verb. */
    int argi_start = 1;
    if (argc < 2) {
        fprintf(stderr, "ykern: a verb is required (browse or invoke).\n\n");
        usage(stderr);
        opts_free_args(&opts);
        return 2;
    }
    if (strcmp(argv[1], "browse") == 0) {
        argi_start = 2;
    } else if (strcmp(argv[1], "invoke") == 0) {
        opts.invoke_mode = 1;
        argi_start = 2;
    } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        opts_free_args(&opts);
        return 0;
    } else if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("ykern %s\n", YKERN_TOOL_VERSION);
        opts_free_args(&opts);
        return 0;
    } else {
        fprintf(stderr,
                "ykern: first argument must be a verb (browse or invoke), "
                "got '%s'.\n\n", argv[1]);
        usage(stderr);
        opts_free_args(&opts);
        return 2;
    }

    static const struct option longopts[] = {
        {"long", no_argument, NULL, 'l'},
        {"list-only", no_argument, NULL, 'L'},
        {"recurse", no_argument, NULL, 'r'},
        {"quiet-children", no_argument, NULL, 'q'},
        {"arg", required_argument, NULL, 'a'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;
    /* Make getopt scan from after the optional verb. */
    optind = argi_start;
    while ((c = getopt_long(argc, argv, "lLrqa:hV", longopts, NULL)) != -1) {
        switch (c) {
        case 'l': opts.verbosity++; break;
        case 'L': opts.list_only = 1; break;
        case 'r': opts.recurse = 1; break;
        case 'q': opts.verbose_children = 0; break;
        case 'a':
            if (opts_push_arg(&opts, optarg) < 0) {
                opts_free_args(&opts);
                return 2;
            }
            break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("ykern %s\n", YKERN_TOOL_VERSION); return 0;
        case '?':
        default:
            usage(stderr);
            opts_free_args(&opts);
            return 2;
        }
    }

    const char *path = "/";
    if (opts.invoke_mode) {
        if (optind >= argc) {
            fprintf(stderr, "ykern: invoke needs a PATH argument\n");
            usage(stderr);
            opts_free_args(&opts);
            return 2;
        }
        path = argv[optind];
        if (optind + 1 < argc) {
            fprintf(stderr, "ykern: too many arguments after PATH\n");
            usage(stderr);
            opts_free_args(&opts);
            return 2;
        }
    } else {
        if (optind < argc) {
            path = argv[optind];
            if (optind + 1 < argc) {
                fprintf(stderr, "ykern: too many arguments\n");
                usage(stderr);
                opts_free_args(&opts);
                return 2;
            }
        }
    }

    /*-------------------------------------------------------------------------
     * Build the tree: a netlink transport, plus the synthetic root.
     *-----------------------------------------------------------------------*/
    struct ykern_ynetlink_ynetlink_ptr_result nl_r = ykern_ynetlink_ynetlink_create();
    if (YKERN_IS_ERR(nl_r)) {
        fprintf(stderr, "ykern: ynetlink create failed: %s\n",
                nl_r.error.msg ? nl_r.error.msg : "?");
        ykern_ycore_error_destroy(nl_r.error);
        return 1;
    }
    struct ykern_ysys_ysys_ptr_result sys_r = ykern_ysys_ysys_create();
    if (YKERN_IS_ERR(sys_r)) {
        fprintf(stderr, "ykern: ysys create failed: %s\n",
                sys_r.error.msg ? sys_r.error.msg : "?");
        ykern_ycore_error_destroy(sys_r.error);
        ykern_ycore_object_destroy(ykern_ynetlink_ynetlink_as_object(nl_r.value));
        return 1;
    }
    struct ykern_yproc_yproc_ptr_result proc_r = ykern_yproc_yproc_create();
    if (YKERN_IS_ERR(proc_r)) {
        fprintf(stderr, "ykern: yproc create failed: %s\n",
                proc_r.error.msg ? proc_r.error.msg : "?");
        ykern_ycore_error_destroy(proc_r.error);
        ykern_ycore_object_destroy(ykern_ynetlink_ynetlink_as_object(nl_r.value));
        ykern_ycore_object_destroy(ykern_ysys_ysys_as_object(sys_r.value));
        return 1;
    }
    struct ykern_ycore_object *transports[] = {
        ykern_ynetlink_ynetlink_as_object(nl_r.value),
        ykern_ysys_ysys_as_object(sys_r.value),
        ykern_yproc_yproc_as_object(proc_r.value),
    };
    struct ykern_ycore_object_ptr_result root_r =
        ykern_ycore_root_create(transports, sizeof(transports) / sizeof(*transports));
    if (YKERN_IS_ERR(root_r)) {
        fprintf(stderr, "ykern: root create failed: %s\n",
                root_r.error.msg ? root_r.error.msg : "?");
        ykern_ycore_error_destroy(root_r.error);
        ykern_ycore_object_destroy(transports[0]);
        return 1;
    }
    struct ykern_ycore_object *root = root_r.value;

    /*-------------------------------------------------------------------------
     * Resolve and present.
     *-----------------------------------------------------------------------*/
    struct ykern_ycore_object *deepest = NULL;
    char *missing = NULL;
    struct ykern_ycore_object *target = resolve_path(root, path, &deepest, &missing);

    int exit_code = 0;
    if (!target) {
        fprintf(stderr, "ykern: path '%s' not found", path);
        if (missing) {
            fprintf(stderr, " (segment '%s' has no match)", missing);
        }
        fprintf(stderr, "\n");
        if (deepest) {
            struct ykern_ycore_text_result dpr = ykern_ycore_object_get_path(deepest);
            if (YKERN_IS_OK(dpr)) {
                fprintf(stderr, "ykern: under '%s' the available children are:\n",
                        dpr.value.size > 0 ? dpr.value.data : "/");
                ykern_ycore_text_destroy(&dpr.value);
            } else {
                ykern_ycore_error_destroy(dpr.error);
            }
            print_children(deepest, 0);
        }
        free(missing);
        exit_code = 2;
    } else if (opts.invoke_mode) {
        struct ykern_ycore_invoke_args ia = {
            .entries = opts.args,
            .count = opts.args_count,
        };
        struct ykern_ycore_text_result r = ykern_ycore_object_invoke(target, &ia);
        if (YKERN_IS_ERR(r)) {
            fprintf(stderr, "ykern: invoke failed: %s\n", r.error.msg ? r.error.msg : "?");
            ykern_ycore_error_destroy(r.error);
            exit_code = 1;
        } else {
            fwrite(r.value.data, 1, r.value.size, stdout);
            if (r.value.size && r.value.data[r.value.size - 1] != '\n') {
                fputc('\n', stdout);
            }
            ykern_ycore_text_destroy(&r.value);
        }
        free(missing);
    } else {
        if (opts.recurse) {
            recurse_subtree(target, opts.verbosity, 0);
        } else if (opts.list_only) {
            print_children(target, opts.verbose_children);
        } else {
            print_object(target, opts.verbosity);
            print_invoke_hint(target);
            print_children(target, opts.verbose_children);
        }
        free(missing);
    }

    ykern_ycore_object_destroy(root);
    opts_free_args(&opts);
    return exit_code;
}
