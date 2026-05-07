/*
 * Generic netlink attribute node — child of an operation, describes one
 * attribute the user can pass to `--arg <NAME>=...` or expect back in the
 * response.  No per-attribute type info yet (that needs CTRL_CMD_GETPOLICY
 * or a hand-curated overlay).
 */

#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/types.h>

#include <stdlib.h>
#include <string.h>

static struct ykern_ycore_text_result attr_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_attribute *a =
        ykern_container_of(self, struct ykern_ynetlink_genl_attribute, base);
    return ykern_ycore_text_from_cstr(a->name ? a->name : "?");
}

static struct ykern_ycore_text_result attr_get_short_description(
    struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_attribute *a =
        ykern_container_of(self, struct ykern_ynetlink_genl_attribute, base);
    if (a->type_hint && a->type_hint[0]) {
        return ykern_ycore_text_format("%s — id=%u, set=%s",
                                       a->type_hint, (unsigned)a->attr_id,
                                       a->set_name ? a->set_name : "?");
    }
    return ykern_ycore_text_format("id=%u, set=%s (no type info)",
                                   (unsigned)a->attr_id,
                                   a->set_name ? a->set_name : "?");
}

static struct ykern_ycore_text_result attr_get_long_description(
    struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_attribute *a =
        ykern_container_of(self, struct ykern_ynetlink_genl_attribute, base);
    const char *th = a->type_hint;
    if (th && th[0]) {
        return ykern_ycore_text_format(
            "Attribute **%s** (id %u) of attribute set **%s**.\n\n"
            "Type:  %s\n"
            "  (extracted from the kernel UAPI header's inline comment)\n\n"
            "Common type meanings:\n"
            "  u8 / u16 / u32 / u64  numeric, that many bits\n"
            "  string                NUL-terminated string\n"
            "  binary                opaque blob (often a struct or address)\n"
            "  flag                  presence-only, no payload\n"
            "  nest                  this attribute *contains* other attrs;\n"
            "                        you can't pass a single value for it —\n"
            "                        target the inner attrs instead.\n\n"
            "Pass via:  --arg %s=<value>\n"
            "Numeric VALUE is currently packed as u32 regardless; anything\n"
            "else as a NUL-terminated string. The kernel rejects with errno\n"
            "if that doesn't match the expected type.",
            a->name ? a->name : "?", (unsigned)a->attr_id,
            a->set_name ? a->set_name : "?", th, a->name ? a->name : "?");
    }
    return ykern_ycore_text_format(
        "Attribute **%s** (id %u) of attribute set **%s**.\n\n"
        "The kernel header doesn't ship an inline /* type */ comment for\n"
        "this attribute, so ykern can't tell you what shape it expects.\n\n"
        "Pass via:  --arg %s=<value>\n"
        "Numeric VALUE is packed as u32; anything else as string. The\n"
        "kernel rejects with errno if that's wrong.",
        a->name ? a->name : "?", (unsigned)a->attr_id,
        a->set_name ? a->set_name : "?", a->name ? a->name : "?");
}

static struct ykern_ycore_void_result attr_populate_children(struct ykern_ycore_object *self)
{
    (void)self;
    return YKERN_OK_VOID();
}

static void attr_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_attribute *a =
        ykern_container_of(self, struct ykern_ynetlink_genl_attribute, base);
    free(a->name);
    free(a->set_name);
    free(a->type_hint);
    a->name = NULL;
    a->set_name = NULL;
    a->type_hint = NULL;
}

static const struct ykern_ycore_object_ops attr_ops = {
    .get_name = attr_get_name,
    .get_short_description = attr_get_short_description,
    .get_long_description = attr_get_long_description,
    .populate_children = attr_populate_children,
    .destroy_impl = attr_destroy_impl,
};

struct ykern_ynetlink_genl_attribute_ptr_result ykern_ynetlink_genl_attribute_create(
    uint32_t attr_id, const char *name, const char *set_name, const char *type_hint)
{
    struct ykern_ynetlink_genl_attribute *a = calloc(1, sizeof(*a));
    if (!a) {
        return YKERN_ERR(ykern_ynetlink_genl_attribute_ptr,
                         "genl_attribute_create: calloc failed");
    }
    a->base.ops = &attr_ops;
    a->base.kind = YKERN_YCORE_OBJECT_KIND_ATTRIBUTE;
    a->attr_id = attr_id;
    if (name) {
        a->name = strdup(name);
        if (!a->name) {
            free(a);
            return YKERN_ERR(ykern_ynetlink_genl_attribute_ptr,
                             "genl_attribute_create: strdup name failed");
        }
    }
    if (set_name) {
        a->set_name = strdup(set_name);
        if (!a->set_name) {
            free(a->name);
            free(a);
            return YKERN_ERR(ykern_ynetlink_genl_attribute_ptr,
                             "genl_attribute_create: strdup set_name failed");
        }
    }
    if (type_hint) {
        a->type_hint = strdup(type_hint);
        if (!a->type_hint) {
            free(a->name);
            free(a->set_name);
            free(a);
            return YKERN_ERR(ykern_ynetlink_genl_attribute_ptr,
                             "genl_attribute_create: strdup type_hint failed");
        }
    }
    return YKERN_OK(ykern_ynetlink_genl_attribute_ptr, a);
}
