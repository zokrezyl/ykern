/*
 * NETLINK_ROUTE (rtnetlink) namespace — siblings of /netlink/genl.
 *
 * Exposes the four bread-and-butter rtnetlink dump operations:
 *
 *   /netlink/rtnl/GETLINK    enumerate every network interface (RTM_GETLINK)
 *   /netlink/rtnl/GETADDR    enumerate every assigned address  (RTM_GETADDR)
 *   /netlink/rtnl/GETROUTE   enumerate every IPv4 route        (RTM_GETROUTE)
 *   /netlink/rtnl/GETNEIGH   enumerate every neigh entry       (RTM_GETNEIGH)
 *
 * Each op opens NETLINK_ROUTE on first invoke, sends a single nlmsghdr +
 * rtgenmsg + NLM_F_DUMP request, then walks the multi-message reply.  For
 * each reply message we skip past the per-message struct (ifinfomsg /
 * ifaddrmsg / rtmsg / ndmsg) and decode the trailing nlattr stream using a
 * compact attribute-name table (IFLA_*, IFA_*, RTA_*, NDA_*).
 *
 * Per-attribute type info in the tables drives:
 *   - "string" -> print as quoted text
 *   - "u8/u16/u32" -> print as that integer
 *   - "binary" -> hex-dump (MAC, IPv6 address, etc.)
 *   - "nested" -> tagged but not recursively decoded yet
 */

#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
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

/*=============================================================================
 * Static per-set attribute name tables.  Hand-curated subset of the most
 * commonly-seen attributes — enough to make `ip`-equivalent dumps readable.
 *===========================================================================*/

enum rtnl_kind { RT_U8, RT_U16, RT_U32, RT_U64, RT_S32, RT_STR, RT_BIN, RT_NEST, RT_FLAG };

struct rtnl_attr {
    uint16_t id;
    const char *name;
    enum rtnl_kind kind;
};

static const struct rtnl_attr IFLA_attrs[] = {
    {IFLA_ADDRESS,           "ADDRESS",           RT_BIN},
    {IFLA_BROADCAST,         "BROADCAST",         RT_BIN},
    {IFLA_IFNAME,            "IFNAME",            RT_STR},
    {IFLA_MTU,               "MTU",               RT_U32},
    {IFLA_LINK,              "LINK",              RT_U32},
    {IFLA_QDISC,             "QDISC",             RT_STR},
    {IFLA_STATS,             "STATS",             RT_BIN},
    {IFLA_MASTER,            "MASTER",            RT_U32},
    {IFLA_TXQLEN,            "TXQLEN",            RT_U32},
    {IFLA_OPERSTATE,         "OPERSTATE",         RT_U8},
    {IFLA_LINKMODE,          "LINKMODE",          RT_U8},
    {IFLA_LINKINFO,          "LINKINFO",          RT_NEST},
    {IFLA_NET_NS_PID,        "NET_NS_PID",        RT_U32},
    {IFLA_IFALIAS,           "IFALIAS",           RT_STR},
    {IFLA_NUM_VF,            "NUM_VF",            RT_U32},
    {IFLA_STATS64,           "STATS64",           RT_BIN},
    {IFLA_AF_SPEC,           "AF_SPEC",           RT_NEST},
    {IFLA_GROUP,             "GROUP",             RT_U32},
    {IFLA_PROMISCUITY,       "PROMISCUITY",       RT_U32},
    {IFLA_NUM_TX_QUEUES,     "NUM_TX_QUEUES",     RT_U32},
    {IFLA_NUM_RX_QUEUES,     "NUM_RX_QUEUES",     RT_U32},
    {IFLA_CARRIER,           "CARRIER",           RT_U8},
    {IFLA_PHYS_PORT_ID,      "PHYS_PORT_ID",      RT_BIN},
    {IFLA_CARRIER_CHANGES,   "CARRIER_CHANGES",   RT_U32},
    {IFLA_PHYS_SWITCH_ID,    "PHYS_SWITCH_ID",    RT_BIN},
    {IFLA_LINK_NETNSID,      "LINK_NETNSID",      RT_S32},
    {IFLA_PHYS_PORT_NAME,    "PHYS_PORT_NAME",    RT_STR},
    {IFLA_PROTO_DOWN,        "PROTO_DOWN",        RT_U8},
    {IFLA_GSO_MAX_SEGS,      "GSO_MAX_SEGS",      RT_U32},
    {IFLA_GSO_MAX_SIZE,      "GSO_MAX_SIZE",      RT_U32},
    {IFLA_PAD,               "PAD",               RT_BIN},
    {IFLA_XDP,               "XDP",               RT_NEST},
    {IFLA_EVENT,             "EVENT",             RT_U32},
    {IFLA_NEW_NETNSID,       "NEW_NETNSID",       RT_S32},
    {IFLA_NEW_IFINDEX,       "NEW_IFINDEX",       RT_S32},
    {IFLA_MIN_MTU,           "MIN_MTU",           RT_U32},
    {IFLA_MAX_MTU,           "MAX_MTU",           RT_U32},
    {IFLA_PERM_ADDRESS,      "PERM_ADDRESS",      RT_BIN},
    {0, NULL, 0}
};

static const struct rtnl_attr IFA_attrs[] = {
    {IFA_ADDRESS,    "ADDRESS",    RT_BIN},
    {IFA_LOCAL,      "LOCAL",      RT_BIN},
    {IFA_LABEL,      "LABEL",      RT_STR},
    {IFA_BROADCAST,  "BROADCAST",  RT_BIN},
    {IFA_ANYCAST,    "ANYCAST",    RT_BIN},
    {IFA_CACHEINFO,  "CACHEINFO",  RT_BIN},
    {IFA_MULTICAST,  "MULTICAST",  RT_BIN},
    {IFA_FLAGS,      "FLAGS",      RT_U32},
    {IFA_RT_PRIORITY,"RT_PRIORITY",RT_U32},
    {IFA_TARGET_NETNSID,"TARGET_NETNSID",RT_S32},
    {0, NULL, 0}
};

static const struct rtnl_attr RTA_attrs[] = {
    {RTA_DST,         "DST",         RT_BIN},
    {RTA_SRC,         "SRC",         RT_BIN},
    {RTA_IIF,         "IIF",         RT_U32},
    {RTA_OIF,         "OIF",         RT_U32},
    {RTA_GATEWAY,     "GATEWAY",     RT_BIN},
    {RTA_PRIORITY,    "PRIORITY",    RT_U32},
    {RTA_PREFSRC,     "PREFSRC",     RT_BIN},
    {RTA_METRICS,     "METRICS",     RT_NEST},
    {RTA_MULTIPATH,   "MULTIPATH",   RT_NEST},
    {RTA_FLOW,        "FLOW",        RT_U32},
    {RTA_CACHEINFO,   "CACHEINFO",   RT_BIN},
    {RTA_TABLE,       "TABLE",       RT_U32},
    {RTA_MARK,        "MARK",        RT_U32},
    {RTA_MFC_STATS,   "MFC_STATS",   RT_BIN},
    {RTA_VIA,         "VIA",         RT_BIN},
    {RTA_NEWDST,      "NEWDST",      RT_BIN},
    {RTA_PREF,        "PREF",        RT_U8},
    {RTA_ENCAP_TYPE,  "ENCAP_TYPE",  RT_U16},
    {RTA_ENCAP,       "ENCAP",       RT_NEST},
    {RTA_EXPIRES,     "EXPIRES",     RT_U32},
    {RTA_PAD,         "PAD",         RT_BIN},
    {RTA_UID,         "UID",         RT_U32},
    {RTA_TTL_PROPAGATE,"TTL_PROPAGATE",RT_U8},
    {0, NULL, 0}
};

static const struct rtnl_attr NDA_attrs[] = {
    {NDA_DST,        "DST",        RT_BIN},
    {NDA_LLADDR,     "LLADDR",     RT_BIN},
    {NDA_CACHEINFO,  "CACHEINFO",  RT_BIN},
    {NDA_PROBES,     "PROBES",     RT_U32},
    {NDA_VLAN,       "VLAN",       RT_U16},
    {NDA_PORT,       "PORT",       RT_U16},
    {NDA_VNI,        "VNI",        RT_U32},
    {NDA_IFINDEX,    "IFINDEX",    RT_U32},
    {NDA_MASTER,     "MASTER",     RT_U32},
    {NDA_LINK_NETNSID,"LINK_NETNSID",RT_U32},
    {NDA_SRC_VNI,    "SRC_VNI",    RT_U32},
    {0, NULL, 0}
};

/*=============================================================================
 * Per-op metadata.
 *===========================================================================*/

struct rtnl_op_meta {
    const char *name;
    const char *short_desc;
    uint16_t msg_type;       /* RTM_GETLINK et al */
    uint8_t default_family;  /* AF_UNSPEC for "any", AF_INET to scope to v4 */
    size_t hdr_skip;         /* bytes between nlmsghdr payload start and attrs */
    const struct rtnl_attr *attrs;
};

static const struct rtnl_op_meta rtnl_ops[] = {
    {"GETLINK",  "List network interfaces",
     RTM_GETLINK,  AF_UNSPEC, sizeof(struct ifinfomsg), IFLA_attrs},
    {"GETADDR",  "List configured addresses (per interface, all families)",
     RTM_GETADDR,  AF_UNSPEC, sizeof(struct ifaddrmsg), IFA_attrs},
    {"GETROUTE", "List IPv4 routes",
     RTM_GETROUTE, AF_INET,   sizeof(struct rtmsg),     RTA_attrs},
    {"GETNEIGH", "List neighbour (ARP/NDISC) entries",
     RTM_GETNEIGH, AF_UNSPEC, sizeof(struct ndmsg),     NDA_attrs},
};
#define RTNL_OP_COUNT (sizeof(rtnl_ops) / sizeof(rtnl_ops[0]))

/*=============================================================================
 * Internal types.
 *===========================================================================*/

struct ykern_ynetlink_rtnl_namespace {
    struct ykern_ycore_object base;  /* kind = NAMESPACE */
    int sock_fd;                     /* lazy NETLINK_ROUTE socket */
    uint32_t seq;
};

struct ykern_ynetlink_rtnl_op {
    struct ykern_ycore_object base;  /* kind = OPERATION */
    const struct rtnl_op_meta *meta;
};

/*=============================================================================
 * Text buffer + scalar rendering — duplicated from genl-operation.c on
 * purpose so this file stays self-contained.
 *===========================================================================*/

struct rtb {
    char *data;
    size_t off;
    size_t cap;
};

static int rtb_grow(struct rtb *b, size_t need)
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

static int rtb_appendf(struct rtb *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int rtb_appendf(struct rtb *b, const char *fmt, ...)
{
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int n = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    if (n < 0) { va_end(ap2); return 0; }
    if (!rtb_grow(b, (size_t)n)) { va_end(ap2); return 0; }
    vsnprintf(b->data + b->off, b->cap - b->off, fmt, ap2);
    va_end(ap2);
    b->off += (size_t)n;
    return 1;
}

static const struct rtnl_attr *rtnl_attr_lookup(const struct rtnl_attr *table, uint16_t id)
{
    for (const struct rtnl_attr *e = table; e->name; e++) {
        if (e->id == id) return e;
    }
    return NULL;
}

static void render_mac(struct rtb *b, const uint8_t *p, size_t plen)
{
    if (plen != 6) {
        rtb_appendf(b, "(%zu bytes)", plen);
        for (size_t i = 0; i < plen && i < 16; i++) rtb_appendf(b, " %02x", p[i]);
        return;
    }
    rtb_appendf(b, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
}

static void render_ip(struct rtb *b, const uint8_t *p, size_t plen)
{
    char buf[64];
    if (plen == 4) {
        if (inet_ntop(AF_INET, p, buf, sizeof(buf))) {
            rtb_appendf(b, "%s", buf);
            return;
        }
    } else if (plen == 16) {
        if (inet_ntop(AF_INET6, p, buf, sizeof(buf))) {
            rtb_appendf(b, "%s", buf);
            return;
        }
    }
    rtb_appendf(b, "(%zu bytes)", plen);
    size_t show = plen < 16 ? plen : 16;
    for (size_t i = 0; i < show; i++) rtb_appendf(b, " %02x", p[i]);
}

static void render_attr_value(struct rtb *b, const struct rtnl_attr *meta,
                              const struct nlattr *na)
{
    size_t plen = (size_t)NLA_PAYLOAD(na);
    const uint8_t *p = NLA_DATA(na);
    enum rtnl_kind k = meta ? meta->kind : RT_BIN;
    switch (k) {
    case RT_FLAG:
        rtb_appendf(b, "(flag)");
        return;
    case RT_U8:
        rtb_appendf(b, "%u", plen ? p[0] : 0u);
        return;
    case RT_U16: {
        uint16_t v = 0;
        if (plen >= 2) memcpy(&v, p, 2);
        rtb_appendf(b, "%u", v);
        return;
    }
    case RT_U32: {
        uint32_t v = 0;
        if (plen >= 4) memcpy(&v, p, 4);
        rtb_appendf(b, "%u", v);
        return;
    }
    case RT_S32: {
        int32_t v = 0;
        if (plen >= 4) memcpy(&v, p, 4);
        rtb_appendf(b, "%d", v);
        return;
    }
    case RT_U64: {
        uint64_t v = 0;
        if (plen >= 8) memcpy(&v, p, 8);
        rtb_appendf(b, "%llu", (unsigned long long)v);
        return;
    }
    case RT_STR: {
        size_t real = plen;
        if (real && p[real - 1] == '\0') real--;
        rtb_appendf(b, "\"%.*s\"", (int)real, (const char *)p);
        return;
    }
    case RT_BIN: {
        /* Heuristic: 6 bytes -> MAC, 4 or 16 bytes -> IP, else hex. */
        if (meta && (strcmp(meta->name, "ADDRESS") == 0 ||
                     strcmp(meta->name, "BROADCAST") == 0 ||
                     strcmp(meta->name, "LLADDR") == 0 ||
                     strcmp(meta->name, "PERM_ADDRESS") == 0)) {
            render_mac(b, p, plen);
            return;
        }
        if (plen == 4 || plen == 16) {
            render_ip(b, p, plen);
            return;
        }
        rtb_appendf(b, "(%zu bytes)", plen);
        size_t show = plen < 16 ? plen : 16;
        for (size_t i = 0; i < show; i++) rtb_appendf(b, " %02x", p[i]);
        return;
    }
    case RT_NEST:
        rtb_appendf(b, "(nested, %zu bytes)", plen);
        return;
    }
}

/*=============================================================================
 * Per-message-type header summary.  Handy at the top of each entry so the
 * user sees the ifindex / family / flags before the attributes.
 *===========================================================================*/

/* Render the fixed-size per-message header the kernel prepended to this
 * reply. These are READ-ONLY response fields — emphatically not arguments
 * the user passes. We format them vertically under a labelled section so
 * the output can't be confused with `--arg KEY=VALUE` syntax. */
static void render_msg_header(struct rtb *b, const struct rtnl_op_meta *meta,
                              const void *hdr_data)
{
    if (meta->msg_type == RTM_GETLINK || meta->msg_type == RTM_NEWLINK ||
        meta->msg_type == RTM_DELLINK) {
        const struct ifinfomsg *ifi = hdr_data;
        rtb_appendf(b, "  [response header — ifinfomsg, read-only]\n");
        rtb_appendf(b, "    ifi_family : %u\n", ifi->ifi_family);
        rtb_appendf(b, "    ifi_type   : %u\n", ifi->ifi_type);
        rtb_appendf(b, "    ifi_index  : %d\n", ifi->ifi_index);
        rtb_appendf(b, "    ifi_flags  : 0x%08x\n", ifi->ifi_flags);
        rtb_appendf(b, "    ifi_change : 0x%08x\n", ifi->ifi_change);
    } else if (meta->msg_type == RTM_GETADDR || meta->msg_type == RTM_NEWADDR ||
               meta->msg_type == RTM_DELADDR) {
        const struct ifaddrmsg *ifa = hdr_data;
        rtb_appendf(b, "  [response header — ifaddrmsg, read-only]\n");
        rtb_appendf(b, "    ifa_family    : %u\n", ifa->ifa_family);
        rtb_appendf(b, "    ifa_prefixlen : %u\n", ifa->ifa_prefixlen);
        rtb_appendf(b, "    ifa_flags     : 0x%02x\n", ifa->ifa_flags);
        rtb_appendf(b, "    ifa_scope     : %u\n", ifa->ifa_scope);
        rtb_appendf(b, "    ifa_index     : %u\n", ifa->ifa_index);
    } else if (meta->msg_type == RTM_GETROUTE || meta->msg_type == RTM_NEWROUTE ||
               meta->msg_type == RTM_DELROUTE) {
        const struct rtmsg *rtm = hdr_data;
        rtb_appendf(b, "  [response header — rtmsg, read-only]\n");
        rtb_appendf(b, "    rtm_family   : %u\n", rtm->rtm_family);
        rtb_appendf(b, "    rtm_dst_len  : %u\n", rtm->rtm_dst_len);
        rtb_appendf(b, "    rtm_src_len  : %u\n", rtm->rtm_src_len);
        rtb_appendf(b, "    rtm_tos      : %u\n", rtm->rtm_tos);
        rtb_appendf(b, "    rtm_table    : %u\n", rtm->rtm_table);
        rtb_appendf(b, "    rtm_protocol : %u\n", rtm->rtm_protocol);
        rtb_appendf(b, "    rtm_scope    : %u\n", rtm->rtm_scope);
        rtb_appendf(b, "    rtm_type     : %u\n", rtm->rtm_type);
        rtb_appendf(b, "    rtm_flags    : 0x%08x\n", rtm->rtm_flags);
    } else if (meta->msg_type == RTM_GETNEIGH || meta->msg_type == RTM_NEWNEIGH ||
               meta->msg_type == RTM_DELNEIGH) {
        const struct ndmsg *nd = hdr_data;
        rtb_appendf(b, "  [response header — ndmsg, read-only]\n");
        rtb_appendf(b, "    ndm_family  : %u\n", nd->ndm_family);
        rtb_appendf(b, "    ndm_ifindex : %d\n", nd->ndm_ifindex);
        rtb_appendf(b, "    ndm_state   : 0x%04x\n", nd->ndm_state);
        rtb_appendf(b, "    ndm_flags   : 0x%02x\n", nd->ndm_flags);
        rtb_appendf(b, "    ndm_type    : %u\n", nd->ndm_type);
    }
}

/*=============================================================================
 * Socket handling — open NETLINK_ROUTE on demand, cache on the namespace.
 *===========================================================================*/

static int rtnl_socket_get(struct ykern_ynetlink_rtnl_namespace *ns)
{
    if (ns->sock_fd >= 0) return ns->sock_fd;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -errno;
    struct sockaddr_nl src = {.nl_family = AF_NETLINK};
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    ns->sock_fd = fd;
    yinfo("rtnl: socket opened (fd=%d)", fd);
    return fd;
}

static struct ykern_ynetlink_rtnl_namespace *find_rtnl_namespace(struct ykern_ycore_object *node)
{
    while (node && node->kind != YKERN_YCORE_OBJECT_KIND_NAMESPACE) {
        node = node->parent;
    }
    if (!node) return NULL;
    return ykern_container_of(node, struct ykern_ynetlink_rtnl_namespace, base);
}

/*=============================================================================
 * Operation node — invoke does the real work.
 *===========================================================================*/

static struct ykern_ycore_text_result rtnl_op_get_name(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_rtnl_op *op =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_op, base);
    return ykern_ycore_text_from_cstr(op->meta->name);
}

static struct ykern_ycore_text_result rtnl_op_get_short(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_rtnl_op *op =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_op, base);
    return ykern_ycore_text_from_cstr(op->meta->short_desc);
}

static struct ykern_ycore_text_result rtnl_op_get_long(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_rtnl_op *op =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_op, base);
    size_t attr_count = 0;
    for (const struct rtnl_attr *e = op->meta->attrs; e->name; e++) attr_count++;
    return ykern_ycore_text_format(
        "**%s** — %s. (rtnetlink message type %u; classic netlink, not generic.)\n\n"
        "How to call it:\n"
        "  ykern invoke /netlink/rtnl/%s\n"
        "      No --arg needed (and none accepted yet). This sends RTM_%s\n"
        "      with NLM_F_DUMP and prints every object the kernel returns.\n\n"
        "What you'll see in the response:\n"
        "  [response header — ...msg, read-only]   the fixed-size per-message\n"
        "      kernel header (ifinfomsg / ifaddrmsg / rtmsg / ndmsg).  These\n"
        "      are NOT input args — they are values the kernel reports back.\n"
        "  NAME = value                            one line per nlattr the\n"
        "      kernel returned, decoded against the %s attribute set\n"
        "      (%zu known attrs; unknown ids show as '? = (attr N) ...').",
        op->meta->name, op->meta->short_desc, op->meta->msg_type,
        op->meta->name, op->meta->name,
        op->meta->name + 3 /* skip "GET" */, attr_count);
}

static struct ykern_ycore_void_result rtnl_op_populate(struct ykern_ycore_object *self)
{
    /* Spawn one ATTRIBUTE child per known attr in the op's set so the user
     * sees what to expect in the response. */
    struct ykern_ynetlink_rtnl_op *op =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_op, base);
    for (const struct rtnl_attr *e = op->meta->attrs; e->name; e++) {
        const char *type_str = "binary";
        switch (e->kind) {
        case RT_U8:   type_str = "u8";     break;
        case RT_U16:  type_str = "u16";    break;
        case RT_U32:  type_str = "u32";    break;
        case RT_U64:  type_str = "u64";    break;
        case RT_S32:  type_str = "s32";    break;
        case RT_STR:  type_str = "string"; break;
        case RT_BIN:  type_str = "binary"; break;
        case RT_NEST: type_str = "nest";   break;
        case RT_FLAG: type_str = "flag";   break;
        }
        struct ykern_ynetlink_genl_attribute_ptr_result ar =
            ykern_ynetlink_genl_attribute_create(e->id, e->name, op->meta->name,
                                                  type_str);
        YKERN_RETURN_IF_ERR(ykern_ycore_void, ar, "rtnl op_populate: attr create");
        struct ykern_ycore_void_result append =
            ykern_ycore_object_append_child(self, &ar.value->base);
        if (YKERN_IS_ERR(append)) {
            ykern_ycore_object_destroy(&ar.value->base);
            return YKERN_ERR(ykern_ycore_void, "rtnl op_populate: append", append);
        }
    }
    return YKERN_OK_VOID();
}

static struct ykern_ycore_text_result rtnl_op_invoke(struct ykern_ycore_object *self,
                                                     const struct ykern_ycore_invoke_args *args)
{
    (void)args; /* dump-only for now */
    struct ykern_ynetlink_rtnl_op *op =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_op, base);
    struct ykern_ynetlink_rtnl_namespace *ns = find_rtnl_namespace(self);
    if (!ns) {
        return YKERN_ERR(ykern_ycore_text, "rtnl invoke: no namespace ancestor");
    }

    int fd = rtnl_socket_get(ns);
    if (fd < 0) {
        return ykern_ycore_text_format("rtnl invoke: socket failed (%s)",
                                       strerror(-fd));
    }

    /* Build request: nlmsghdr | rtgenmsg.  The kernel inspects only the
     * family field on dumps. */
    struct {
        struct nlmsghdr nh;
        struct rtgenmsg rt;
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = sizeof(req);
    req.nh.nlmsg_type = op->meta->msg_type;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_seq = ++ns->seq;
    req.nh.nlmsg_pid = 0;
    req.rt.rtgen_family = op->meta->default_family;

    struct sockaddr_nl dst = {.nl_family = AF_NETLINK};
    if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        return ykern_ycore_text_format("rtnl invoke: sendto failed (%s)", strerror(errno));
    }

    /* Drain reply stream. */
    char buf[65536];
    struct rtb out = {0};
    rtb_appendf(&out, "%s reply (rtnl msg_type=%u, family=%u) [DUMP]:\n",
                op->meta->name, op->meta->msg_type, op->meta->default_family);

    int reply_count = 0;
    int done = 0;
    while (!done) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            free(out.data);
            return ykern_ycore_text_format("rtnl invoke: recv failed (%s)", strerror(errno));
        }
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        int rem = (int)n;
        while (NLMSG_OK(nh, rem)) {
            if (nh->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(nh);
                if (err->error == 0) { nh = NLMSG_NEXT(nh, rem); continue; }
                int code = -err->error;
                struct ykern_ycore_text_result r = ykern_ycore_text_format(
                    "%s: kernel returned errno %d (%s) after %d %s",
                    op->meta->name, code, strerror(code),
                    reply_count, reply_count == 1 ? "reply" : "replies");
                free(out.data);
                return r;
            }
            void *hdr_data = NLMSG_DATA(nh);
            rtb_appendf(&out, "\n--- entry %d ---\n", reply_count + 1);
            render_msg_header(&out, op->meta, hdr_data);

            struct nlattr *na =
                (struct nlattr *)((char *)hdr_data + NLMSG_ALIGN(op->meta->hdr_skip));
            int attr_rem = (int)nh->nlmsg_len - (int)NLMSG_HDRLEN -
                           (int)NLMSG_ALIGN(op->meta->hdr_skip);
            while (NLA_OK(na, attr_rem)) {
                uint16_t t = na->nla_type & NLA_TYPE_MASK;
                const struct rtnl_attr *meta = rtnl_attr_lookup(op->meta->attrs, t);
                rtb_appendf(&out, "  %s = ", meta ? meta->name : "?");
                if (!meta) rtb_appendf(&out, "(attr %u) ", (unsigned)t);
                render_attr_value(&out, meta, na);
                rtb_appendf(&out, "\n");
                na = NLA_NEXT(na, attr_rem);
            }
            reply_count++;
            nh = NLMSG_NEXT(nh, rem);
        }
    }
    rtb_appendf(&out, "\n(%d %s)\n", reply_count,
                reply_count == 1 ? "object" : "objects");
    if (!out.data) {
        return YKERN_ERR(ykern_ycore_text, "rtnl invoke: oom");
    }
    return YKERN_OK(ykern_ycore_text,
                    ((struct ykern_ycore_text){.data = out.data, .size = out.off}));
}

static void rtnl_op_destroy(struct ykern_ycore_object *self) { (void)self; }

static const struct ykern_ycore_object_ops rtnl_op_ops = {
    .get_name = rtnl_op_get_name,
    .get_short_description = rtnl_op_get_short,
    .get_long_description = rtnl_op_get_long,
    .populate_children = rtnl_op_populate,
    .invoke = rtnl_op_invoke,
    .destroy_impl = rtnl_op_destroy,
};

/*=============================================================================
 * Namespace node — populates with the four ops above.
 *===========================================================================*/

static struct ykern_ycore_text_result rtnl_ns_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("rtnl");
}

static struct ykern_ycore_text_result rtnl_ns_get_short(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "rtnetlink — classic netlink for interfaces, addresses, routes, neighbours");
}

static struct ykern_ycore_text_result rtnl_ns_get_long(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "rtnetlink (NETLINK_ROUTE) — the transport that backs `ip link`,\n"
        "`ip addr`, `ip route`, `ip neigh`. Unlike generic netlink it has no\n"
        "family registry; you talk directly via numeric RTM_* message types.\n\n"
        "Each child here is one such message. ykern's invoke sends a DUMP\n"
        "with the appropriate per-message header and decodes the response\n"
        "into the named attributes shown as the op's children.");
}

static struct ykern_ycore_void_result rtnl_ns_populate(struct ykern_ycore_object *self)
{
    for (size_t i = 0; i < RTNL_OP_COUNT; i++) {
        struct ykern_ynetlink_rtnl_op *op = calloc(1, sizeof(*op));
        if (!op) return YKERN_ERR(ykern_ycore_void, "rtnl_ns_populate: calloc op");
        op->base.ops = &rtnl_op_ops;
        op->base.kind = YKERN_YCORE_OBJECT_KIND_OPERATION;
        op->meta = &rtnl_ops[i];
        struct ykern_ycore_void_result r =
            ykern_ycore_object_append_child(self, &op->base);
        if (YKERN_IS_ERR(r)) {
            free(op);
            return YKERN_ERR(ykern_ycore_void, "rtnl_ns_populate: append", r);
        }
    }
    return YKERN_OK_VOID();
}

static void rtnl_ns_destroy(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_rtnl_namespace *ns =
        ykern_container_of(self, struct ykern_ynetlink_rtnl_namespace, base);
    if (ns->sock_fd >= 0) {
        close(ns->sock_fd);
        ns->sock_fd = -1;
    }
}

static const struct ykern_ycore_object_ops rtnl_ns_ops = {
    .get_name = rtnl_ns_get_name,
    .get_short_description = rtnl_ns_get_short,
    .get_long_description = rtnl_ns_get_long,
    .populate_children = rtnl_ns_populate,
    .destroy_impl = rtnl_ns_destroy,
};

/*=============================================================================
 * Public entry point — used by ynetlink.c to attach the rtnl namespace under
 * the netlink transport.
 *===========================================================================*/

struct ykern_ycore_object_ptr_result ykern_ynetlink_rtnl_namespace_create(void)
{
    struct ykern_ynetlink_rtnl_namespace *ns = calloc(1, sizeof(*ns));
    if (!ns) {
        return YKERN_ERR(ykern_ycore_object_ptr, "rtnl_namespace_create: calloc");
    }
    ns->base.ops = &rtnl_ns_ops;
    ns->base.kind = YKERN_YCORE_OBJECT_KIND_NAMESPACE;
    ns->sock_fd = -1;
    return YKERN_OK(ykern_ycore_object_ptr, &ns->base);
}
