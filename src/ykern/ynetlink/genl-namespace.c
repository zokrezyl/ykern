/*
 * Generic netlink namespace — child of the netlink transport.
 *
 * On populate_children, sends CTRL_CMD_GETFAMILY as a NLM_F_DUMP request to
 * the kernel control family (GENL_ID_CTRL) and converts every advertised
 * family into a child genl_family node.
 */

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
        default:
            /* CTRL_ATTR_OPS, CTRL_ATTR_MCAST_GROUPS — skipped for v1. */
            break;
        }
        na = NLA_NEXT(na, attrs_len);
    }

    if (!have_name || !have_id) {
        ywarn("genl dump: family msg missing name or id; skipping");
        return YKERN_OK_VOID();
    }

    struct ykern_ynetlink_genl_family_ptr_result fr =
        ykern_ynetlink_genl_family_create(family_name, family_id, version, hdrsize, maxattr);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, fr, "genl dump: family create failed");

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
