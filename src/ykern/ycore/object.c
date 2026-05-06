/*
 * Base polymorphic object — public API funnels through the vtable; the base
 * also handles lazy children cache, parent-walking path computation, and
 * destruction order (children first, then impl, then base struct).
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/path.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>

#include <stdlib.h>
#include <string.h>

const char *ykern_ycore_object_kind_name(enum ykern_ycore_object_kind kind)
{
    switch (kind) {
    case YKERN_YCORE_OBJECT_KIND_ROOT: return "root";
    case YKERN_YCORE_OBJECT_KIND_TRANSPORT: return "transport";
    case YKERN_YCORE_OBJECT_KIND_NAMESPACE: return "namespace";
    case YKERN_YCORE_OBJECT_KIND_FAMILY: return "family";
    case YKERN_YCORE_OBJECT_KIND_OPERATION: return "operation";
    case YKERN_YCORE_OBJECT_KIND_ATTRIBUTE: return "attribute";
    case YKERN_YCORE_OBJECT_KIND_VALUE: return "value";
    }
    return "?";
}

enum ykern_ycore_object_kind ykern_ycore_object_get_kind(struct ykern_ycore_object *self)
{
    return self ? self->kind : YKERN_YCORE_OBJECT_KIND_ROOT;
}

struct ykern_ycore_text_result ykern_ycore_object_get_name(struct ykern_ycore_object *self)
{
    if (!self || !self->ops || !self->ops->get_name) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_object_get_name: missing vtable");
    }
    return self->ops->get_name(self);
}

struct ykern_ycore_text_result ykern_ycore_object_get_short_description(
    struct ykern_ycore_object *self)
{
    if (!self || !self->ops || !self->ops->get_short_description) {
        return YKERN_ERR(ykern_ycore_text,
                         "ykern_ycore_object_get_short_description: missing vtable");
    }
    return self->ops->get_short_description(self);
}

struct ykern_ycore_text_result ykern_ycore_object_get_long_description(
    struct ykern_ycore_object *self)
{
    if (!self || !self->ops || !self->ops->get_long_description) {
        return YKERN_ERR(ykern_ycore_text,
                         "ykern_ycore_object_get_long_description: missing vtable");
    }
    return self->ops->get_long_description(self);
}

/*-----------------------------------------------------------------------------
 * Path — walk parent chain bottom-up, build segments[], then join top-down.
 *---------------------------------------------------------------------------*/
struct ykern_ycore_text_result ykern_ycore_object_get_path(struct ykern_ycore_object *self)
{
    if (!self) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_object_get_path: self is NULL");
    }

    /* Count depth */
    size_t depth = 0;
    for (struct ykern_ycore_object *p = self; p && p->kind != YKERN_YCORE_OBJECT_KIND_ROOT;
         p = p->parent) {
        depth++;
    }

    if (depth == 0) {
        /* root or detached root-kind */
        return ykern_ycore_text_from_cstr("/");
    }

    char **segments = calloc(depth, sizeof(*segments));
    if (!segments) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_object_get_path: calloc failed");
    }

    /* Fill segments[depth-1 .. 0] going up the chain */
    size_t idx = depth;
    for (struct ykern_ycore_object *p = self; p && p->kind != YKERN_YCORE_OBJECT_KIND_ROOT;
         p = p->parent) {
        struct ykern_ycore_text_result nr = ykern_ycore_object_get_name(p);
        if (YKERN_IS_ERR(nr)) {
            for (size_t i = idx; i < depth; i++) {
                free(segments[i]);
            }
            free(segments);
            return YKERN_ERR(ykern_ycore_text, "ykern_ycore_object_get_path: get_name failed", nr);
        }
        idx--;
        segments[idx] = nr.value.data; /* take ownership */
    }

    /* Compute total size: depth slashes + sum of segment lengths + NUL */
    size_t total = 1; /* trailing NUL */
    for (size_t i = 0; i < depth; i++) {
        total += 1 + strlen(segments[i]);
    }

    char *buf = malloc(total);
    if (!buf) {
        for (size_t i = 0; i < depth; i++) {
            free(segments[i]);
        }
        free(segments);
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_object_get_path: malloc failed");
    }

    size_t off = 0;
    for (size_t i = 0; i < depth; i++) {
        buf[off++] = '/';
        size_t slen = strlen(segments[i]);
        memcpy(buf + off, segments[i], slen);
        off += slen;
        free(segments[i]);
    }
    buf[off] = '\0';
    free(segments);

    return YKERN_OK(ykern_ycore_text, ((struct ykern_ycore_text){.data = buf, .size = off}));
}

/*-----------------------------------------------------------------------------
 * Children — lazy populate, view-only return.
 *---------------------------------------------------------------------------*/
struct ykern_ycore_object_list_result ykern_ycore_object_get_children(
    struct ykern_ycore_object *self)
{
    if (!self) {
        return YKERN_ERR(ykern_ycore_object_list,
                         "ykern_ycore_object_get_children: self is NULL");
    }
    if (!self->children_populated) {
        if (self->ops && self->ops->populate_children) {
            struct ykern_ycore_void_result r = self->ops->populate_children(self);
            YKERN_RETURN_IF_ERR(ykern_ycore_object_list, r,
                                "ykern_ycore_object_get_children: populate failed");
        }
        self->children_populated = true;
    }
    struct ykern_ycore_object_list view = {
        .items = self->children,
        .count = self->children_count,
    };
    return YKERN_OK(ykern_ycore_object_list, view);
}

struct ykern_ycore_void_result ykern_ycore_object_refresh(struct ykern_ycore_object *self)
{
    if (!self) {
        return YKERN_ERR(ykern_ycore_void, "ykern_ycore_object_refresh: self is NULL");
    }
    ykern_ycore_object_drop_children(self);
    self->children_populated = false;
    return YKERN_OK_VOID();
}

/*-----------------------------------------------------------------------------
 * Lifecycle — children destroyed first, then impl-specific cleanup, then self.
 *---------------------------------------------------------------------------*/
void ykern_ycore_object_destroy(struct ykern_ycore_object *self)
{
    if (!self) {
        return;
    }
    ykern_ycore_object_drop_children(self);
    if (self->ops && self->ops->destroy_impl) {
        self->ops->destroy_impl(self);
    }
    free(self);
}

void ykern_ycore_object_drop_children(struct ykern_ycore_object *self)
{
    if (!self) {
        return;
    }
    for (size_t i = 0; i < self->children_count; i++) {
        ykern_ycore_object_destroy(self->children[i]);
    }
    free(self->children);
    self->children = NULL;
    self->children_count = 0;
    self->children_capacity = 0;
}

struct ykern_ycore_void_result ykern_ycore_object_append_child(struct ykern_ycore_object *self,
                                                               struct ykern_ycore_object *child)
{
    if (!self) {
        return YKERN_ERR(ykern_ycore_void, "ykern_ycore_object_append_child: self is NULL");
    }
    if (!child) {
        return YKERN_ERR(ykern_ycore_void, "ykern_ycore_object_append_child: child is NULL");
    }
    if (self->children_count == self->children_capacity) {
        size_t new_cap = self->children_capacity ? self->children_capacity * 2 : 8;
        struct ykern_ycore_object **grown =
            realloc(self->children, new_cap * sizeof(*self->children));
        if (!grown) {
            return YKERN_ERR(ykern_ycore_void, "ykern_ycore_object_append_child: realloc failed");
        }
        self->children = grown;
        self->children_capacity = new_cap;
    }
    child->parent = self;
    self->children[self->children_count++] = child;
    return YKERN_OK_VOID();
}
