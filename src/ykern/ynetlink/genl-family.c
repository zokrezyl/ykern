/*
 * Generic netlink family node — leaf for v1.
 *
 * Carries the four scalar attributes ykern collects from CTRL_ATTR_FAMILY_*:
 * id, version, hdrsize, maxattr, plus the family name. Operations and
 * attribute groups land as children once we wire up CTRL_ATTR_OPS parsing.
 */

#include "genl-overlay.h"
#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/types.h>

#include <stdlib.h>
#include <string.h>

static struct ykern_ycore_text_result fam_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);
    return ykern_ycore_text_from_cstr(f->name);
}

static struct ykern_ycore_text_result fam_get_short_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);
    if (f->overlay && f->overlay->short_desc && f->overlay->short_desc[0]) {
        return ykern_ycore_text_from_cstr(f->overlay->short_desc);
    }
    return ykern_ycore_text_format(
        "id=%u, version=%u, maxattr=%u, ops=%zu, mcast=%zu",
        (unsigned)f->id, (unsigned)f->version, (unsigned)f->maxattr,
        f->op_count, f->mcast_count);
}

static struct ykern_ycore_text_result fam_get_long_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);

    if (f->overlay && f->overlay->long_desc && f->overlay->long_desc[0]) {
        return ykern_ycore_text_format(
            "%s\n\n"
            "  id      = %u\n"
            "  version = %u\n"
            "  hdrsize = %u\n"
            "  maxattr = %u\n"
            "  ops     = %zu\n"
            "  mcast   = %zu",
            f->overlay->long_desc, (unsigned)f->id, (unsigned)f->version,
            (unsigned)f->hdrsize, (unsigned)f->maxattr, f->op_count, f->mcast_count);
    }

    return ykern_ycore_text_format(
        "Generic netlink family **%s**.\n\n"
        "Wire-level facts:\n"
        "  family id = %u  (use as nlmsghdr.nlmsg_type)\n"
        "  version   = %u\n"
        "  hdrsize   = %u  (extra family-specific header bytes after genlmsghdr)\n"
        "  maxattr   = %u  (highest attribute id this family understands)\n"
        "  ops       = %zu  (browse them as children)\n"
        "  mcast     = %zu  (notification streams)\n\n"
        "How to use this family:\n"
        "  ykern browse /netlink/genl/%s/<OP_NAME_OR_CMD-N>\n"
        "      describe a specific operation — accepted args, attribute set.\n"
        "  ykern invoke /netlink/genl/%s/<OP_NAME_OR_CMD-N>\n"
        "      send a real request — no --arg sends NLM_F_DUMP and the\n"
        "      kernel walks every object this op covers.\n"
        "  ykern invoke /netlink/genl/%s/<OP> --arg KEY=VALUE [--arg ...]\n"
        "      send a real request targeting a specific object.",
        f->name, (unsigned)f->id, (unsigned)f->version, (unsigned)f->hdrsize,
        (unsigned)f->maxattr, f->op_count, f->mcast_count,
        f->name, f->name, f->name);
}

static struct ykern_ycore_void_result fam_populate_children(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);

    for (size_t i = 0; i < f->op_count; i++) {
        const struct ykern_ynetlink_genl_op_overlay *ov =
            ykern_ynetlink_genl_family_overlay_find_op(f->overlay, f->ops[i].id);

        struct ykern_ynetlink_genl_operation_ptr_result opr =
            ykern_ynetlink_genl_operation_create(
                f->ops[i].id, f->ops[i].flags,
                ov ? ov->name : NULL,
                ov ? ov->short_desc : NULL,
                ov ? ov->long_desc : NULL);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, opr,
                            "fam_populate_children: op create failed");
        struct ykern_ycore_void_result ar =
            ykern_ycore_object_append_child(self, &opr.value->base);
        if (YKERN_IS_ERR(ar)) {
            ykern_ycore_object_destroy(&opr.value->base);
            return YKERN_ERR(ykern_ycore_void, "fam_populate_children: append op failed", ar);
        }
    }

    for (size_t i = 0; i < f->mcast_count; i++) {
        const struct ykern_ynetlink_genl_mcast_overlay *ov =
            ykern_ynetlink_genl_family_overlay_find_mcast(f->overlay,
                                                          f->mcast_groups[i].name);

        struct ykern_ynetlink_genl_mcast_group_ptr_result mr =
            ykern_ynetlink_genl_mcast_group_create(
                f->mcast_groups[i].id, f->mcast_groups[i].name,
                ov ? ov->short_desc : NULL,
                ov ? ov->long_desc : NULL);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, mr,
                            "fam_populate_children: mcast group create failed");
        struct ykern_ycore_void_result ar =
            ykern_ycore_object_append_child(self, &mr.value->base);
        if (YKERN_IS_ERR(ar)) {
            ykern_ycore_object_destroy(&mr.value->base);
            return YKERN_ERR(ykern_ycore_void,
                             "fam_populate_children: append mcast group failed", ar);
        }
    }

    return YKERN_OK_VOID();
}

static void fam_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);
    free(f->name);
    f->name = NULL;

    free(f->ops);
    f->ops = NULL;
    f->op_count = 0;

    if (f->mcast_groups) {
        for (size_t i = 0; i < f->mcast_count; i++) {
            free(f->mcast_groups[i].name);
        }
        free(f->mcast_groups);
        f->mcast_groups = NULL;
        f->mcast_count = 0;
    }

    ykern_ynetlink_genl_family_overlay_destroy(f->overlay);
    f->overlay = NULL;
}

void ykern_ynetlink_genl_family_set_overlay(struct ykern_ynetlink_genl_family *f,
                                            struct ykern_ynetlink_genl_family_overlay *overlay)
{
    if (!f) {
        ykern_ynetlink_genl_family_overlay_destroy(overlay);
        return;
    }
    if (f->overlay) {
        ykern_ynetlink_genl_family_overlay_destroy(f->overlay);
    }
    f->overlay = overlay;
}

void ykern_ynetlink_genl_family_set_ops(struct ykern_ynetlink_genl_family *f,
                                        struct ykern_ynetlink_genl_op_info *ops, size_t count)
{
    if (!f) {
        free(ops);
        return;
    }
    free(f->ops);
    f->ops = ops;
    f->op_count = count;
}

void ykern_ynetlink_genl_family_set_mcast_groups(
    struct ykern_ynetlink_genl_family *f, struct ykern_ynetlink_genl_mcast_info *groups,
    size_t count)
{
    if (!f) {
        if (groups) {
            for (size_t i = 0; i < count; i++) {
                free(groups[i].name);
            }
            free(groups);
        }
        return;
    }
    if (f->mcast_groups) {
        for (size_t i = 0; i < f->mcast_count; i++) {
            free(f->mcast_groups[i].name);
        }
        free(f->mcast_groups);
    }
    f->mcast_groups = groups;
    f->mcast_count = count;
}

static const struct ykern_ycore_object_ops fam_ops = {
    .get_name = fam_get_name,
    .get_short_description = fam_get_short_description,
    .get_long_description = fam_get_long_description,
    .populate_children = fam_populate_children,
    .destroy_impl = fam_destroy_impl,
};

struct ykern_ynetlink_genl_family_ptr_result ykern_ynetlink_genl_family_create(
    const char *name, uint16_t id, uint32_t version, uint32_t hdrsize, uint32_t maxattr)
{
    if (!name) {
        return YKERN_ERR(ykern_ynetlink_genl_family_ptr,
                         "genl_family_create: name is NULL");
    }
    struct ykern_ynetlink_genl_family *f = calloc(1, sizeof(*f));
    if (!f) {
        return YKERN_ERR(ykern_ynetlink_genl_family_ptr, "genl_family_create: calloc failed");
    }
    f->base.ops = &fam_ops;
    f->base.kind = YKERN_YCORE_OBJECT_KIND_FAMILY;
    f->name = strdup(name);
    if (!f->name) {
        free(f);
        return YKERN_ERR(ykern_ynetlink_genl_family_ptr, "genl_family_create: strdup failed");
    }
    f->id = id;
    f->version = version;
    f->hdrsize = hdrsize;
    f->maxattr = maxattr;
    return YKERN_OK(ykern_ynetlink_genl_family_ptr, f);
}
