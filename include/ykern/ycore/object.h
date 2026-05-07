#ifndef YKERN_YCORE_OBJECT_H
#define YKERN_YCORE_OBJECT_H

/*
 * struct ykern_ycore_object — base polymorphic kernel-object node.
 *
 * Every node in the kernel tree (transports, namespaces, families, ops,
 * attributes, leaf values) embeds this base as its first member. Concrete
 * implementations install a vtable, then are upcast/downcast with
 * ykern_container_of.
 *
 * Identity & description are vtable-driven and always return owned text:
 *   - get_name           — short identifier ("nl80211", "cmd-get-station")
 *   - get_path           — absolute path beginning with the transport name
 *   - get_short_description — single-line summary
 *   - get_long_description  — multi-line help (markdown OK)
 *
 * Tree navigation:
 *   - get_children — lazy; populates on first call, cached on the object
 *   - refresh      — invalidates the cache
 *
 * Lifecycle:
 *   - destroy — frees impl resources, then base helper frees children + self
 */

#include <stdbool.h>
#include <stddef.h>

#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ykern_ycore_object_kind {
    YKERN_YCORE_OBJECT_KIND_ROOT,
    YKERN_YCORE_OBJECT_KIND_TRANSPORT,
    YKERN_YCORE_OBJECT_KIND_NAMESPACE,
    YKERN_YCORE_OBJECT_KIND_FAMILY,
    YKERN_YCORE_OBJECT_KIND_OPERATION,
    YKERN_YCORE_OBJECT_KIND_MCAST_GROUP,
    YKERN_YCORE_OBJECT_KIND_ATTRIBUTE,
    YKERN_YCORE_OBJECT_KIND_VALUE,
};

const char *ykern_ycore_object_kind_name(enum ykern_ycore_object_kind kind);

struct ykern_ycore_object;

YKERN_YRESULT_DECLARE(ykern_ycore_object_ptr, struct ykern_ycore_object *);

struct ykern_ycore_object_list {
    struct ykern_ycore_object **items; /* not owned by the list */
    size_t count;
};

YKERN_YRESULT_DECLARE(ykern_ycore_object_list, struct ykern_ycore_object_list);

/*-----------------------------------------------------------------------------
 * Invoke arguments — passed to ykern_ycore_object_invoke().
 *
 * Strings only at this layer; the receiving op interprets them according to
 * its own typed schema (an op that wants a u32 parses the string as one and
 * errors if it can't).  Both pointers are non-owning — the caller keeps the
 * underlying buffers alive for the duration of the invoke call.
 *---------------------------------------------------------------------------*/
struct ykern_ycore_invoke_arg {
    const char *key;
    const char *value;
};

struct ykern_ycore_invoke_args {
    const struct ykern_ycore_invoke_arg *entries;
    size_t count;
};

const char *ykern_ycore_invoke_args_lookup(const struct ykern_ycore_invoke_args *args,
                                           const char *key);

/*=============================================================================
 * Vtable
 *
 * All getters return owned text. populate_children fills the cache the first
 * time get_children is called; the base layer manages caching, so the impl
 * only needs to know how to produce its children. destroy frees impl-specific
 * resources only — the base destroy walks children and frees the base itself
 * via the impl_free hook (free / custom).
 *===========================================================================*/
struct ykern_ycore_object_ops {
    struct ykern_ycore_text_result (*get_name)(struct ykern_ycore_object *self);
    struct ykern_ycore_text_result (*get_short_description)(struct ykern_ycore_object *self);
    struct ykern_ycore_text_result (*get_long_description)(struct ykern_ycore_object *self);

    struct ykern_ycore_void_result (*populate_children)(struct ykern_ycore_object *self);

    /* Invoke this object as an operation. Most node kinds leave this NULL;
     * only OPERATION nodes wire it.  Returns a human-readable text rendering
     * of the kernel's response (or an error result describing why the call
     * could not be made / what the kernel said). */
    struct ykern_ycore_text_result (*invoke)(struct ykern_ycore_object *self,
                                             const struct ykern_ycore_invoke_args *args);

    void (*destroy_impl)(struct ykern_ycore_object *self);
};

struct ykern_ycore_object {
    const struct ykern_ycore_object_ops *ops;
    enum ykern_ycore_object_kind kind;

    struct ykern_ycore_object *parent; /* NULL at root */

    struct ykern_ycore_object **children;
    size_t children_count;
    size_t children_capacity;
    bool children_populated;
};

/*=============================================================================
 * Public API — thin wrappers that funnel through the vtable.
 *===========================================================================*/

struct ykern_ycore_text_result ykern_ycore_object_get_name(struct ykern_ycore_object *self);
struct ykern_ycore_text_result ykern_ycore_object_get_path(struct ykern_ycore_object *self);
struct ykern_ycore_text_result ykern_ycore_object_get_short_description(
    struct ykern_ycore_object *self);
struct ykern_ycore_text_result ykern_ycore_object_get_long_description(
    struct ykern_ycore_object *self);

struct ykern_ycore_object_list_result ykern_ycore_object_get_children(
    struct ykern_ycore_object *self);

struct ykern_ycore_void_result ykern_ycore_object_refresh(struct ykern_ycore_object *self);

struct ykern_ycore_text_result ykern_ycore_object_invoke(
    struct ykern_ycore_object *self, const struct ykern_ycore_invoke_args *args);

void ykern_ycore_object_destroy(struct ykern_ycore_object *self);

enum ykern_ycore_object_kind ykern_ycore_object_get_kind(struct ykern_ycore_object *self);

/*=============================================================================
 * Helpers for impl authors.
 *===========================================================================*/

/* Append a child to self's cache, growing the array as needed. Takes
 * ownership of `child` — base destroy frees it. */
struct ykern_ycore_void_result ykern_ycore_object_append_child(struct ykern_ycore_object *self,
                                                               struct ykern_ycore_object *child);

/* Walk and free the children array (calls destroy on each). Used by impls'
 * destroy_impl as a final cleanup before freeing themselves. */
void ykern_ycore_object_drop_children(struct ykern_ycore_object *self);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_OBJECT_H */
