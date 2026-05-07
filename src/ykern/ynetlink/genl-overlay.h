#ifndef YKERN_YNETLINK_GENL_OVERLAY_H
#define YKERN_YNETLINK_GENL_OVERLAY_H

/*
 * Per-family YAML overlay — hand-curated metadata that augments what
 * CTRL_CMD_GETFAMILY reports. The kernel returns numeric op cmd ids; the
 * overlay maps each to a human name (`STRSET_GET`, `LINKINFO_GET`, ...) plus
 * short / long descriptions. See docs/metadata-schema.md for the file format.
 *
 * Internal to the ynetlink module.
 */

#include <stddef.h>
#include <stdint.h>

#include <ykern/ycore/result.h>

struct ykern_ynetlink_genl_op_overlay {
    uint32_t cmd_id;
    char *name;        /* required if entry exists; replaces "cmd-<id>" */
    char *short_desc;  /* may be NULL */
    char *long_desc;   /* may be NULL */
};

struct ykern_ynetlink_genl_mcast_overlay {
    char *name;        /* matches kernel-assigned mcast group name */
    char *short_desc;
    char *long_desc;
};

/*-----------------------------------------------------------------------------
 * Attribute sets — generated from the kernel header.
 *
 * Ethtool-shape families declare one anonymous enum per logical message
 * (HEADER, LINKINFO, FEATURES, ...). Each enum maps an attribute id to a
 * name. The runtime uses these to:
 *   - build requests (look up id by name when packing nlattrs)
 *   - decode responses (look up name by id when rendering nlattrs)
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_attr_overlay {
    uint32_t attr_id;
    char *name; /* owned */
    char *type_hint; /* owned; e.g. "u32", "string", "nest - _A_HEADER_*", or NULL */
};

struct ykern_ynetlink_genl_attr_set_overlay {
    char *set_name; /* owned, e.g. "HEADER", "LINKINFO" */
    struct ykern_ynetlink_genl_attr_overlay *attrs;
    size_t attr_count;
};

struct ykern_ynetlink_genl_family_overlay {
    char *family_name; /* sanity-check vs the kernel-reported family name */
    char *short_desc;  /* family-level override; may be NULL */
    char *long_desc;   /* family-level override; may be NULL */

    struct ykern_ynetlink_genl_op_overlay *ops;
    size_t op_count;

    struct ykern_ynetlink_genl_mcast_overlay *mcast;
    size_t mcast_count;

    struct ykern_ynetlink_genl_attr_set_overlay *attr_sets;
    size_t attr_set_count;
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_family_overlay_ptr,
                      struct ykern_ynetlink_genl_family_overlay *);

/* Load the overlay for the named family. The function searches the
 * metadata directory:
 *   1) $YKERN_METADATA_DIR (if set)
 *   2) compile-time YKERN_METADATA_DIR (cmake-injected)
 * If no overlay file exists for the family, the result is OK with a NULL
 * value pointer (overlays are optional). Malformed YAML produces an error. */
struct ykern_ynetlink_genl_family_overlay_ptr_result
ykern_ynetlink_genl_overlay_load(const char *family_name);

void ykern_ynetlink_genl_family_overlay_destroy(
    struct ykern_ynetlink_genl_family_overlay *o);

/* Lookups — NULL if no entry. The returned pointers are owned by the
 * overlay; consumers must not free them. */
const struct ykern_ynetlink_genl_op_overlay *
ykern_ynetlink_genl_family_overlay_find_op(
    const struct ykern_ynetlink_genl_family_overlay *o, uint32_t cmd_id);

const struct ykern_ynetlink_genl_mcast_overlay *
ykern_ynetlink_genl_family_overlay_find_mcast(
    const struct ykern_ynetlink_genl_family_overlay *o, const char *name);

const struct ykern_ynetlink_genl_attr_set_overlay *
ykern_ynetlink_genl_family_overlay_find_attr_set(
    const struct ykern_ynetlink_genl_family_overlay *o, const char *set_name);

/* Lookup name by id. Returns owned-by-overlay pointer (do not free). */
const char *ykern_ynetlink_genl_attr_set_lookup_name(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, uint32_t attr_id);

/* Lookup id by name. Returns 1 on found (and writes *out_id), 0 on miss. */
int ykern_ynetlink_genl_attr_set_lookup_id(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, const char *name,
    uint32_t *out_id);

/* Lookup the inline-comment type hint kernel headers ship for the attribute
 * (e.g. "u32", "string", "nest - _A_HEADER_*").  Returns NULL when the
 * header has no comment or the attr isn't in the set. */
const char *ykern_ynetlink_genl_attr_set_lookup_type(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, uint32_t attr_id);
const char *ykern_ynetlink_genl_attr_set_lookup_type_by_name(
    const struct ykern_ynetlink_genl_attr_set_overlay *set, const char *name);

#endif /* YKERN_YNETLINK_GENL_OVERLAY_H */
