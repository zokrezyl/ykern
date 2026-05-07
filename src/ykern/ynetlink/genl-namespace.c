/*
 * Generic netlink namespace — child of the netlink transport.
 *
 * On populate_children, sends CTRL_CMD_GETFAMILY as a NLM_F_DUMP request to
 * the kernel control family (GENL_ID_CTRL) and converts every advertised
 * family into a child genl_family node.
 */

#include "genl-overlay.h"
#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
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

#define GENL_DUMP_BUF_SIZE 65536

/*-----------------------------------------------------------------------------
 * Vtable
 *---------------------------------------------------------------------------*/

static struct ykern_ycore_text_result ns_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("genl");
}

static struct ykern_ycore_text_result ns_get_short_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "Generic netlink — self-describing families (nl80211, devlink, ethtool, ...)");
}

static struct ykern_ycore_text_result ns_get_long_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "Generic netlink (genl) is the multiplexer the kernel exposes for "
        "modern subsystem APIs. Each registered family carries its own command "
        "set, attribute schema, and policy. ykern enumerates them via "
        "CTRL_CMD_GETFAMILY against the GENL_ID_CTRL control family and exposes "
        "each one as a child node.");
}

static int send_dump_request(int fd, uint32_t seq)
{
    struct {
        struct nlmsghdr nlh;
        struct genlmsghdr ghdr;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct genlmsghdr));
    req.nlh.nlmsg_type = GENL_ID_CTRL;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = seq;
    req.nlh.nlmsg_pid = 0;
    req.ghdr.cmd = CTRL_CMD_GETFAMILY;
    req.ghdr.version = 1;

    struct sockaddr_nl dst = {0};
    dst.nl_family = AF_NETLINK;

    ssize_t sent =
        sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) {
        return -errno;
    }
    return 0;
}

/*-----------------------------------------------------------------------------
 * Parse the nested CTRL_ATTR_OPS payload — list of per-op nlattrs, each
 * containing CTRL_ATTR_OP_ID + CTRL_ATTR_OP_FLAGS. Returns owned array.
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result parse_ops_attr(struct nlattr *outer,
                                                     struct ykern_ynetlink_genl_op_info **out_ops,
                                                     size_t *out_count)
{
    *out_ops = NULL;
    *out_count = 0;

    int outer_len = (int)NLA_PAYLOAD(outer);
    if (outer_len <= 0) {
        return YKERN_OK_VOID();
    }

    /* First pass: count entries */
    size_t count = 0;
    {
        int rem = outer_len;
        struct nlattr *cur = (struct nlattr *)NLA_DATA(outer);
        while (NLA_OK(cur, rem)) {
            count++;
            cur = NLA_NEXT(cur, rem);
        }
    }
    if (count == 0) {
        return YKERN_OK_VOID();
    }

    struct ykern_ynetlink_genl_op_info *ops = calloc(count, sizeof(*ops));
    if (!ops) {
        return YKERN_ERR(ykern_ycore_void, "parse_ops_attr: calloc failed");
    }

    /* Second pass: fill */
    int rem = outer_len;
    struct nlattr *cur = (struct nlattr *)NLA_DATA(outer);
    size_t i = 0;
    while (NLA_OK(cur, rem)) {
        int sub_rem = (int)NLA_PAYLOAD(cur);
        struct nlattr *sub = (struct nlattr *)NLA_DATA(cur);
        while (NLA_OK(sub, sub_rem)) {
            uint16_t t = sub->nla_type & NLA_TYPE_MASK;
            if (t == CTRL_ATTR_OP_ID && (size_t)NLA_PAYLOAD(sub) >= sizeof(uint32_t)) {
                memcpy(&ops[i].id, NLA_DATA(sub), sizeof(uint32_t));
            } else if (t == CTRL_ATTR_OP_FLAGS &&
                       (size_t)NLA_PAYLOAD(sub) >= sizeof(uint32_t)) {
                memcpy(&ops[i].flags, NLA_DATA(sub), sizeof(uint32_t));
            }
            sub = NLA_NEXT(sub, sub_rem);
        }
        i++;
        cur = NLA_NEXT(cur, rem);
    }

    *out_ops = ops;
    *out_count = count;
    return YKERN_OK_VOID();
}

/*-----------------------------------------------------------------------------
 * Parse the nested CTRL_ATTR_MCAST_GROUPS payload — list of per-group
 * nlattrs containing CTRL_ATTR_MCAST_GRP_ID + CTRL_ATTR_MCAST_GRP_NAME.
 *---------------------------------------------------------------------------*/
static struct ykern_ycore_void_result parse_mcast_attr(
    struct nlattr *outer, struct ykern_ynetlink_genl_mcast_info **out_groups, size_t *out_count)
{
    *out_groups = NULL;
    *out_count = 0;

    int outer_len = (int)NLA_PAYLOAD(outer);
    if (outer_len <= 0) {
        return YKERN_OK_VOID();
    }

    size_t count = 0;
    {
        int rem = outer_len;
        struct nlattr *cur = (struct nlattr *)NLA_DATA(outer);
        while (NLA_OK(cur, rem)) {
            count++;
            cur = NLA_NEXT(cur, rem);
        }
    }
    if (count == 0) {
        return YKERN_OK_VOID();
    }

    struct ykern_ynetlink_genl_mcast_info *grps = calloc(count, sizeof(*grps));
    if (!grps) {
        return YKERN_ERR(ykern_ycore_void, "parse_mcast_attr: calloc failed");
    }

    int rem = outer_len;
    struct nlattr *cur = (struct nlattr *)NLA_DATA(outer);
    size_t i = 0;
    while (NLA_OK(cur, rem)) {
        int sub_rem = (int)NLA_PAYLOAD(cur);
        struct nlattr *sub = (struct nlattr *)NLA_DATA(cur);
        while (NLA_OK(sub, sub_rem)) {
            uint16_t t = sub->nla_type & NLA_TYPE_MASK;
            if (t == CTRL_ATTR_MCAST_GRP_ID && (size_t)NLA_PAYLOAD(sub) >= sizeof(uint32_t)) {
                memcpy(&grps[i].id, NLA_DATA(sub), sizeof(uint32_t));
            } else if (t == CTRL_ATTR_MCAST_GRP_NAME) {
                size_t plen = (size_t)NLA_PAYLOAD(sub);
                if (plen > 0) {
                    char *name = malloc(plen + 1);
                    if (!name) {
                        /* Free what we've built so far */
                        for (size_t j = 0; j < count; j++) {
                            free(grps[j].name);
                        }
                        free(grps);
                        return YKERN_ERR(ykern_ycore_void, "parse_mcast_attr: malloc failed");
                    }
                    memcpy(name, NLA_DATA(sub), plen);
                    name[plen] = '\0';
                    /* Strip trailing NULs the kernel may have included. */
                    size_t real = strnlen(name, plen);
                    name[real] = '\0';
                    free(grps[i].name); /* in case set twice */
                    grps[i].name = name;
                }
            }
            sub = NLA_NEXT(sub, sub_rem);
        }
        i++;
        cur = NLA_NEXT(cur, rem);
    }

    *out_groups = grps;
    *out_count = count;
    return YKERN_OK_VOID();
}

/* Free a parsed mcast array on error paths. */
static void free_mcast_array(struct ykern_ynetlink_genl_mcast_info *groups, size_t count)
{
    if (!groups) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(groups[i].name);
    }
    free(groups);
}

/* Parse one CTRL_CMD_NEWFAMILY message into a freshly created family node and
 * append it to `parent`. */
static struct ykern_ycore_void_result process_family_msg(struct ykern_ycore_object *parent,
                                                         struct nlmsghdr *nh)
{
    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct genlmsghdr))) {
        return YKERN_ERR(ykern_ycore_void, "genl dump: truncated genlmsghdr");
    }
    struct genlmsghdr *gh = NLMSG_DATA(nh);
    if (gh->cmd != CTRL_CMD_NEWFAMILY) {
        /* Some kernels emit other ctrl commands during a dump; skip them. */
        return YKERN_OK_VOID();
    }

    char family_name[GENL_NAMSIZ + 1] = {0};
    uint16_t family_id = 0;
    uint32_t version = 0;
    uint32_t hdrsize = 0;
    uint32_t maxattr = 0;
    int have_name = 0;
    int have_id = 0;

    struct ykern_ynetlink_genl_op_info *ops = NULL;
    size_t op_count = 0;
    struct ykern_ynetlink_genl_mcast_info *grps = NULL;
    size_t grp_count = 0;

    int attrs_len =
        (int)nh->nlmsg_len - (int)NLMSG_LENGTH((int)NLA_ALIGN(sizeof(struct genlmsghdr)));
    struct nlattr *na =
        (struct nlattr *)((char *)gh + NLA_ALIGN(sizeof(struct genlmsghdr)));

    while (NLA_OK(na, attrs_len)) {
        uint16_t type = na->nla_type & NLA_TYPE_MASK;
        switch (type) {
        case CTRL_ATTR_FAMILY_NAME: {
            size_t plen = (size_t)NLA_PAYLOAD(na);
            if (plen > 0 && plen <= sizeof(family_name)) {
                memcpy(family_name, NLA_DATA(na), plen);
                family_name[sizeof(family_name) - 1] = '\0';
                have_name = 1;
            }
            break;
        }
        case CTRL_ATTR_FAMILY_ID: {
            if ((size_t)NLA_PAYLOAD(na) >= sizeof(uint16_t)) {
                memcpy(&family_id, NLA_DATA(na), sizeof(uint16_t));
                have_id = 1;
            }
            break;
        }
        case CTRL_ATTR_VERSION: {
            if ((size_t)NLA_PAYLOAD(na) >= sizeof(uint32_t)) {
                memcpy(&version, NLA_DATA(na), sizeof(uint32_t));
            }
            break;
        }
        case CTRL_ATTR_HDRSIZE: {
            if ((size_t)NLA_PAYLOAD(na) >= sizeof(uint32_t)) {
                memcpy(&hdrsize, NLA_DATA(na), sizeof(uint32_t));
            }
            break;
        }
        case CTRL_ATTR_MAXATTR: {
            if ((size_t)NLA_PAYLOAD(na) >= sizeof(uint32_t)) {
                memcpy(&maxattr, NLA_DATA(na), sizeof(uint32_t));
            }
            break;
        }
        case CTRL_ATTR_OPS: {
            struct ykern_ycore_void_result r = parse_ops_attr(na, &ops, &op_count);
            if (YKERN_IS_ERR(r)) {
                free_mcast_array(grps, grp_count);
                return YKERN_ERR(ykern_ycore_void, "genl dump: ops parse failed", r);
            }
            break;
        }
        case CTRL_ATTR_MCAST_GROUPS: {
            struct ykern_ycore_void_result r = parse_mcast_attr(na, &grps, &grp_count);
            if (YKERN_IS_ERR(r)) {
                free(ops);
                return YKERN_ERR(ykern_ycore_void, "genl dump: mcast parse failed", r);
            }
            break;
        }
        default:
            /* Other CTRL_ATTR_* — ignored for now. */
            break;
        }
        na = NLA_NEXT(na, attrs_len);
    }

    if (!have_name || !have_id) {
        ywarn("genl dump: family msg missing name or id; skipping");
        free(ops);
        free_mcast_array(grps, grp_count);
        return YKERN_OK_VOID();
    }

    struct ykern_ynetlink_genl_family_ptr_result fr =
        ykern_ynetlink_genl_family_create(family_name, family_id, version, hdrsize, maxattr);
    if (YKERN_IS_ERR(fr)) {
        free(ops);
        free_mcast_array(grps, grp_count);
        return YKERN_ERR(ykern_ycore_void, "genl dump: family create failed", fr);
    }

    /* Hand ownership of ops / mcast arrays to the family. */
    ykern_ynetlink_genl_family_set_ops(fr.value, ops, op_count);
    ykern_ynetlink_genl_family_set_mcast_groups(fr.value, grps, grp_count);

    /* Best-effort overlay load. A missing overlay is normal; only a
     * malformed file is an error, and even then we degrade to the synthetic
     * representation rather than failing the whole dump. */
    struct ykern_ynetlink_genl_family_overlay_ptr_result overlay_r =
        ykern_ynetlink_genl_overlay_load(family_name);
    if (YKERN_IS_OK(overlay_r)) {
        ykern_ynetlink_genl_family_set_overlay(fr.value, overlay_r.value);
    } else {
        ywarn("genl dump: overlay load failed for '%s' (%s); continuing without",
              family_name, overlay_r.error.msg ? overlay_r.error.msg : "?");
        ykern_ycore_error_destroy(overlay_r.error);
    }

    struct ykern_ycore_void_result ar =
        ykern_ycore_object_append_child(parent, &fr.value->base);
    if (YKERN_IS_ERR(ar)) {
        ykern_ycore_object_destroy(&fr.value->base);
        return YKERN_ERR(ykern_ycore_void, "genl dump: append family failed", ar);
    }
    return YKERN_OK_VOID();
}

static struct ykern_ycore_void_result ns_populate_children(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_genl_namespace *ns =
        ykern_container_of(self, struct ykern_ynetlink_genl_namespace, base);

    struct ykern_ynetlink_ynetlink_socket_result sock_r =
        ykern_ynetlink_ynetlink_socket_get(ns->transport);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, sock_r, "genl populate: socket open failed");
    int fd = sock_r.value;
    uint32_t seq = ykern_ynetlink_ynetlink_next_seq(ns->transport);

    int rc = send_dump_request(fd, seq);
    if (rc < 0) {
        return YKERN_ERR(ykern_ycore_void, "genl populate: sendto failed");
    }

    char *buf = malloc(GENL_DUMP_BUF_SIZE);
    if (!buf) {
        return YKERN_ERR(ykern_ycore_void, "genl populate: dump buffer alloc failed");
    }

    int done = 0;
    while (!done) {
        ssize_t n = recv(fd, buf, GENL_DUMP_BUF_SIZE, 0);
        if (n < 0) {
            free(buf);
            return YKERN_ERR(ykern_ycore_void, "genl populate: recv failed");
        }
        if (n == 0) {
            break;
        }

        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        int remaining = (int)n;
        while (NLMSG_OK(nh, remaining)) {
            if (nh->nlmsg_seq != seq) {
                /* Stray reply — ignore and move on. */
                nh = NLMSG_NEXT(nh, remaining);
                continue;
            }
            if (nh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(nh);
                free(buf);
                int code = err ? -err->error : 0;
                (void)code;
                return YKERN_ERR(ykern_ycore_void, "genl populate: kernel returned error");
            }
            struct ykern_ycore_void_result pr = process_family_msg(self, nh);
            if (YKERN_IS_ERR(pr)) {
                free(buf);
                return YKERN_ERR(ykern_ycore_void, "genl populate: family processing failed", pr);
            }
            nh = NLMSG_NEXT(nh, remaining);
        }
    }

    free(buf);
    yinfo("genl populate: %zu families enumerated", self->children_count);
    return YKERN_OK_VOID();
}

static void ns_destroy_impl(struct ykern_ycore_object *self)
{
    /* No impl-specific resources beyond children + base. */
    (void)self;
}

static const struct ykern_ycore_object_ops ns_ops = {
    .get_name = ns_get_name,
    .get_short_description = ns_get_short_description,
    .get_long_description = ns_get_long_description,
    .populate_children = ns_populate_children,
    .destroy_impl = ns_destroy_impl,
};

struct ykern_ynetlink_genl_namespace_ptr_result ykern_ynetlink_genl_namespace_create(
    struct ykern_ynetlink_ynetlink *transport)
{
    if (!transport) {
        return YKERN_ERR(ykern_ynetlink_genl_namespace_ptr,
                         "genl namespace create: transport is NULL");
    }
    struct ykern_ynetlink_genl_namespace *ns = calloc(1, sizeof(*ns));
    if (!ns) {
        return YKERN_ERR(ykern_ynetlink_genl_namespace_ptr,
                         "genl namespace create: calloc failed");
    }
    ns->base.ops = &ns_ops;
    ns->base.kind = YKERN_YCORE_OBJECT_KIND_NAMESPACE;
    ns->transport = transport;
    return YKERN_OK(ykern_ynetlink_genl_namespace_ptr, ns);
}
