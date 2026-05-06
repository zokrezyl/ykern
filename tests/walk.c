/*
 * walk — minimal smoke test that exercises the entire ykern stack:
 *
 *   create netlink transport -> wrap into a synthetic root -> recurse
 *
 * For every visited object we print its path, kind, name, and short
 * description. Useful to confirm wiring end-to-end and to eyeball what
 * generic netlink reports on the current kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/root.h>
#include <ykern/ycore/text.h>
#include <ykern/ynetlink/ynetlink.h>

static void indent(int depth)
{
    for (int i = 0; i < depth; i++) {
        fputs("  ", stdout);
    }
}

static int walk(struct ykern_ycore_object *obj, int depth)
{
    struct ykern_ycore_text_result path_r = ykern_ycore_object_get_path(obj);
    struct ykern_ycore_text_result name_r = ykern_ycore_object_get_name(obj);
    struct ykern_ycore_text_result desc_r = ykern_ycore_object_get_short_description(obj);

    if (YKERN_IS_ERR(path_r) || YKERN_IS_ERR(name_r) || YKERN_IS_ERR(desc_r)) {
        fprintf(stderr, "walk: getter failed at depth %d\n", depth);
        if (YKERN_IS_OK(path_r)) ykern_ycore_text_destroy(&path_r.value);
        if (YKERN_IS_OK(name_r)) ykern_ycore_text_destroy(&name_r.value);
        if (YKERN_IS_OK(desc_r)) ykern_ycore_text_destroy(&desc_r.value);
        return 1;
    }

    indent(depth);
    printf("[%s] %s\n", ykern_ycore_object_kind_name(ykern_ycore_object_get_kind(obj)),
           path_r.value.size > 0 ? path_r.value.data : "/");
    indent(depth);
    printf("    name : %s\n", name_r.value.size ? name_r.value.data : "(empty)");
    indent(depth);
    printf("    desc : %s\n", desc_r.value.data);

    ykern_ycore_text_destroy(&path_r.value);
    ykern_ycore_text_destroy(&name_r.value);
    ykern_ycore_text_destroy(&desc_r.value);

    struct ykern_ycore_object_list_result kids_r = ykern_ycore_object_get_children(obj);
    if (YKERN_IS_ERR(kids_r)) {
        indent(depth);
        printf("    (children failed: %s)\n",
               kids_r.error.msg ? kids_r.error.msg : "?");
        ykern_ycore_error_destroy(kids_r.error);
        return 0; /* don't bail the whole walk */
    }
    for (size_t i = 0; i < kids_r.value.count; i++) {
        if (walk(kids_r.value.items[i], depth + 1) != 0) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    struct ykern_ynetlink_ynetlink_ptr_result nl_r = ykern_ynetlink_ynetlink_create();
    if (YKERN_IS_ERR(nl_r)) {
        fprintf(stderr, "ynetlink create failed: %s\n",
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
        fprintf(stderr, "root create failed: %s\n",
                root_r.error.msg ? root_r.error.msg : "?");
        ykern_ycore_error_destroy(root_r.error);
        ykern_ycore_object_destroy(transports[0]);
        return 1;
    }

    int rc = walk(root_r.value, 0);
    ykern_ycore_object_destroy(root_r.value);
    return rc;
}
