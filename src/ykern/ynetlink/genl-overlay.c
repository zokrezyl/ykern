/*
 * libyaml-based loader for per-family YAML overlays.
 *
 * Recursive-descent walker over the libyaml event stream. Strict about
 * structure but lenient about ordering of keys at any level.
 */

#include "genl-overlay.h"

#include <ykern/ycore/result.h>
#include <ykern/ycore/trace.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#ifndef YKERN_METADATA_DIR
#define YKERN_METADATA_DIR "metadata"
#endif

struct overlay_loader {
    yaml_parser_t parser;
    FILE *file;
    struct ykern_ynetlink_genl_family_overlay *out;

    /* Growable working storage. */
    size_t op_capacity;
    size_t mcast_capacity;
    size_t attr_set_capacity;
};

/*-----------------------------------------------------------------------------
 * Small helpers
 *---------------------------------------------------------------------------*/

static char *take_scalar(yaml_event_t *ev)
{
    return strndup((const char *)ev->data.scalar.value, ev->data.scalar.length);
}

static int parse_uint32(const char *s, uint32_t *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static struct ykern_ycore_void_result next_event(struct overlay_loader *l, yaml_event_t *ev)
{
    if (!yaml_parser_parse(&l->parser, ev)) {
        return YKERN_ERR(ykern_ycore_void, "yaml: parse error");
    }
    return YKERN_OK_VOID();
}

static struct ykern_ycore_void_result expect_event_type(struct overlay_loader *l,
                                                        yaml_event_t *ev,
                                                        yaml_event_type_t type)
{
    struct ykern_ycore_void_result r = next_event(l, ev);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: read event");
    if (ev->type != type) {
        yaml_event_delete(ev);
        return YKERN_ERR(ykern_ycore_void, "yaml: unexpected event type");
    }
    return YKERN_OK_VOID();
}

/*-----------------------------------------------------------------------------
 * Per-op map (inner mapping): { name: ..., short: ..., long: ... }.
 *
 * The same inner shape is used in both file flavours; what differs is the
 * outer key (cmd id vs op name) and which inner fields are expected to be
 * present.  This routine appends an entry to l->out->ops, populating
 * whichever fields the file supplied; the merge step in load_overlay
 * stitches the two halves together.
 *---------------------------------------------------------------------------*/

static struct ykern_ycore_void_result read_op_inner(struct overlay_loader *l,
                                                    uint32_t cmd_id_or_zero,
                                                    char *name_owned)
{
    /* Caller already consumed the value's MAPPING_START.  Takes ownership of
     * `name_owned` if non-NULL (frees it on error path or transfers it). */
    char *name = name_owned;
    char *short_desc = NULL;
    char *long_desc = NULL;

    while (1) {
        yaml_event_t ev;
        struct ykern_ycore_void_result r = next_event(l, &ev);
        if (YKERN_IS_ERR(r)) {
            free(name);
            free(short_desc);
            free(long_desc);
            return r;
        }
        if (ev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&ev);
            break;
        }
        if (ev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&ev);
            free(name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: expected scalar key in op map");
        }
        char *key = take_scalar(&ev);
        yaml_event_delete(&ev);

        yaml_event_t vev;
        r = next_event(l, &vev);
        if (YKERN_IS_ERR(r) || vev.type != YAML_SCALAR_EVENT) {
            free(key);
            free(name);
            free(short_desc);
            free(long_desc);
            if (YKERN_IS_OK(r)) yaml_event_delete(&vev);
            return YKERN_ERR(ykern_ycore_void, "yaml: expected scalar value in op map");
        }
        char *val = take_scalar(&vev);
        yaml_event_delete(&vev);
        if (!key || !val) {
            free(key);
            free(val);
            free(name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: oom in op map");
        }

        if (strcmp(key, "name") == 0) {
            free(name);
            name = val;
            val = NULL;
        } else if (strcmp(key, "short") == 0) {
            free(short_desc);
            short_desc = val;
            val = NULL;
        } else if (strcmp(key, "long") == 0) {
            free(long_desc);
            long_desc = val;
            val = NULL;
        }
        free(key);
        free(val);
    }

    if (l->out->op_count == l->op_capacity) {
        size_t new_cap = l->op_capacity ? l->op_capacity * 2 : 8;
        struct ykern_ynetlink_genl_op_overlay *grown =
            realloc(l->out->ops, new_cap * sizeof(*l->out->ops));
        if (!grown) {
            free(name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: op array realloc failed");
        }
        l->out->ops = grown;
        l->op_capacity = new_cap;
    }
    l->out->ops[l->out->op_count++] = (struct ykern_ynetlink_genl_op_overlay){
        .cmd_id = cmd_id_or_zero,
        .name = name,
        .short_desc = short_desc,
        .long_desc = long_desc,
    };
    return YKERN_OK_VOID();
}

/*-----------------------------------------------------------------------------
 * operations: outer mapping. Two flavours:
 *   - id-keyed   (generated file):  `{ <int>: { name: ... } }`
 *   - name-keyed (curated file):    `{ <NAME>: { short: ..., long: ... } }`
 *
 * The parser detects the key flavour by trying to parse the key as an int.
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result read_operations(struct overlay_loader *l)
{
    yaml_event_t ev;
    struct ykern_ycore_void_result r = expect_event_type(l, &ev, YAML_MAPPING_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: operations expected mapping");
    yaml_event_delete(&ev);

    while (1) {
        yaml_event_t kev;
        r = next_event(l, &kev);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: operations next");
        if (kev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_OK_VOID();
        }
        if (kev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_ERR(ykern_ycore_void, "yaml: operations expected scalar key");
        }
        char *key = take_scalar(&kev);
        yaml_event_delete(&kev);
        if (!key) return YKERN_ERR(ykern_ycore_void, "yaml: oom on op key");

        uint32_t cmd_id = 0;
        char *name_from_key = NULL;
        if (parse_uint32(key, &cmd_id)) {
            /* id-keyed (generated): name comes from the inner map */
            free(key);
        } else {
            /* name-keyed (curated): key is the op name */
            name_from_key = key;  /* transfer ownership */
        }

        yaml_event_t vstart;
        r = expect_event_type(l, &vstart, YAML_MAPPING_START_EVENT);
        if (YKERN_IS_ERR(r)) {
            free(name_from_key);
            return YKERN_ERR(ykern_ycore_void, "yaml: op value expected mapping", r);
        }
        yaml_event_delete(&vstart);

        r = read_op_inner(l, cmd_id, name_from_key);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: read_op_inner failed");
    }
}

/*-----------------------------------------------------------------------------
 * Per-mcast map (similar to per-op but no cmd_id; key is the mcast name)
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result read_mcast_entry(struct overlay_loader *l, char *mcast_name)
{
    char *short_desc = NULL;
    char *long_desc = NULL;

    while (1) {
        yaml_event_t ev;
        struct ykern_ycore_void_result r = next_event(l, &ev);
        if (YKERN_IS_ERR(r)) {
            free(mcast_name);
            free(short_desc);
            free(long_desc);
            return r;
        }
        if (ev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&ev);
            break;
        }
        if (ev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&ev);
            free(mcast_name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: expected scalar in mcast map");
        }
        char *key = take_scalar(&ev);
        yaml_event_delete(&ev);

        yaml_event_t vev;
        r = next_event(l, &vev);
        if (YKERN_IS_ERR(r) || vev.type != YAML_SCALAR_EVENT) {
            free(key);
            free(mcast_name);
            free(short_desc);
            free(long_desc);
            if (YKERN_IS_OK(r)) yaml_event_delete(&vev);
            return YKERN_ERR(ykern_ycore_void, "yaml: expected scalar value in mcast map");
        }
        char *val = take_scalar(&vev);
        yaml_event_delete(&vev);
        if (!key || !val) {
            free(key);
            free(val);
            free(mcast_name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: oom in mcast map");
        }

        if (strcmp(key, "short") == 0) {
            free(short_desc);
            short_desc = val;
            val = NULL;
        } else if (strcmp(key, "long") == 0) {
            free(long_desc);
            long_desc = val;
            val = NULL;
        }
        free(key);
        free(val);
    }

    if (l->out->mcast_count == l->mcast_capacity) {
        size_t new_cap = l->mcast_capacity ? l->mcast_capacity * 2 : 8;
        struct ykern_ynetlink_genl_mcast_overlay *grown =
            realloc(l->out->mcast, new_cap * sizeof(*l->out->mcast));
        if (!grown) {
            free(mcast_name);
            free(short_desc);
            free(long_desc);
            return YKERN_ERR(ykern_ycore_void, "yaml: mcast array realloc failed");
        }
        l->out->mcast = grown;
        l->mcast_capacity = new_cap;
    }
    l->out->mcast[l->out->mcast_count++] = (struct ykern_ynetlink_genl_mcast_overlay){
        .name = mcast_name,
        .short_desc = short_desc,
        .long_desc = long_desc,
    };
    return YKERN_OK_VOID();
}

static struct ykern_ycore_void_result read_mcast_groups(struct overlay_loader *l)
{
    yaml_event_t ev;
    struct ykern_ycore_void_result r = expect_event_type(l, &ev, YAML_MAPPING_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: mcast_groups expected mapping");
    yaml_event_delete(&ev);

    while (1) {
        yaml_event_t kev;
        r = next_event(l, &kev);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: mcast_groups next");
        if (kev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_OK_VOID();
        }
        if (kev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_ERR(ykern_ycore_void, "yaml: mcast_groups expected name key");
        }
        char *name = take_scalar(&kev);
        yaml_event_delete(&kev);
        if (!name) return YKERN_ERR(ykern_ycore_void, "yaml: oom on mcast key");

        yaml_event_t vstart;
        r = expect_event_type(l, &vstart, YAML_MAPPING_START_EVENT);
        if (YKERN_IS_ERR(r)) {
            free(name);
            return YKERN_ERR(ykern_ycore_void, "yaml: mcast value expected mapping", r);
        }
        yaml_event_delete(&vstart);

        r = read_mcast_entry(l, name); /* takes ownership of name */
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: read_mcast_entry failed");
    }
}

/*-----------------------------------------------------------------------------
 * attribute_sets: { <SET>: { <int>: { name: <str> } } }
 *
 * Two levels of nested mappings.  The outer key is the set name (string),
 * the inner key is an attribute id (int), the innermost value is a small
 * map currently holding only `name`.
 *---------------------------------------------------------------------------*/

static struct ykern_ycore_void_result read_attr_entries_into_set(
    struct overlay_loader *l, struct ykern_ynetlink_genl_attr_set_overlay *set)
{
    /* Caller has consumed MAPPING_START for the per-set mapping. */
    size_t cap = 0;

    while (1) {
        yaml_event_t kev;
        struct ykern_ycore_void_result r = next_event(l, &kev);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attr set next");
        if (kev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_OK_VOID();
        }
        if (kev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_ERR(ykern_ycore_void, "yaml: attr set expects int key");
        }
        char *key = take_scalar(&kev);
        yaml_event_delete(&kev);
        if (!key) return YKERN_ERR(ykern_ycore_void, "yaml: oom on attr key");

        uint32_t attr_id = 0;
        if (!parse_uint32(key, &attr_id)) {
            free(key);
            return YKERN_ERR(ykern_ycore_void, "yaml: attr key not an integer");
        }
        free(key);

        yaml_event_t vstart;
        r = expect_event_type(l, &vstart, YAML_MAPPING_START_EVENT);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attr value expected mapping");
        yaml_event_delete(&vstart);

        char *attr_name = NULL;
        char *type_hint = NULL;
        while (1) {
            yaml_event_t ev;
            r = next_event(l, &ev);
            if (YKERN_IS_ERR(r)) {
                free(attr_name);
                free(type_hint);
                return r;
            }
            if (ev.type == YAML_MAPPING_END_EVENT) {
                yaml_event_delete(&ev);
                break;
            }
            if (ev.type != YAML_SCALAR_EVENT) {
                yaml_event_delete(&ev);
                free(attr_name);
                free(type_hint);
                return YKERN_ERR(ykern_ycore_void, "yaml: attr inner expected scalar key");
            }
            char *k = take_scalar(&ev);
            yaml_event_delete(&ev);

            yaml_event_t vev;
            r = next_event(l, &vev);
            if (YKERN_IS_ERR(r) || vev.type != YAML_SCALAR_EVENT) {
                free(k);
                free(attr_name);
                free(type_hint);
                if (YKERN_IS_OK(r)) yaml_event_delete(&vev);
                return YKERN_ERR(ykern_ycore_void, "yaml: attr inner expected scalar value");
            }
            char *v = take_scalar(&vev);
            yaml_event_delete(&vev);

            if (k && v && strcmp(k, "name") == 0) {
                free(attr_name);
                attr_name = v;
                v = NULL;
            } else if (k && v && strcmp(k, "type") == 0) {
                free(type_hint);
                type_hint = v;
                v = NULL;
            }
            free(k);
            free(v);
        }

        /* Append to the set's attrs[]. */
        if (set->attr_count == cap) {
            size_t new_cap = cap ? cap * 2 : 16;
            struct ykern_ynetlink_genl_attr_overlay *grown =
                realloc(set->attrs, new_cap * sizeof(*set->attrs));
            if (!grown) {
                free(attr_name);
                return YKERN_ERR(ykern_ycore_void, "yaml: attrs realloc failed");
            }
            set->attrs = grown;
            cap = new_cap;
        }
        set->attrs[set->attr_count++] = (struct ykern_ynetlink_genl_attr_overlay){
            .attr_id = attr_id,
            .name = attr_name,
            .type_hint = type_hint,
        };
    }
}

static struct ykern_ycore_void_result read_attribute_sets(struct overlay_loader *l)
{
    yaml_event_t ev;
    struct ykern_ycore_void_result r = expect_event_type(l, &ev, YAML_MAPPING_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attribute_sets expected mapping");
    yaml_event_delete(&ev);

    while (1) {
        yaml_event_t kev;
        r = next_event(l, &kev);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attribute_sets next");
        if (kev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_OK_VOID();
        }
        if (kev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_ERR(ykern_ycore_void, "yaml: attribute_sets expects name key");
        }
        char *set_name = take_scalar(&kev);
        yaml_event_delete(&kev);
        if (!set_name) return YKERN_ERR(ykern_ycore_void, "yaml: oom on set key");

        yaml_event_t vstart;
        r = expect_event_type(l, &vstart, YAML_MAPPING_START_EVENT);
        if (YKERN_IS_ERR(r)) {
            free(set_name);
            return YKERN_ERR(ykern_ycore_void, "yaml: set value expected mapping", r);
        }
        yaml_event_delete(&vstart);

        if (l->out->attr_set_count == l->attr_set_capacity) {
            size_t new_cap = l->attr_set_capacity ? l->attr_set_capacity * 2 : 16;
            struct ykern_ynetlink_genl_attr_set_overlay *grown =
                realloc(l->out->attr_sets, new_cap * sizeof(*l->out->attr_sets));
            if (!grown) {
                free(set_name);
                return YKERN_ERR(ykern_ycore_void, "yaml: attr_sets realloc failed");
            }
            l->out->attr_sets = grown;
            l->attr_set_capacity = new_cap;
        }

        struct ykern_ynetlink_genl_attr_set_overlay *slot =
            &l->out->attr_sets[l->out->attr_set_count];
        slot->set_name = set_name; /* transfer ownership */
        slot->attrs = NULL;
        slot->attr_count = 0;
        l->out->attr_set_count++;

        r = read_attr_entries_into_set(l, slot);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attr set entries failed");
    }
}

/*-----------------------------------------------------------------------------
 * Top-level mapping
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result read_top(struct overlay_loader *l)
{
    yaml_event_t ev;
    struct ykern_ycore_void_result r;

    /* STREAM_START */
    r = expect_event_type(l, &ev, YAML_STREAM_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: stream start");
    yaml_event_delete(&ev);

    /* DOCUMENT_START */
    r = expect_event_type(l, &ev, YAML_DOCUMENT_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: document start");
    yaml_event_delete(&ev);

    /* Top mapping */
    r = expect_event_type(l, &ev, YAML_MAPPING_START_EVENT);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: top mapping");
    yaml_event_delete(&ev);

    while (1) {
        yaml_event_t kev;
        r = next_event(l, &kev);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: top map next");
        if (kev.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&kev);
            break;
        }
        if (kev.type != YAML_SCALAR_EVENT) {
            yaml_event_delete(&kev);
            return YKERN_ERR(ykern_ycore_void, "yaml: top map expects scalar key");
        }
        char *key = take_scalar(&kev);
        yaml_event_delete(&kev);
        if (!key) return YKERN_ERR(ykern_ycore_void, "yaml: oom on top key");

        if (strcmp(key, "operations") == 0) {
            free(key);
            r = read_operations(l);
            YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: operations failed");
        } else if (strcmp(key, "mcast_groups") == 0) {
            free(key);
            r = read_mcast_groups(l);
            YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: mcast_groups failed");
        } else if (strcmp(key, "attribute_sets") == 0) {
            free(key);
            r = read_attribute_sets(l);
            YKERN_RETURN_IF_ERR(ykern_ycore_void, r, "yaml: attribute_sets failed");
        } else {
            /* Scalar values: family, short, long, plus tolerated unknowns. */
            yaml_event_t vev;
            r = next_event(l, &vev);
            if (YKERN_IS_ERR(r) || vev.type != YAML_SCALAR_EVENT) {
                free(key);
                if (YKERN_IS_OK(r)) yaml_event_delete(&vev);
                return YKERN_ERR(ykern_ycore_void, "yaml: top map expected scalar value");
            }
            char *val = take_scalar(&vev);
            yaml_event_delete(&vev);
            if (!val) {
                free(key);
                return YKERN_ERR(ykern_ycore_void, "yaml: oom on top val");
            }

            if (strcmp(key, "family") == 0) {
                free(l->out->family_name);
                l->out->family_name = val;
                val = NULL;
            } else if (strcmp(key, "short") == 0) {
                free(l->out->short_desc);
                l->out->short_desc = val;
                val = NULL;
            } else if (strcmp(key, "long") == 0) {
                free(l->out->long_desc);
                l->out->long_desc = val;
                val = NULL;
            } else {
                ywarn("yaml overlay: unknown top-level key '%s', ignoring", key);
            }
            free(key);
            free(val);
        }
    }

    return YKERN_OK_VOID();
}

/*-----------------------------------------------------------------------------
 * Public entry points
 *---------------------------------------------------------------------------*/

void ykern_ynetlink_genl_family_overlay_destroy(
    struct ykern_ynetlink_genl_family_overlay *o)
{
    if (!o) return;
    free(o->family_name);
    free(o->short_desc);
    free(o->long_desc);
    for (size_t i = 0; i < o->op_count; i++) {
        free(o->ops[i].name);
        free(o->ops[i].short_desc);
        free(o->ops[i].long_desc);
    }
    free(o->ops);
    for (size_t i = 0; i < o->mcast_count; i++) {
        free(o->mcast[i].name);
        free(o->mcast[i].short_desc);
        free(o->mcast[i].long_desc);
    }
    free(o->mcast);
    for (size_t i = 0; i < o->attr_set_count; i++) {
        free(o->attr_sets[i].set_name);
        for (size_t j = 0; j < o->attr_sets[i].attr_count; j++) {
            free(o->attr_sets[i].attrs[j].name);
            free(o->attr_sets[i].attrs[j].type_hint);
        }
        free(o->attr_sets[i].attrs);
    }
    free(o->attr_sets);
    free(o);
}

const struct ykern_ynetlink_genl_attr_set_overlay *
ykern_ynetlink_genl_family_overlay_find_attr_set(
    const struct ykern_ynetlink_genl_family_overlay *o, const char *set_name)
{
    if (!o || !set_name) return NULL;
    for (size_t i = 0; i < o->attr_set_count; i++) {
        if (o->attr_sets[i].set_name && strcmp(o->attr_sets[i].set_name, set_name) == 0) {
            return &o->attr_sets[i];
        }
    }
    return NULL;
}

const char *ykern_ynetlink_genl_attr_set_lookup_name(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, uint32_t attr_id)
{
    if (!set) return NULL;
    for (size_t i = 0; i < set->attr_count; i++) {
        if (set->attrs[i].attr_id == attr_id) {
            return set->attrs[i].name;
        }
    }
    return NULL;
}

int ykern_ynetlink_genl_attr_set_lookup_id(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, const char *name,
    uint32_t *out_id)
{
    if (!set || !name) return 0;
    for (size_t i = 0; i < set->attr_count; i++) {
        if (set->attrs[i].name && strcmp(set->attrs[i].name, name) == 0) {
            if (out_id) *out_id = set->attrs[i].attr_id;
            return 1;
        }
    }
    return 0;
}

const char *ykern_ynetlink_genl_attr_set_lookup_type(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, uint32_t attr_id)
{
    if (!set) return NULL;
    for (size_t i = 0; i < set->attr_count; i++) {
        if (set->attrs[i].attr_id == attr_id) {
            return set->attrs[i].type_hint;
        }
    }
    return NULL;
}

const char *ykern_ynetlink_genl_attr_set_lookup_type_by_name(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, const char *name)
{
    if (!set || !name) return NULL;
    for (size_t i = 0; i < set->attr_count; i++) {
        if (set->attrs[i].name && strcmp(set->attrs[i].name, name) == 0) {
            return set->attrs[i].type_hint;
        }
    }
    return NULL;
}

const struct ykern_ynetlink_genl_op_overlay *
ykern_ynetlink_genl_family_overlay_find_op(
    const struct ykern_ynetlink_genl_family_overlay *o, uint32_t cmd_id)
{
    if (!o) return NULL;
    for (size_t i = 0; i < o->op_count; i++) {
        if (o->ops[i].cmd_id == cmd_id) {
            return &o->ops[i];
        }
    }
    return NULL;
}

const struct ykern_ynetlink_genl_mcast_overlay *
ykern_ynetlink_genl_family_overlay_find_mcast(
    const struct ykern_ynetlink_genl_family_overlay *o, const char *name)
{
    if (!o || !name) return NULL;
    for (size_t i = 0; i < o->mcast_count; i++) {
        if (o->mcast[i].name && strcmp(o->mcast[i].name, name) == 0) {
            return &o->mcast[i];
        }
    }
    return NULL;
}

static const char *resolve_metadata_dir(void)
{
    const char *env = getenv("YKERN_METADATA_DIR");
    return (env && *env) ? env : YKERN_METADATA_DIR;
}

/* Parse one YAML file into the in-progress overlay struct. Returns OK with
 * `loaded_any` set to true if the file was found and parsed; false if it
 * didn't exist (no overlay file is OK). */
static struct ykern_ycore_void_result load_one_file(
    struct ykern_ynetlink_genl_family_overlay *out, const char *path,
    bool *loaded_any)
{
    *loaded_any = false;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            return YKERN_OK_VOID();
        }
        return YKERN_ERR(ykern_ycore_void, "overlay_load: fopen failed");
    }

    struct overlay_loader l = {0};
    l.out = out;
    l.file = f;
    /* Resume at current array sizes — appending, not replacing. */
    l.op_capacity = out->op_count;
    l.mcast_capacity = out->mcast_count;

    if (!yaml_parser_initialize(&l.parser)) {
        fclose(f);
        return YKERN_ERR(ykern_ycore_void, "overlay_load: yaml_parser_initialize failed");
    }
    yaml_parser_set_input_file(&l.parser, f);

    struct ykern_ycore_void_result rt = read_top(&l);
    yaml_parser_delete(&l.parser);
    fclose(f);

    if (YKERN_IS_ERR(rt)) {
        return YKERN_ERR(ykern_ycore_void, "overlay_load: yaml parse failed", rt);
    }
    *loaded_any = true;
    return YKERN_OK_VOID();
}

/* Merge step: after both files loaded, ops may contain
 *   (a) entries from generated:   cmd_id != 0, name set,    no descriptions
 *   (b) entries from curated:     cmd_id == 0, name set,    descriptions set
 * For each (b), find the matching (a) by name, transfer descriptions,
 * drop the (b) row.  Curated entries with no matching generated row are
 * warned about and dropped. */
static void merge_curated_into_generated(struct ykern_ynetlink_genl_family_overlay *o)
{
    size_t kept = 0;
    for (size_t i = 0; i < o->op_count; i++) {
        struct ykern_ynetlink_genl_op_overlay *cur = &o->ops[i];
        if (cur->cmd_id != 0) {
            /* generated entry — keep in place */
            if (kept != i) {
                o->ops[kept] = *cur;
            }
            kept++;
            continue;
        }
        /* curated entry — find generated entry with same name */
        int matched = 0;
        for (size_t j = 0; j < o->op_count; j++) {
            if (j == i) continue;
            if (o->ops[j].cmd_id != 0 && o->ops[j].name && cur->name &&
                strcmp(o->ops[j].name, cur->name) == 0) {
                /* Transfer descriptions. Free anything previously set. */
                free(o->ops[j].short_desc);
                free(o->ops[j].long_desc);
                o->ops[j].short_desc = cur->short_desc;
                o->ops[j].long_desc = cur->long_desc;
                cur->short_desc = NULL;
                cur->long_desc = NULL;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            ywarn("overlay_load: curated op '%s' has no generated entry; "
                  "did you forget to run `make regen-metadata`?",
                  cur->name ? cur->name : "?");
        }
        free(cur->name);
        free(cur->short_desc);
        free(cur->long_desc);
    }
    o->op_count = kept;
}

struct ykern_ynetlink_genl_family_overlay_ptr_result
ykern_ynetlink_genl_overlay_load(const char *family_name)
{
    if (!family_name) {
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: family_name is NULL");
    }

    const char *base = resolve_metadata_dir();
    char generated_path[4096];
    char curated_path[4096];
    int n;

    n = snprintf(generated_path, sizeof(generated_path),
                 "%s/netlink/genl/_generated/%s.yaml", base, family_name);
    if (n < 0 || (size_t)n >= sizeof(generated_path)) {
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: generated path too long");
    }
    n = snprintf(curated_path, sizeof(curated_path),
                 "%s/netlink/genl/%s.yaml", base, family_name);
    if (n < 0 || (size_t)n >= sizeof(curated_path)) {
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: curated path too long");
    }

    struct ykern_ynetlink_genl_family_overlay *out = calloc(1, sizeof(*out));
    if (!out) {
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: out calloc failed");
    }

    bool loaded_generated = false;
    bool loaded_curated = false;

    struct ykern_ycore_void_result r;
    r = load_one_file(out, generated_path, &loaded_generated);
    if (YKERN_IS_ERR(r)) {
        ykern_ynetlink_genl_family_overlay_destroy(out);
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: generated file failed", r);
    }
    r = load_one_file(out, curated_path, &loaded_curated);
    if (YKERN_IS_ERR(r)) {
        ykern_ynetlink_genl_family_overlay_destroy(out);
        return YKERN_ERR(ykern_ynetlink_genl_family_overlay_ptr,
                         "overlay_load: curated file failed", r);
    }

    if (!loaded_generated && !loaded_curated) {
        /* Neither file present — no overlay available. */
        ykern_ynetlink_genl_family_overlay_destroy(out);
        return YKERN_OK(ykern_ynetlink_genl_family_overlay_ptr, NULL);
    }

    merge_curated_into_generated(out);

    if (out->family_name && strcmp(out->family_name, family_name) != 0) {
        ywarn("overlay_load: family field '%s' does not match requested '%s'",
              out->family_name, family_name);
    }

    yinfo("overlay_load: %s — %zu ops, %zu mcast entries (generated=%d, curated=%d)",
          family_name, out->op_count, out->mcast_count,
          loaded_generated ? 1 : 0, loaded_curated ? 1 : 0);
    return YKERN_OK(ykern_ynetlink_genl_family_overlay_ptr, out);
}
