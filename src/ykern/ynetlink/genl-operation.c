/*
 * Generic netlink operation node — one cmd advertised by a family.
 *
 * If a YAML overlay supplied a human name (`STRSET_GET`, `LINKINFO_GET`, …)
 * the operation uses it; otherwise it falls back to the synthetic
 * "cmd-<id>". Same pattern for short / long descriptions.
 */

#include "genl-overlay.h"
#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>

#include <errno.h>
#include <linux/ethtool.h>
#include <linux/ethtool_netlink.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef NLA_OK
#define NLA_OK(nla, len)                                                                           \
    ((len) >= (int)sizeof(struct nlattr) && (nla)->nla_len >= sizeof(struct nlattr) &&             \
     (nla)->nla_len <= (len))
#endif
#ifndef NLA_NEXT
#define NLA_NEXT(nla, attrlen)                                                                     \
    ((attrlen) -= NLA_ALIGN((nla)->nla_len),                                                       \
     (struct nlattr *)((char *)(nla) + NLA_ALIGN((nla)->nla_len)))
#endif
#ifndef NLA_DATA
#define NLA_DATA(nla) ((void *)((char *)(nla) + NLA_HDRLEN))
#endif
#ifndef NLA_PAYLOAD
#define NLA_PAYLOAD(nla) ((nla)->nla_len - NLA_HDRLEN)
#endif

/* Builds a comma-separated list of flag mnemonics. Caller frees. Returns
 * NULL on alloc failure or empty (no flags). */
static char *describe_op_flags(uint32_t flags)
{
    char buf[256];
    size_t off = 0;
    buf[0] = '\0';

#define APPEND(name)                                                                               \
    do {                                                                                           \
        size_t need = strlen(name) + (off ? 2 : 0);                                                \
        if (off + need + 1 > sizeof(buf)) {                                                        \
            break;                                                                                 \
        }                                                                                          \
        if (off) {                                                                                 \
            buf[off++] = ',';                                                                      \
            buf[off++] = ' ';                                                                      \
        }                                                                                          \
        size_t nl = strlen(name);                                                                  \
        memcpy(buf + off, (name), nl);                                                             \
        off += nl;                                                                                 \
        buf[off] = '\0';                                                                           \
    } while (0)

    if (flags & GENL_ADMIN_PERM) APPEND("GENL_ADMIN_PERM");
    if (flags & GENL_CMD_CAP_DO) APPEND("GENL_CMD_CAP_DO");
    if (flags & GENL_CMD_CAP_DUMP) APPEND("GENL_CMD_CAP_DUMP");
    if (flags & GENL_CMD_CAP_HASPOL) APPEND("GENL_CMD_CAP_HASPOL");
#ifdef GENL_UNS_ADMIN_PERM
    if (flags & GENL_UNS_ADMIN_PERM) APPEND("GENL_UNS_ADMIN_PERM");
#endif

#undef APPEND

    if (off == 0) {
        return NULL;
    }
    char *out = malloc(off + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, buf, off + 1);
    return out;
}

static struct ykern_ycore_text_result op_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);
    if (op->overlay_name && op->overlay_name[0] != '\0') {
        return ykern_ycore_text_from_cstr(op->overlay_name);
    }
    return ykern_ycore_text_format("cmd-%u", (unsigned)op->cmd_id);
}

static struct ykern_ycore_text_result op_get_short_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);

    if (op->overlay_short && op->overlay_short[0] != '\0') {
        return ykern_ycore_text_from_cstr(op->overlay_short);
    }

    char *flag_str = describe_op_flags(op->flags);
    struct ykern_ycore_text_result r;
    if (flag_str) {
        r = ykern_ycore_text_format("cmd=%u, flags=0x%02x (%s)",
                                    (unsigned)op->cmd_id, (unsigned)op->flags, flag_str);
    } else {
        r = ykern_ycore_text_format("cmd=%u, flags=0x%02x",
                                    (unsigned)op->cmd_id, (unsigned)op->flags);
    }
    free(flag_str);
    return r;
}

/* Forward decls — these helpers are defined further down with the rest of
 * the invoke logic but op_get_long_description above uses them. */
static const struct ykern_ynetlink_genl_attr_set_overlay *find_op_attr_set(
    struct ykern_ynetlink_genl_operation *op,
    struct ykern_ynetlink_genl_family *family);

/*-----------------------------------------------------------------------------
 * Categorise an op name by its suffix so we can describe what it does in
 * one verb, e.g. LINKINFO_GET → "read", PORT_NEW → "create".
 *---------------------------------------------------------------------------*/
static void op_category(const char *name, const char **noun, const char **verb)
{
    *noun = "operation";
    *verb = "interact with";
    if (!name) return;
    size_t n = strlen(name);
#define ENDS_IN(suffix)                                                                            \
    (n >= sizeof(suffix) - 1 && strcmp(name + n - (sizeof(suffix) - 1), suffix) == 0)
    if (ENDS_IN("_GET") || ENDS_IN("_GET_CFG") || ENDS_IN("_GET_STATUS") ||
        ENDS_IN("_GET_ID")) {
        *noun = "read";
        *verb = "read state from";
    } else if (ENDS_IN("_SET") || ENDS_IN("_SET_CFG")) {
        *noun = "write";
        *verb = "configure";
    } else if (ENDS_IN("_NEW") || ENDS_IN("_ADD")) {
        *noun = "create";
        *verb = "create an entry on";
    } else if (ENDS_IN("_DEL") || ENDS_IN("_REMOVE")) {
        *noun = "delete";
        *verb = "remove an entry from";
    } else if (ENDS_IN("_ACT")) {
        *noun = "action";
        *verb = "trigger";
    } else if (ENDS_IN("_NTF")) {
        *noun = "notification";
        *verb = "subscribe to";
    } else if (strncmp(name, "GET_", 4) == 0) {
        *noun = "read";
        *verb = "read state from";
    } else if (strncmp(name, "SET_", 4) == 0) {
        *noun = "write";
        *verb = "configure";
    } else if (strncmp(name, "NEW_", 4) == 0 || strncmp(name, "ADD_", 4) == 0) {
        *noun = "create";
        *verb = "create an entry on";
    } else if (strncmp(name, "DEL_", 4) == 0) {
        *noun = "delete";
        *verb = "remove an entry from";
    }
#undef ENDS_IN
}

static struct ykern_ycore_text_result op_get_long_description(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);

    /* Resolve family context — names + the attribute set this op acts on. */
    const char *family_name = "?";
    struct ykern_ycore_object *fam_obj = self->parent;
    struct ykern_ynetlink_genl_family *family = NULL;
    if (fam_obj && fam_obj->kind == YKERN_YCORE_OBJECT_KIND_FAMILY) {
        family = ykern_container_of(fam_obj, struct ykern_ynetlink_genl_family, base);
        family_name = family->name ? family->name : "?";
    }
    const struct ykern_ynetlink_genl_attr_set_overlay *set = NULL;
    if (family) {
        set = find_op_attr_set(op, family);
    }
    const char *set_name = set ? set->set_name : "(no metadata)";
    size_t attr_count = set ? set->attr_count : 0;

    char *flag_str = describe_op_flags(op->flags);
    const char *noun = NULL, *verb = NULL;
    op_category(op->overlay_name, &noun, &verb);

    /* Build the body in one of three flavours: curated, named-without-curation,
     * synthetic-cmd-N.  Each is genuinely useful — no apologies, no docs
     * pointers, no "regen the overlay" advice. */
    struct ykern_ycore_text_result r;
    const char *name = op->overlay_name;

    if (op->overlay_long && op->overlay_long[0] != '\0') {
        /* Curated long description: use it, then append wire facts. */
        r = ykern_ycore_text_format(
            "%s\n\n"
            "Wire-level facts:\n"
            "  family       = %s (id %u)\n"
            "  cmd id       = %u (0x%02x)\n"
            "  flags        = 0x%02x %s%s%s\n"
            "  attribute set = %s%s%s\n",
            op->overlay_long,
            family_name, family ? (unsigned)family->id : 0,
            (unsigned)op->cmd_id, (unsigned)op->cmd_id,
            (unsigned)op->flags,
            flag_str ? "(" : "(none)",
            flag_str ? flag_str : "",
            flag_str ? ")" : "",
            set_name,
            set ? " — " : "",
            set ? "see children for the attribute names" : "");
    } else if (name) {
        /* Named op, no curated long_desc — synthesise something useful from
         * the metadata we have. */
        r = ykern_ycore_text_format(
            "**%s** — a %s operation. It lets you %s the **%s** family.\n\n"
            "Wire-level facts:\n"
            "  family       = %s (id %u)\n"
            "  cmd id       = %u (0x%02x)\n"
            "  flags        = 0x%02x %s%s%s\n"
            "  attribute set = %s (%zu attribute%s — see children below)\n\n"
            "How to call it:\n"
            "  ykern invoke /netlink/genl/%s/%s\n"
            "      no --arg → NLM_F_DUMP (kernel iterates every matching\n"
            "      object). Most reads support this.\n\n"
            "  ykern invoke /netlink/genl/%s/%s --arg KEY=VALUE [--arg ...]\n"
            "      KEY is one of the children's names. Numeric VALUE is\n"
            "      packed as u32; anything else as a NUL-terminated string.\n"
            "      Pass identifying attributes (ifindex, ifname, address,\n"
            "      pid, …) to target one specific object.",
            name, noun, verb, family_name,
            family_name, family ? (unsigned)family->id : 0,
            (unsigned)op->cmd_id, (unsigned)op->cmd_id,
            (unsigned)op->flags,
            flag_str ? "(" : "(none)",
            flag_str ? flag_str : "",
            flag_str ? ")" : "",
            set_name, attr_count, attr_count == 1 ? "" : "s",
            family_name, name,
            family_name, name);
    } else {
        /* Synthetic cmd-N — no UAPI header on this build, so we don't know
         * the name or attribute names.  Tell the user what they CAN do. */
        r = ykern_ycore_text_format(
            "Operation #%u of family **%s**. The kernel exposes this op but\n"
            "the matching UAPI header isn't installed on this build, so\n"
            "ykern can't put a name on it.\n\n"
            "Wire-level facts:\n"
            "  family       = %s (id %u)\n"
            "  cmd id       = %u (0x%02x)\n"
            "  flags        = 0x%02x %s%s%s\n\n"
            "How to call it:\n"
            "  ykern invoke /netlink/genl/%s/cmd-%u\n"
            "      no --arg → NLM_F_DUMP. The kernel will accept the\n"
            "      request and respond if the shape is right; it'll return\n"
            "      EOPNOTSUPP / EINVAL if it needs specific arguments.\n\n"
            "Adding a curated overlay file for this family would let ykern\n"
            "show its real op name and the attributes it accepts.",
            (unsigned)op->cmd_id, family_name,
            family_name, family ? (unsigned)family->id : 0,
            (unsigned)op->cmd_id, (unsigned)op->cmd_id,
            (unsigned)op->flags,
            flag_str ? "(" : "(none)",
            flag_str ? flag_str : "",
            flag_str ? ")" : "",
            family_name, (unsigned)op->cmd_id);
    }
    free(flag_str);
    return r;
}

/*-----------------------------------------------------------------------------
 * Find the family ancestor of an operation, and the attribute set that
 * describes the op's request/response shape (best effort without policy).
 *---------------------------------------------------------------------------*/

static struct ykern_ynetlink_genl_family *find_family_for_op(
    struct ykern_ycore_object *self)
{
    struct ykern_ycore_object *parent = self ? self->parent : NULL;
    if (!parent || parent->kind != YKERN_YCORE_OBJECT_KIND_FAMILY) return NULL;
    return ykern_container_of(parent, struct ykern_ynetlink_genl_family, base);
}

static int strip_op_suffix(const char *op_name, char *out, size_t out_size);
static int strip_op_prefix_get(const char *op_name, char *out, size_t out_size);

static const struct ykern_ynetlink_genl_attr_set_overlay *find_op_attr_set(
    struct ykern_ynetlink_genl_operation *op,
    struct ykern_ynetlink_genl_family *family)
{
    if (!family || !family->overlay) return NULL;
    /* 1. Flat ATTRS (nl80211, devlink, tcp_metrics, ...). */
    const struct ykern_ynetlink_genl_attr_set_overlay *s =
        ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay, "ATTRS");
    if (s) return s;
    /* 2. Per-set, derived from op name. */
    if (op->overlay_name) {
        char candidate[128];
        if (strip_op_suffix(op->overlay_name, candidate, sizeof(candidate))) {
            s = ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay,
                                                                 candidate);
            if (s) return s;
        }
        if (strip_op_prefix_get(op->overlay_name, candidate, sizeof(candidate))) {
            s = ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay,
                                                                 candidate);
            if (s) return s;
        }
    }
    /* 3. Ethtool: HEADER carries the request args (ifname / ifindex). */
    if (family->name && strcmp(family->name, "ethtool") == 0) {
        s = ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay, "HEADER");
        if (s) return s;
    }
    return NULL;
}

/*-----------------------------------------------------------------------------
 * populate_children — spawn one ATTRIBUTE child per attribute in the relevant
 * set, so browsing the operation reveals the names callers can pass to --arg.
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result op_populate_children(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);
    struct ykern_ynetlink_genl_family *family = find_family_for_op(self);
    if (!family) return YKERN_OK_VOID();

    const struct ykern_ynetlink_genl_attr_set_overlay *set =
        find_op_attr_set(op, family);
    if (!set) return YKERN_OK_VOID();

    for (size_t i = 0; i < set->attr_count; i++) {
        const struct ykern_ynetlink_genl_attr_overlay *a = &set->attrs[i];
        if (!a->name) continue;

        struct ykern_ynetlink_genl_attribute_ptr_result ar =
            ykern_ynetlink_genl_attribute_create(a->attr_id, a->name,
                                                  set->set_name, a->type_hint);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, ar,
                            "op_populate_children: attribute create failed");
        struct ykern_ycore_void_result append =
            ykern_ycore_object_append_child(self, &ar.value->base);
        if (YKERN_IS_ERR(append)) {
            ykern_ycore_object_destroy(&ar.value->base);
            return YKERN_ERR(ykern_ycore_void,
                             "op_populate_children: append failed", append);
        }
    }
    return YKERN_OK_VOID();
}

/*=============================================================================
 * Invoke — generic ethtool *_GET implementation driven by overlay metadata.
 *
 * The libclang generator extracts every ETHTOOL_A_<SET>_<ATTR> enum from the
 * kernel headers into the overlay's `attribute_sets`. This file uses those
 * lookups to:
 *   - look up HEADER / DEV_NAME / DEV_INDEX attribute ids when packing a
 *     request (no hardcoded ETHTOOL_A_* numbers anywhere in this file)
 *   - decode the response by walking attributes and finding their names in
 *     the op's response set, recursing when an attribute is itself a set.
 *
 * Result: every *_GET op that takes only HEADER (currently most of them)
 * works without any per-op code.
 *===========================================================================*/

#include "genl-overlay.h"

struct attr_buf {
    uint8_t *data;
    size_t off;
    size_t cap;
};

static int ab_reserve(struct attr_buf *b, size_t need)
{
    return b->off + need <= b->cap;
}

static int ab_string(struct attr_buf *b, uint16_t type, const char *s)
{
    size_t slen = strlen(s) + 1; /* include NUL */
    size_t total = NLA_HDRLEN + slen;
    if (!ab_reserve(b, NLA_ALIGN(total))) return 0;
    struct nlattr *nla = (struct nlattr *)(b->data + b->off);
    nla->nla_type = type;
    nla->nla_len = total;
    memcpy(b->data + b->off + NLA_HDRLEN, s, slen);
    b->off += NLA_ALIGN(total);
    return 1;
}

static int ab_u32(struct attr_buf *b, uint16_t type, uint32_t v)
{
    size_t total = NLA_HDRLEN + sizeof(v);
    if (!ab_reserve(b, NLA_ALIGN(total))) return 0;
    struct nlattr *nla = (struct nlattr *)(b->data + b->off);
    nla->nla_type = type;
    nla->nla_len = total;
    memcpy(b->data + b->off + NLA_HDRLEN, &v, sizeof(v));
    b->off += NLA_ALIGN(total);
    return 1;
}

static size_t ab_begin_nested(struct attr_buf *b, uint16_t type)
{
    /* Some kernels expect NLA_F_NESTED on the outer attribute type. */
    if (!ab_reserve(b, NLA_HDRLEN)) return SIZE_MAX;
    size_t at = b->off;
    struct nlattr *nla = (struct nlattr *)(b->data + at);
    nla->nla_type = type | NLA_F_NESTED;
    nla->nla_len = 0; /* patched in ab_end_nested */
    b->off += NLA_HDRLEN;
    return at;
}

static void ab_end_nested(struct attr_buf *b, size_t outer_off)
{
    struct nlattr *nla = (struct nlattr *)(b->data + outer_off);
    nla->nla_len = b->off - outer_off;
    /* The buffer is already aligned at b->off because every primitive ab_*
     * call advances by NLA_ALIGN. */
}

/*-----------------------------------------------------------------------------
 * Response rendering — growing char buffer with printf-style append.
 *---------------------------------------------------------------------------*/
struct text_buf {
    char *data;
    size_t off;
    size_t cap;
};

static int tb_grow(struct text_buf *b, size_t need)
{
    if (b->off + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->off + need + 1) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int tb_appendf(struct text_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int tb_appendf(struct text_buf *b, const char *fmt, ...)
{
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int n = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    if (n < 0) { va_end(ap2); return 0; }
    if (!tb_grow(b, (size_t)n)) { va_end(ap2); return 0; }
    vsnprintf(b->data + b->off, b->cap - b->off, fmt, ap2);
    va_end(ap2);
    b->off += (size_t)n;
    return 1;
}

static void tb_indent(struct text_buf *b, int depth)
{
    for (int i = 0; i < depth; i++) {
        tb_appendf(b, "  ");
    }
}

/*-----------------------------------------------------------------------------
 * Generic scalar / nested attribute rendering.
 *
 * Without policy info we can't tell exactly what each attribute's type is,
 * but we can be honest about it: pick a representation by payload size and
 * mark it as a guess.
 *---------------------------------------------------------------------------*/

static int looks_like_string(const uint8_t *data, size_t len)
{
    if (len < 1) return 0;
    /* Strings the kernel sends typically end in a NUL byte. */
    if (data[len - 1] != '\0') return 0;
    for (size_t i = 0; i < len - 1; i++) {
        unsigned char c = data[i];
        if (c < 0x20 && c != '\t') return 0;
        if (c == 0x7f) return 0;
    }
    return 1;
}

static void render_scalar(struct text_buf *tb, const struct nlattr *na)
{
    size_t plen = (size_t)NLA_PAYLOAD(na);
    const uint8_t *p = NLA_DATA(na);

    if (plen == 0) {
        tb_appendf(tb, "(flag, no payload)\n");
        return;
    }
    /* Try string FIRST — many netlink attributes are NUL-terminated names,
     * including 8-byte strings like "docker0\0" that would otherwise be
     * misread as u64.  looks_like_string requires the whole payload (minus
     * the trailing NUL) to be printable, so accidental matches are rare. */
    if (plen > 1 && looks_like_string(p, plen)) {
        tb_appendf(tb, "\"%.*s\"  (string, %zu bytes)\n",
                   (int)(plen - 1), (const char *)p, plen);
        return;
    }
    switch (plen) {
    case 1:
        tb_appendf(tb, "%u  (u8)\n", (unsigned)p[0]);
        return;
    case 2: {
        uint16_t v;
        memcpy(&v, p, 2);
        tb_appendf(tb, "%u  (u16)\n", (unsigned)v);
        return;
    }
    case 4: {
        uint32_t v;
        memcpy(&v, p, 4);
        tb_appendf(tb, "%u  (u32, 0x%08x)\n", (unsigned)v, (unsigned)v);
        return;
    }
    case 8: {
        uint64_t v;
        memcpy(&v, p, 8);
        tb_appendf(tb, "%llu  (u64, 0x%016llx)\n", (unsigned long long)v,
                   (unsigned long long)v);
        return;
    }
    default: {
        tb_appendf(tb, "(blob, %zu bytes)", plen);
        size_t show = plen < 32 ? plen : 32;
        for (size_t i = 0; i < show; i++) {
            tb_appendf(tb, " %02x", p[i]);
        }
        if (show < plen) tb_appendf(tb, " ...");
        tb_appendf(tb, "\n");
        return;
    }
    }
}

/*-----------------------------------------------------------------------------
 * Schema aliases.
 *
 * Many ethtool attributes are nested with a different schema than their
 * apparent name suggests — e.g. FEATURES.HW is itself a BITSET.  The kernel
 * doesn't tell us this in the simple GETFAMILY flow; we'd need policy info.
 * For now, hardcode a small table mapping (parent_set, attr_name) to the
 * actual schema set.  The table is short (~25 entries) and covers every
 * BITSET-shaped attribute in the ethtool family plus the strset wiring.
 *
 * TODO: surface this in the curated YAML overlay (e.g. `nested_as: BITSET`
 * next to the attribute) so families can declare their own aliases.
 *---------------------------------------------------------------------------*/
struct schema_alias {
    const char *parent_set;
    const char *attr_name;
    const char *child_set;
};

static const struct schema_alias g_schema_aliases[] = {
    /* ethtool BITSET wrappers — the most common pattern. */
    {"FEATURES",    "HW",            "BITSET"},
    {"FEATURES",    "WANTED",        "BITSET"},
    {"FEATURES",    "ACTIVE",        "BITSET"},
    {"FEATURES",    "NOCHANGE",      "BITSET"},
    {"LINKMODES",   "OURS",          "BITSET"},
    {"LINKMODES",   "PEER",          "BITSET"},
    {"DEBUG",       "MSGMASK",       "BITSET"},
    {"PRIVFLAGS",   "FLAGS",         "BITSET"},
    {"WOL",         "MODES",         "BITSET"},
    {"TSINFO",      "TIMESTAMPING",  "BITSET"},
    {"TSINFO",      "TX_TYPES",      "BITSET"},
    {"TSINFO",      "RX_FILTERS",    "BITSET"},
    {"FEC",         "ACTIVE_FEC",    "BITSET"},
    {"FEC",         "MODES",         "BITSET"},
    {"RSS",         "INDIR",         "BITSET"},
    /* BITSET internal wiring (nested name == set name except these). */
    {"BITSET",      "BITS",          "BITSET_BITS"},
    {"BITSET_BITS", "BIT",           "BITSET_BIT"},
    /* strset wiring. */
    {"STRSET",      "STRINGSETS",    "STRINGSETS"},
    {"STRINGSETS",  "STRINGSET",     "STRINGSET"},
    {"STRINGSET",   "STRINGS",       "STRINGS"},
    {"STRINGS",     "STRING",        "STRING"},
    {NULL, NULL, NULL},
};

static const char *resolve_alias(const char *parent_set, const char *attr_name)
{
    if (!parent_set || !attr_name) return NULL;
    for (const struct schema_alias *a = g_schema_aliases; a->parent_set; a++) {
        if (strcmp(a->parent_set, parent_set) == 0 &&
            strcmp(a->attr_name, attr_name) == 0) {
            return a->child_set;
        }
    }
    return NULL;
}

/*-----------------------------------------------------------------------------
 * BITSET_BIT compact renderer — folds the verbose
 *
 *   BIT:
 *     INDEX = N (u32)
 *     NAME = "..."
 *     VALUE = (flag)
 *
 * shape into a single line:  [N] name = on/off.  Without this, FEATURES_GET
 * would produce 600+ lines for the HW bitset alone.
 *---------------------------------------------------------------------------*/
static int try_render_bit_compact(struct text_buf *tb, struct nlattr *bit_outer,
                                  const struct ykern_ynetlink_genl_attr_set_overlay *bit_set,
                                  int depth)
{
    uint32_t idx_id = 0, name_id = 0, val_id = 0;
    if (!ykern_ynetlink_genl_attr_set_lookup_id(bit_set, "INDEX", &idx_id) ||
        !ykern_ynetlink_genl_attr_set_lookup_id(bit_set, "NAME", &name_id) ||
        !ykern_ynetlink_genl_attr_set_lookup_id(bit_set, "VALUE", &val_id)) {
        return 0;
    }

    uint32_t idx = UINT32_MAX;
    const char *name = NULL;
    size_t name_len = 0;
    int value_set = 0;
    int have_idx = 0;

    int rem = (int)NLA_PAYLOAD(bit_outer);
    struct nlattr *na = (struct nlattr *)NLA_DATA(bit_outer);
    while (NLA_OK(na, rem)) {
        uint16_t t = na->nla_type & NLA_TYPE_MASK;
        if (t == idx_id && (size_t)NLA_PAYLOAD(na) >= sizeof(uint32_t)) {
            memcpy(&idx, NLA_DATA(na), sizeof(uint32_t));
            have_idx = 1;
        } else if (t == name_id) {
            name = (const char *)NLA_DATA(na);
            name_len = (size_t)NLA_PAYLOAD(na);
            if (name_len > 0 && name[name_len - 1] == '\0') name_len--;
        } else if (t == val_id) {
            value_set = 1;
        }
        na = NLA_NEXT(na, rem);
    }

    tb_indent(tb, depth);
    if (name) {
        tb_appendf(tb, "[%3u] %.*s = %s\n", have_idx ? (unsigned)idx : 0u,
                   (int)name_len, name, value_set ? "on" : "off");
    } else {
        tb_appendf(tb, "[%3u] (no name) = %s\n",
                   have_idx ? (unsigned)idx : 0u, value_set ? "on" : "off");
    }
    return 1;
}

static void render_attrs_recursive(struct text_buf *tb, struct nlattr *first, int total_len,
                                   const struct ykern_ynetlink_genl_attr_set_overlay *set,
                                   const struct ykern_ynetlink_genl_family_overlay *all,
                                   int depth)
{
    struct nlattr *na = first;
    int rem = total_len;
    while (NLA_OK(na, rem)) {
        uint16_t raw_type = na->nla_type;
        uint16_t t = raw_type & NLA_TYPE_MASK;
        int nested_flag = (raw_type & NLA_F_NESTED) != 0;
        const char *name = ykern_ynetlink_genl_attr_set_lookup_name(set, t);

        /* Resolve child set. Try in order:
         *   1) same-name set (kernel convention: HEADER attr → HEADER set)
         *   2) hardcoded schema alias (BITSET wrappers etc.)
         */
        const struct ykern_ynetlink_genl_attr_set_overlay *child_set = NULL;
        if (name) {
            child_set = ykern_ynetlink_genl_family_overlay_find_attr_set(all, name);
            if (!child_set) {
                const char *aliased = resolve_alias(set ? set->set_name : NULL, name);
                if (aliased) {
                    child_set = ykern_ynetlink_genl_family_overlay_find_attr_set(all, aliased);
                }
            }
        }
        int treat_nested = nested_flag || (child_set != NULL);

        if (treat_nested) {
            /* Compact rendering for BITSET BITS items. */
            if (child_set && strcmp(child_set->set_name, "BITSET_BIT") == 0 &&
                try_render_bit_compact(tb, na, child_set, depth)) {
                na = NLA_NEXT(na, rem);
                continue;
            }

            tb_indent(tb, depth);
            tb_appendf(tb, "%s:\n", name ? name : "?");
            if (child_set) {
                render_attrs_recursive(tb, (struct nlattr *)NLA_DATA(na),
                                       (int)NLA_PAYLOAD(na), child_set, all, depth + 1);
            } else {
                tb_indent(tb, depth + 1);
                tb_appendf(tb,
                           "(nested, %u bytes — schema not declared; add a schema "
                           "alias if this attribute uses a known sub-set)\n",
                           (unsigned)NLA_PAYLOAD(na));
            }
        } else {
            tb_indent(tb, depth);
            if (name) {
                tb_appendf(tb, "%s = ", name);
            } else {
                tb_appendf(tb, "[attr %u] = ", (unsigned)t);
            }
            render_scalar(tb, na);
        }
        na = NLA_NEXT(na, rem);
    }
}

/*-----------------------------------------------------------------------------
 * Generic ethtool *_GET invoke.
 *
 *   1) Strip the trailing _GET / _GET_CFG / _GET_STATUS to derive the
 *      response attribute set name.
 *   2) Look up HEADER / DEV_NAME / DEV_INDEX ids from the HEADER set.
 *   3) Look up the response set's HEADER attribute id.
 *   4) Build, send, receive, decode.
 *---------------------------------------------------------------------------*/

static int strip_op_suffix(const char *op_name, char *out, size_t out_size)
{
    if (!op_name) return 0;
    size_t len = strlen(op_name);
    /* Suffixes longest-first so we don't accidentally match "_GET" on
     * "FOO_GET_STATUS" before testing "_GET_STATUS". */
    static const char *suffixes[] = {
        "_GET_STATUS", "_GET_CFG", "_GET_ID", "_GET", "_SET", "_NEW", "_DEL",
        "_ADD", "_NTF", "_ACT", NULL,
    };
    for (size_t i = 0; suffixes[i]; i++) {
        size_t slen = strlen(suffixes[i]);
        if (len > slen && strcmp(op_name + len - slen, suffixes[i]) == 0) {
            size_t keep = len - slen;
            if (keep + 1 > out_size) return 0;
            memcpy(out, op_name, keep);
            out[keep] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Strips the ethtool-style "GET_" prefix used by some families (e.g. nl80211
 * has GET_INTERFACE, GET_STATION) so we can derive INTERFACE / STATION as a
 * candidate set name. */
static int strip_op_prefix_get(const char *op_name, char *out, size_t out_size)
{
    if (!op_name) return 0;
    static const char *prefixes[] = {"GET_", "SET_", "NEW_", "DEL_", NULL};
    for (size_t i = 0; prefixes[i]; i++) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(op_name, prefixes[i], plen) == 0 && op_name[plen]) {
            size_t keep = strlen(op_name + plen);
            if (keep + 1 > out_size) return 0;
            memcpy(out, op_name + plen, keep + 1);
            return 1;
        }
    }
    return 0;
}

static struct ykern_ycore_text_result invoke_ethtool_get(
    struct ykern_ynetlink_genl_operation *op, struct ykern_ynetlink_genl_family *family,
    struct ykern_ynetlink_ynetlink *transport,
    const struct ykern_ycore_invoke_args *args)
{
    if (!family->overlay) {
        return YKERN_ERR(ykern_ycore_text,
                         "ethtool *_GET: no overlay loaded — run `make regen-metadata-ethtool`");
    }

    /* Derive response attribute set name from op name. */
    char set_name[64];
    if (!op->overlay_name || !strip_op_suffix(op->overlay_name, set_name, sizeof(set_name))) {
        return ykern_ycore_text_format(
            "%s: cannot derive an attribute set from the op name (expected "
            "*_GET, *_GET_CFG, or *_GET_STATUS)",
            op->overlay_name ? op->overlay_name : "?");
    }

    const struct ykern_ynetlink_genl_attr_set_overlay *response_set =
        ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay, set_name);
    if (!response_set) {
        return ykern_ycore_text_format(
            "%s: derived attribute set '%s' not present in overlay; either the "
            "kernel header doesn't expose it or the overlay needs regeneration",
            op->overlay_name, set_name);
    }
    const struct ykern_ynetlink_genl_attr_set_overlay *header_set =
        ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay, "HEADER");
    if (!header_set) {
        return YKERN_ERR(ykern_ycore_text,
                         "ethtool *_GET: HEADER attribute set missing from overlay");
    }

    /* Look up the attribute ids we need. */
    uint32_t hdr_attr_id = 0, dev_name_id = 0, dev_index_id = 0;
    if (!ykern_ynetlink_genl_attr_set_lookup_id(response_set, "HEADER", &hdr_attr_id)) {
        return ykern_ycore_text_format(
            "%s: response set '%s' has no HEADER attribute — this op may not be "
            "a plain *_GET shape; needs hand-curated invoke",
            op->overlay_name, set_name);
    }
    if (!ykern_ynetlink_genl_attr_set_lookup_id(header_set, "DEV_NAME", &dev_name_id) ||
        !ykern_ynetlink_genl_attr_set_lookup_id(header_set, "DEV_INDEX", &dev_index_id)) {
        return YKERN_ERR(ykern_ycore_text,
                         "ethtool *_GET: HEADER set missing DEV_NAME / DEV_INDEX");
    }

    /* Resolve the target NIC.  No ifname/ifindex => DUMP every NIC. */
    const char *ifname = ykern_ycore_invoke_args_lookup(args, "ifname");
    const char *ifindex_s = ykern_ycore_invoke_args_lookup(args, "ifindex");
    int dump_mode = (!ifname && !ifindex_s);

    /* Build the request. */
    uint8_t req[256] = {0};
    struct nlmsghdr *nh = (struct nlmsghdr *)req;
    nh->nlmsg_type = family->id;
    nh->nlmsg_flags = NLM_F_REQUEST | (dump_mode ? NLM_F_DUMP : 0);
    nh->nlmsg_seq = ykern_ynetlink_ynetlink_next_seq(transport);
    nh->nlmsg_pid = 0;

    struct genlmsghdr *gh = (struct genlmsghdr *)NLMSG_DATA(nh);
    gh->cmd = (uint8_t)op->cmd_id;
    gh->version = (uint8_t)(family->version ? family->version : 1);

    struct attr_buf b = {
        .data = req,
        .off = NLMSG_LENGTH(sizeof(*gh)),
        .cap = sizeof(req),
    };

    size_t hdr_off = ab_begin_nested(&b, (uint16_t)hdr_attr_id);
    if (hdr_off == SIZE_MAX) {
        return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: buffer overflow on HEADER");
    }
    if (ifname) {
        if (!ab_string(&b, (uint16_t)dev_name_id, ifname)) {
            return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: buffer overflow on DEV_NAME");
        }
    } else if (ifindex_s) {
        char *end = NULL;
        unsigned long v = strtoul(ifindex_s, &end, 10);
        if (!end || *end != '\0' || v > UINT32_MAX) {
            return ykern_ycore_text_format("%s: ifindex must be a u32", op->overlay_name);
        }
        if (!ab_u32(&b, (uint16_t)dev_index_id, (uint32_t)v)) {
            return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: buffer overflow on DEV_INDEX");
        }
    }
    /* DUMP mode: HEADER carries no inner attrs — kernel iterates all NICs. */
    ab_end_nested(&b, hdr_off);
    nh->nlmsg_len = b.off;

    /* Send. */
    struct ykern_ynetlink_ynetlink_socket_result sr =
        ykern_ynetlink_ynetlink_socket_get(transport);
    if (YKERN_IS_ERR(sr)) {
        return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: socket open failed", sr);
    }
    int fd = sr.value;

    struct sockaddr_nl dst = {.nl_family = AF_NETLINK};
    if (sendto(fd, req, nh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: sendto failed");
    }

    /* Receive — single response for non-dump, multi-message stream for dump. */
    char rbuf[65536];
    struct text_buf tb = {0};
    int reply_count = 0;
    int dump_done = 0;

    tb_appendf(&tb, "%s reply (cmd=%u, family=%s id=%u, set=%s)%s:\n",
               op->overlay_name, (unsigned)op->cmd_id, family->name,
               (unsigned)family->id, set_name, dump_mode ? " [DUMP]" : "");

    do {
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n < 0) {
            free(tb.data);
            return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: recv failed");
        }
        struct nlmsghdr *rh = (struct nlmsghdr *)rbuf;
        int remaining = (int)n;
        while (NLMSG_OK(rh, remaining)) {
            if (rh->nlmsg_type == NLMSG_DONE) {
                dump_done = 1;
                break;
            }
            if (rh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(rh);
                if (err->error == 0) {
                    /* ack — should only appear if NLM_F_ACK was set */
                    rh = NLMSG_NEXT(rh, remaining);
                    continue;
                }
                int code = -err->error;
                struct ykern_ycore_text_result rr = ykern_ycore_text_format(
                    "%s: kernel returned errno %d (%s) after %d %s",
                    op->overlay_name, code, strerror(code),
                    reply_count, reply_count == 1 ? "reply" : "replies");
                free(tb.data);
                return rr;
            }
            struct genlmsghdr *rgh = NLMSG_DATA(rh);
            int attrs_len = (int)rh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*rgh));
            struct nlattr *first =
                (struct nlattr *)((char *)rgh + NLA_ALIGN(sizeof(*rgh)));

            if (dump_mode) {
                tb_appendf(&tb, "\n--- entry %d ---\n", reply_count + 1);
            }
            render_attrs_recursive(&tb, first, attrs_len, response_set, family->overlay, 1);
            reply_count++;
            if (!dump_mode) {
                dump_done = 1; /* one shot, exit the outer loop */
                break;
            }
            rh = NLMSG_NEXT(rh, remaining);
        }
    } while (!dump_done);

    if (dump_mode) {
        tb_appendf(&tb, "\n(%d %s)\n", reply_count,
                   reply_count == 1 ? "interface" : "interfaces");
    }

    if (!tb.data) {
        return YKERN_ERR(ykern_ycore_text, "ethtool *_GET: oom while rendering");
    }
    return YKERN_OK(ykern_ycore_text,
                    ((struct ykern_ycore_text){.data = tb.data, .size = tb.off}));
}

static int op_name_ends_in_get(const char *name)
{
    if (!name) return 0;
    size_t len = strlen(name);
    if (len >= 4 && strcmp(name + len - 4, "_GET") == 0) return 1;
    if (len >= 8 && strcmp(name + len - 8, "_GET_CFG") == 0) return 1;
    if (len >= 11 && strcmp(name + len - 11, "_GET_STATUS") == 0) return 1;
    return 0;
}

/*-----------------------------------------------------------------------------
 * Generic genl *_GET invoke for any family that has a flat ATTRS set.
 *
 *   - No --arg flags → NLM_F_DUMP with no attrs (kernel iterates all objects)
 *   - --arg KEY=VAL  → look KEY up in ATTRS set, heuristic-type VAL,
 *                      pack as nlattr.  Numeric → u32; else string.
 *
 * Limitations honestly:
 *   - Without policy info we guess attribute types; some kernels reject
 *     a u32 where they want u8/u16/u64.  The kernel error is surfaced as-is.
 *   - Doesn't handle nested or binary-blob arguments.  For those, hand-curate.
 *---------------------------------------------------------------------------*/

static int looks_like_uint(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

static struct ykern_ycore_text_result invoke_generic_genl(
    struct ykern_ynetlink_genl_operation *op, struct ykern_ynetlink_genl_family *family,
    struct ykern_ynetlink_ynetlink *transport,
    const struct ykern_ycore_invoke_args *args)
{
    const struct ykern_ynetlink_genl_attr_set_overlay *attrs_set = NULL;
    if (family->overlay) {
        /* Try flat ATTRS first (nl80211, devlink, tcp_metrics, ...). */
        attrs_set = ykern_ynetlink_genl_family_overlay_find_attr_set(family->overlay,
                                                                      "ATTRS");
        /* Fallback for families with per-set sub-enums (netdev, dpll, ...).
         * Derive a candidate set name from the op name by stripping common
         * suffixes/prefixes. */
        if (!attrs_set && op->overlay_name) {
            char candidate[128];
            if (strip_op_suffix(op->overlay_name, candidate, sizeof(candidate))) {
                attrs_set = ykern_ynetlink_genl_family_overlay_find_attr_set(
                    family->overlay, candidate);
            }
            if (!attrs_set &&
                strip_op_prefix_get(op->overlay_name, candidate, sizeof(candidate))) {
                attrs_set = ykern_ynetlink_genl_family_overlay_find_attr_set(
                    family->overlay, candidate);
            }
        }
    }
    /* Build request. */
    int dump_mode = (args == NULL || args->count == 0);

    uint8_t req[1024] = {0};
    struct nlmsghdr *nh = (struct nlmsghdr *)req;
    nh->nlmsg_type = family->id;
    nh->nlmsg_flags = NLM_F_REQUEST | (dump_mode ? NLM_F_DUMP : 0);
    nh->nlmsg_seq = ykern_ynetlink_ynetlink_next_seq(transport);
    nh->nlmsg_pid = 0;

    struct genlmsghdr *gh = (struct genlmsghdr *)NLMSG_DATA(nh);
    gh->cmd = (uint8_t)op->cmd_id;
    gh->version = (uint8_t)(family->version ? family->version : 1);

    struct attr_buf b = {
        .data = req,
        .off = NLMSG_LENGTH(sizeof(*gh)),
        .cap = sizeof(req),
    };

    if (!dump_mode) {
        if (!attrs_set) {
            return ykern_ycore_text_format(
                "%s/%s: --arg used but family has no ATTRS metadata; "
                "rerun `make regen-metadata-all` or omit --arg for a DUMP.",
                family->name, op->overlay_name ? op->overlay_name : "?");
        }
        for (size_t i = 0; i < args->count; i++) {
            const char *key = args->entries[i].key;
            const char *val = args->entries[i].value;
            uint32_t attr_id = 0;
            if (!ykern_ynetlink_genl_attr_set_lookup_id(attrs_set, key, &attr_id)) {
                return ykern_ycore_text_format(
                    "%s/%s: unknown attribute '%s' in this family's ATTRS set",
                    family->name, op->overlay_name, key);
            }
            int packed = 0;
            if (looks_like_uint(val)) {
                char *end = NULL;
                unsigned long long v = strtoull(val, &end, 10);
                if (end && *end == '\0' && v <= UINT32_MAX) {
                    if (!ab_u32(&b, (uint16_t)attr_id, (uint32_t)v)) {
                        return YKERN_ERR(ykern_ycore_text,
                                         "generic invoke: buffer overflow on u32 attr");
                    }
                    packed = 1;
                }
            }
            if (!packed) {
                if (!ab_string(&b, (uint16_t)attr_id, val)) {
                    return YKERN_ERR(ykern_ycore_text,
                                     "generic invoke: buffer overflow on string attr");
                }
            }
        }
    }
    nh->nlmsg_len = b.off;

    /* Send. */
    struct ykern_ynetlink_ynetlink_socket_result sr =
        ykern_ynetlink_ynetlink_socket_get(transport);
    if (YKERN_IS_ERR(sr)) {
        return YKERN_ERR(ykern_ycore_text, "generic invoke: socket open failed", sr);
    }
    int fd = sr.value;

    struct sockaddr_nl dst = {.nl_family = AF_NETLINK};
    if (sendto(fd, req, nh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        return YKERN_ERR(ykern_ycore_text, "generic invoke: sendto failed");
    }

    /* Receive. */
    char rbuf[65536];
    struct text_buf tb = {0};
    int reply_count = 0;
    int done = 0;

    tb_appendf(&tb, "%s/%s reply (cmd=%u, family id=%u)%s:\n",
               family->name, op->overlay_name ? op->overlay_name : "?",
               (unsigned)op->cmd_id, (unsigned)family->id,
               dump_mode ? " [DUMP]" : "");

    do {
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n < 0) {
            free(tb.data);
            return YKERN_ERR(ykern_ycore_text, "generic invoke: recv failed");
        }
        struct nlmsghdr *rh = (struct nlmsghdr *)rbuf;
        int remaining = (int)n;
        while (NLMSG_OK(rh, remaining)) {
            if (rh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (rh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(rh);
                if (err->error == 0) {
                    rh = NLMSG_NEXT(rh, remaining);
                    continue;
                }
                int code = -err->error;
                struct ykern_ycore_text_result rr = ykern_ycore_text_format(
                    "%s/%s: kernel returned errno %d (%s) after %d %s",
                    family->name, op->overlay_name, code, strerror(code),
                    reply_count, reply_count == 1 ? "reply" : "replies");
                free(tb.data);
                return rr;
            }
            struct genlmsghdr *rgh = NLMSG_DATA(rh);
            int attrs_len = (int)rh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*rgh));
            struct nlattr *first =
                (struct nlattr *)((char *)rgh + NLA_ALIGN(sizeof(*rgh)));

            if (dump_mode) {
                tb_appendf(&tb, "\n--- entry %d ---\n", reply_count + 1);
            }
            if (attrs_set) {
                render_attrs_recursive(&tb, first, attrs_len, attrs_set,
                                       family->overlay, 1);
            } else {
                tb_appendf(&tb, "  (response carries %d bytes of attributes; "
                           "no ATTRS metadata to decode names — regen overlay)\n",
                           attrs_len);
            }
            reply_count++;
            if (!dump_mode) {
                done = 1;
                break;
            }
            rh = NLMSG_NEXT(rh, remaining);
        }
    } while (!done);

    if (dump_mode) {
        tb_appendf(&tb, "\n(%d %s)\n", reply_count,
                   reply_count == 1 ? "object" : "objects");
    }

    if (!tb.data) {
        return YKERN_ERR(ykern_ycore_text, "generic invoke: oom while rendering");
    }
    return YKERN_OK(ykern_ycore_text,
                    ((struct ykern_ycore_text){.data = tb.data, .size = tb.off}));
}

static struct ykern_ycore_text_result op_invoke(struct ykern_ycore_object *self,
                                                const struct ykern_ycore_invoke_args *args)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);

    struct ykern_ycore_object *parent = self->parent;
    if (!parent || parent->kind != YKERN_YCORE_OBJECT_KIND_FAMILY) {
        return YKERN_ERR(ykern_ycore_text, "invoke: operation has no family parent");
    }
    struct ykern_ynetlink_genl_family *family =
        ykern_container_of(parent, struct ykern_ynetlink_genl_family, base);

    struct ykern_ynetlink_ynetlink *transport = ykern_ynetlink_find_transport(self);
    if (!transport) {
        return YKERN_ERR(ykern_ycore_text, "invoke: could not locate transport ancestor");
    }

    if (strcmp(family->name, "ethtool") == 0 && op_name_ends_in_get(op->overlay_name)) {
        return invoke_ethtool_get(op, family, transport, args);
    }

    /* Generic dispatcher for every other family.  No notion of "is this a
     * read?" — we just ship whatever the user asked, and let the kernel
     * accept or reject.  Most ops without --arg work as DUMPs. */
    return invoke_generic_genl(op, family, transport, args);
}

static void op_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_operation *op =
        ykern_container_of(self, struct ykern_ynetlink_genl_operation, base);
    free(op->overlay_name);
    free(op->overlay_short);
    free(op->overlay_long);
    op->overlay_name = NULL;
    op->overlay_short = NULL;
    op->overlay_long = NULL;
}

static const struct ykern_ycore_object_ops op_ops = {
    .get_name = op_get_name,
    .get_short_description = op_get_short_description,
    .get_long_description = op_get_long_description,
    .populate_children = op_populate_children,
    .invoke = op_invoke,
    .destroy_impl = op_destroy_impl,
};

/* Helper to dup an optional string. Returns 1 on success (or src is NULL),
 * 0 on alloc failure. */
static int dup_optional(const char *src, char **dst)
{
    if (!src) {
        *dst = NULL;
        return 1;
    }
    *dst = strdup(src);
    return *dst != NULL;
}

struct ykern_ynetlink_genl_operation_ptr_result ykern_ynetlink_genl_operation_create(
    uint32_t cmd_id, uint32_t flags, const char *overlay_name, const char *overlay_short,
    const char *overlay_long)
{
    struct ykern_ynetlink_genl_operation *op = calloc(1, sizeof(*op));
    if (!op) {
        return YKERN_ERR(ykern_ynetlink_genl_operation_ptr,
                         "genl_operation_create: calloc failed");
    }
    op->base.ops = &op_ops;
    op->base.kind = YKERN_YCORE_OBJECT_KIND_OPERATION;
    op->cmd_id = cmd_id;
    op->flags = flags;

    if (!dup_optional(overlay_name, &op->overlay_name) ||
        !dup_optional(overlay_short, &op->overlay_short) ||
        !dup_optional(overlay_long, &op->overlay_long)) {
        free(op->overlay_name);
        free(op->overlay_short);
        free(op->overlay_long);
        free(op);
        return YKERN_ERR(ykern_ynetlink_genl_operation_ptr,
                         "genl_operation_create: overlay strdup failed");
    }
    return YKERN_OK(ykern_ynetlink_genl_operation_ptr, op);
}
