/*
 * Generic netlink family node — leaf for v1.
 *
 * Carries the four scalar attributes ykern collects from CTRL_ATTR_FAMILY_*:
 * id, version, hdrsize, maxattr, plus the family name. Operations and
 * attribute groups land as children once we wire up CTRL_ATTR_OPS parsing.
 */

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
    return ykern_ycore_text_format(
        "generic netlink family '%s' (id=%u, version=%u, maxattr=%u)",
        f->name, (unsigned)f->id, (unsigned)f->version, (unsigned)f->maxattr);
}

static struct ykern_ycore_text_result fam_get_long_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);
    return ykern_ycore_text_format(
        "Generic netlink family **%s**.\n\n"
        "  id      = %u  (numeric type for nlmsghdr.nlmsg_type)\n"
        "  version = %u\n"
        "  hdrsize = %u  (extra family-specific header bytes after genlmsghdr)\n"
        "  maxattr = %u  (highest attribute id this family understands)\n\n"
        "Operations, attribute schemas, and multicast groups will appear as "
        "children once ykern parses CTRL_ATTR_OPS / CTRL_ATTR_POLICY.",
        f->name, (unsigned)f->id, (unsigned)f->version, (unsigned)f->hdrsize,
        (unsigned)f->maxattr);
}

static struct ykern_ycore_void_result fam_populate_children(struct ykern_ycore_object *self)
{
    /* Leaf for v1. CTRL_ATTR_OPS / CTRL_ATTR_POLICY parsing happens here later. */
    (void)self;
    return YKERN_OK_VOID();
}

static void fam_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_family *f =
        ykern_container_of(self, struct ykern_ynetlink_genl_family, base);
    free(f->name);
    f->name = NULL;
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
