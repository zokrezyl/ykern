/*
 * Generic netlink multicast group node — one group a family broadcasts on.
 *
 * Unlike operations, multicast groups carry a kernel-assigned name string
 * (CTRL_ATTR_MCAST_GRP_NAME). When that's missing we fall back to a
 * synthetic "group-<id>" name. YAML overlay can attach short / long
 * descriptions keyed by the kernel-supplied name.
 */

#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/types.h>

#include <stdlib.h>
#include <string.h>

static struct ykern_ycore_text_result mcg_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_mcast_group *g =
        ykern_container_of(self, struct ykern_ynetlink_genl_mcast_group, base);
    if (g->name && g->name[0] != '\0') {
        return ykern_ycore_text_from_cstr(g->name);
    }
    return ykern_ycore_text_format("group-%u", (unsigned)g->group_id);
}

static struct ykern_ycore_text_result mcg_get_short_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_mcast_group *g =
        ykern_container_of(self, struct ykern_ynetlink_genl_mcast_group, base);
    if (g->overlay_short && g->overlay_short[0] != '\0') {
        return ykern_ycore_text_from_cstr(g->overlay_short);
    }
    return ykern_ycore_text_format("multicast group: id=%u%s%s%s",
                                   (unsigned)g->group_id,
                                   (g->name && g->name[0]) ? ", name='" : "",
                                   (g->name && g->name[0]) ? g->name : "",
                                   (g->name && g->name[0]) ? "'" : "");
}

static struct ykern_ycore_text_result mcg_get_long_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_mcast_group *g =
        ykern_container_of(self, struct ykern_ynetlink_genl_mcast_group, base);

    if (g->overlay_long && g->overlay_long[0] != '\0') {
        return ykern_ycore_text_format(
            "%s\n\n"
            "  group_id = %u%s%s%s",
            g->overlay_long, (unsigned)g->group_id,
            (g->name && g->name[0]) ? ", name='" : "",
            (g->name && g->name[0]) ? g->name : "",
            (g->name && g->name[0]) ? "'" : "");
    }

    return ykern_ycore_text_format(
        "Generic-netlink multicast group (id=%u%s%s%s).\n\n"
        "Subscribers receive notifications by joining this group on a "
        "NETLINK_GENERIC socket via setsockopt(NETLINK_ADD_MEMBERSHIP). The "
        "id is global across the netlink generic protocol, not per-family.",
        (unsigned)g->group_id,
        (g->name && g->name[0]) ? ", name='" : "",
        (g->name && g->name[0]) ? g->name : "",
        (g->name && g->name[0]) ? "'" : "");
}

static struct ykern_ycore_void_result mcg_populate_children(struct ykern_ycore_object *self)
{
    (void)self;
    return YKERN_OK_VOID();
}

static void mcg_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_mcast_group *g =
        ykern_container_of(self, struct ykern_ynetlink_genl_mcast_group, base);
    free(g->name);
    free(g->overlay_short);
    free(g->overlay_long);
    g->name = NULL;
    g->overlay_short = NULL;
    g->overlay_long = NULL;
}

static const struct ykern_ycore_object_ops mcg_ops = {
    .get_name = mcg_get_name,
    .get_short_description = mcg_get_short_description,
    .get_long_description = mcg_get_long_description,
    .populate_children = mcg_populate_children,
    .destroy_impl = mcg_destroy_impl,
};

static int dup_optional(const char *src, char **dst)
{
    if (!src) {
        *dst = NULL;
        return 1;
    }
    *dst = strdup(src);
    return *dst != NULL;
}

struct ykern_ynetlink_genl_mcast_group_ptr_result ykern_ynetlink_genl_mcast_group_create(
    uint32_t group_id, const char *name, const char *overlay_short, const char *overlay_long)
{
    struct ykern_ynetlink_genl_mcast_group *g = calloc(1, sizeof(*g));
    if (!g) {
        return YKERN_ERR(ykern_ynetlink_genl_mcast_group_ptr,
                         "genl_mcast_group_create: calloc failed");
    }
    g->base.ops = &mcg_ops;
    g->base.kind = YKERN_YCORE_OBJECT_KIND_MCAST_GROUP;
    g->group_id = group_id;
    if (name && name[0] != '\0') {
        g->name = strdup(name);
        if (!g->name) {
            free(g);
            return YKERN_ERR(ykern_ynetlink_genl_mcast_group_ptr,
                             "genl_mcast_group_create: strdup name failed");
        }
    }
    if (!dup_optional(overlay_short, &g->overlay_short) ||
        !dup_optional(overlay_long, &g->overlay_long)) {
        free(g->name);
        free(g->overlay_short);
        free(g->overlay_long);
        free(g);
        return YKERN_ERR(ykern_ynetlink_genl_mcast_group_ptr,
                         "genl_mcast_group_create: overlay strdup failed");
    }
    return YKERN_OK(ykern_ynetlink_genl_mcast_group_ptr, g);
}
