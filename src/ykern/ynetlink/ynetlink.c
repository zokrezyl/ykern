#include "ynetlink-internal.h"

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ycore/text.h>
#include <ykern/ycore/trace.h>
#include <ykern/ycore/types.h>

#include <errno.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct ykern_ycore_text_result ynl_get_name(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr("netlink");
}

static struct ykern_ycore_text_result ynl_get_short_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "Netlink — kernel/userspace message bus for configuration and notification");
}

static struct ykern_ycore_text_result ynl_get_long_description(struct ykern_ycore_object *self)
{
    (void)self;
    return ykern_ycore_text_from_cstr(
        "Netlink is the family of socket-based protocols Linux uses to expose "
        "subsystem state and accept configuration. It includes generic netlink "
        "(self-describing families like nl80211, devlink, ethtool), rtnetlink "
        "(routes, links, addresses), and several others. ykern's netlink "
        "transport groups all of these under a single namespace so they can be "
        "browsed uniformly.");
}

static struct ykern_ycore_void_result ynl_populate_children(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_ynetlink *t =
        ykern_container_of(self, struct ykern_ynetlink_ynetlink, base);

    struct ykern_ynetlink_genl_namespace_ptr_result nsr =
        ykern_ynetlink_genl_namespace_create(t);
    YKERN_RETURN_IF_ERR(ykern_ycore_void, nsr,
                        "ynetlink: failed to create genl namespace");

    struct ykern_ycore_void_result ar =
        ykern_ycore_object_append_child(self, &nsr.value->base);
    if (YKERN_IS_ERR(ar)) {
        ykern_ycore_object_destroy(&nsr.value->base);
        return YKERN_ERR(ykern_ycore_void, "ynetlink: append genl namespace failed", ar);
    }
    return YKERN_OK_VOID();
}

static void ynl_destroy_impl(struct ykern_ycore_object *self)
{
    struct ykern_ynetlink_ynetlink *t =
        ykern_container_of(self, struct ykern_ynetlink_ynetlink, base);
    if (t->sock_fd >= 0) {
        close(t->sock_fd);
        t->sock_fd = -1;
    }
}

static const struct ykern_ycore_object_ops ynl_ops = {
    .get_name = ynl_get_name,
    .get_short_description = ynl_get_short_description,
    .get_long_description = ynl_get_long_description,
    .populate_children = ynl_populate_children,
    .destroy_impl = ynl_destroy_impl,
};

struct ykern_ynetlink_ynetlink_ptr_result ykern_ynetlink_ynetlink_create(void)
{
    struct ykern_ynetlink_ynetlink *t = calloc(1, sizeof(*t));
    if (!t) {
        return YKERN_ERR(ykern_ynetlink_ynetlink_ptr,
                         "ykern_ynetlink_ynetlink_create: calloc failed");
    }
    t->base.ops = &ynl_ops;
    t->base.kind = YKERN_YCORE_OBJECT_KIND_TRANSPORT;
    t->sock_fd = -1;
    t->seq = 0;
    yinfo("ynetlink: transport created");
    return YKERN_OK(ykern_ynetlink_ynetlink_ptr, t);
}

struct ykern_ycore_object *ykern_ynetlink_ynetlink_as_object(
    struct ykern_ynetlink_ynetlink *self)
{
    return self ? &self->base : NULL;
}

/*-----------------------------------------------------------------------------
 * Internal: lazy socket + monotonic sequence counter.
 *---------------------------------------------------------------------------*/

struct ykern_ynetlink_ynetlink_socket_result ykern_ynetlink_ynetlink_socket_get(
    struct ykern_ynetlink_ynetlink *self)
{
    if (!self) {
        return YKERN_ERR(ykern_ynetlink_ynetlink_socket, "socket_get: self is NULL");
    }
    if (self->sock_fd >= 0) {
        return YKERN_OK(ykern_ynetlink_ynetlink_socket, self->sock_fd);
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) {
        return YKERN_ERR(ykern_ynetlink_ynetlink_socket, "socket(AF_NETLINK) failed");
    }

    struct sockaddr_nl src = {0};
    src.nl_family = AF_NETLINK;
    src.nl_pid = 0; /* kernel auto-assigns */
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
        close(fd);
        return YKERN_ERR(ykern_ynetlink_ynetlink_socket, "bind(AF_NETLINK) failed");
    }

    self->sock_fd = fd;
    yinfo("ynetlink: socket opened (fd=%d)", fd);
    return YKERN_OK(ykern_ynetlink_ynetlink_socket, fd);
}

uint32_t ykern_ynetlink_ynetlink_next_seq(struct ykern_ynetlink_ynetlink *self)
{
    return ++self->seq;
}
