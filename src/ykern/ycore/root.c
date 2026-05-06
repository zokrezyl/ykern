/*
 * Synthetic root object — owns a list of transports passed in at create time
 * and exposes them as its children. The root has no transport-native path of
 * its own; its name is empty and its path resolves to "/".
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/root.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/types.h>

#include <stdlib.h>
#include <string.h>

struct ykern_ycore_root {
    struct ykern_ycore_object base;

    /* Pending transports — moved into base.children on first populate; freed
     * (with the transports themselves) if destroy fires before populate ran. */
    struct ykern_ycore_object **pending;
    size_t pending_count;
};

static struct ykern_ycore_text_result root_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("");
}

static struct ykern_ycore_text_result root_get_short_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("Linux kernel — every transport ykern can reach");
}

static struct ykern_ycore_text_result root_get_long_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "The synthetic root of the kernel-object tree. Each child is a "
        "transport handler (netlink, /proc, /sys, ...). Browse a child to "
        "explore the kernel surface that transport exposes.");
}

static struct ykern_ycore_void_result root_populate_children(struct ykern_ycore_object *self)
{
    struct ykern_ycore_root *root = ykern_container_of(self, struct ykern_ycore_root, base);

    for (size_t i = 0; i < root->pending_count; i++) {
        struct ykern_ycore_void_result r =
            ykern_ycore_object_append_child(self, root->pending[i]);
        if (YKERN_IS_ERR(r)) {
            /* On partial failure, surrender the rest of the pending transports
             * to the destroy path so we don't leak. */
            for (size_t j = i; j < root->pending_count; j++) {
                ykern_ycore_object_destroy(root->pending[j]);
            }
            free(root->pending);
            root->pending = NULL;
            root->pending_count = 0;
            return YKERN_ERR(ykern_ycore_void, "root_populate_children: append failed", r);
        }
    }
    free(root->pending);
    root->pending = NULL;
    root->pending_count = 0;
    return YKERN_OK_VOID();
}

static void root_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ycore_root *root = ykern_container_of(self, struct ykern_ycore_root, base);
    /* If populate never ran, the root still owns the pending transports. */
    if (root->pending) {
        for (size_t i = 0; i < root->pending_count; i++) {
            ykern_ycore_object_destroy(root->pending[i]);
        }
        free(root->pending);
        root->pending = NULL;
        root->pending_count = 0;
    }
}

static const struct ykern_ycore_object_ops root_ops = {
    .get_name = root_get_name,
    .get_short_description = root_get_short_description,
    .get_long_description = root_get_long_description,
    .populate_children = root_populate_children,
    .destroy_impl = root_destroy_impl,
};

struct ykern_ycore_object_ptr_result ykern_ycore_root_create(
    struct ykern_ycore_object **transports, size_t transport_count)
{
    struct ykern_ycore_root *root = calloc(1, sizeof(*root));
    if (!root) {
        return YKERN_ERR(ykern_ycore_object_ptr, "ykern_ycore_root_create: calloc failed");
    }
    root->base.ops = &root_ops;
    root->base.kind = YKERN_YCORE_OBJECT_KIND_ROOT;

    if (transport_count > 0) {
        root->pending = calloc(transport_count, sizeof(*root->pending));
        if (!root->pending) {
            free(root);
            return YKERN_ERR(ykern_ycore_object_ptr,
                             "ykern_ycore_root_create: pending alloc failed");
        }
        memcpy(root->pending, transports, transport_count * sizeof(*transports));
        root->pending_count = transport_count;
    }
    return YKERN_OK(ykern_ycore_object_ptr, &root->base);
}
