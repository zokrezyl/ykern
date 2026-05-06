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

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/root.h>
#include <ykern/ycore/text.h>
#include <ykern/ynetlink/ynetlink.h>

#define YKERN_TOOL_VERSION "0.1.0"

struct opts {
    int long_desc;
    int list_only;
    int recurse;
    int verbose_children; /* show short desc per child in listings */
};

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: ykern [OPTIONS] [PATH]\n"
            "\n"
            "Browse the Linux kernel surface ykern exposes. Each node has a name,\n"
            "a path, a short summary, and a long description; this tool prints\n"
            "those, plus the list of paths reachable from the current node.\n"
            "\n"
            "Arguments:\n"
            "  PATH                 Kernel-object path (default \"/\").\n"
            "                       Examples:\n"
            "                         /\n"
            "                         /netlink\n"
            "                         /netlink/genl\n"
            "                         /netlink/genl/nl80211\n"
            "\n"
            "Options:\n"
            "  -l, --long           Print the full long description.\n"
            "  -L, --list-only      Skip object info; print children only.\n"
            "  -r, --recurse        Walk the entire subtree from PATH.\n"
            "  -q, --quiet-children List children by name only (no short desc).\n"
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

static void print_object(struct ykern_ycore_object *obj, int with_long)
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

    printf("[%s] %s\n",
           ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(obj)),
           pr.value.size > 0 ? pr.value.data : "/");
    printf("  name: %s\n", nr.value.size ? nr.value.data : "(empty)");
    printf("  desc: %s\n", sr.value.data);

    if (with_long) {
        struct ykern_ycore_text_result lr = ykern_ycore_object_get_long_description(obj);
        if (YKERN_IS_OK(lr)) {
            printf("\n");
            print_long_indented(lr.value.data);
            ykern_ycore_text_destroy(&lr.value);
        } else {
            ykern_ycore_error_destroy(lr.error);
        }
    }

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

    printf("\nchildren (%zu):\n", kr.value.count);
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
                printf("  [%-9s] %s: %s\n", kind, name, sr.value.data);
                ykern_ycore_text_destroy(&sr.value);
            } else {
                printf("  [%-9s] %s\n", kind, name);
                ykern_ycore_error_destroy(sr.error);
            }
        } else {
            printf("  [%-9s] %s\n", kind, name);
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
    struct opts opts = {
        .long_desc = 0,
        .list_only = 0,
        .recurse = 0,
        .verbose_children = 1,
    };

    static const struct option longopts[] = {
        {"long", no_argument, NULL, 'l'},
        {"list-only", no_argument, NULL, 'L'},
        {"recurse", no_argument, NULL, 'r'},
        {"quiet-children", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "lLrqhV", longopts, NULL)) != -1) {
        switch (c) {
        case 'l': opts.long_desc = 1; break;
        case 'L': opts.list_only = 1; break;
        case 'r': opts.recurse = 1; break;
        case 'q': opts.verbose_children = 0; break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("ykern %s\n", YKERN_TOOL_VERSION); return 0;
        case '?': usage(stderr); return 2;
        default: usage(stderr); return 2;
        }
    }

    const char *path = "/";
    if (optind < argc) {
        path = argv[optind];
        if (optind + 1 < argc) {
            fprintf(stderr, "ykern: too many arguments\n");
            usage(stderr);
            return 2;
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
    struct ykern_ycore_object *transports[] = {
        ykern_ynetlink_ynetlink_as_object(nl_r.value),
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
    } else {
        if (opts.recurse) {
            recurse_subtree(target, opts.long_desc, 0);
        } else if (opts.list_only) {
            print_children(target, opts.verbose_children);
        } else {
            print_object(target, opts.long_desc);
            print_children(target, opts.verbose_children);
        }
        free(missing);
    }

    ykern_ycore_object_destroy(root);
    return exit_code;
}
