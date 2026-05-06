#ifndef YKERN_YNETLINK_YNETLINK_INTERNAL_H
#define YKERN_YNETLINK_YNETLINK_INTERNAL_H

/*
 * Internal interfaces shared between the ynetlink module's translation units.
 * Not part of the public API.
 */

#include <stdint.h>

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>
#include <ykern/ynetlink/ynetlink.h>

struct ykern_ynetlink_ynetlink {
    struct ykern_ycore_object base;
    int sock_fd;    /* -1 if not yet opened */
    uint32_t seq;   /* monotonic; bump per request */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_ynetlink_socket, int);

/* Lazy: opens AF_NETLINK / NETLINK_GENERIC socket on first call, caches fd. */
struct ykern_ynetlink_ynetlink_socket_result ykern_ynetlink_ynetlink_socket_get(
    struct ykern_ynetlink_ynetlink *self);

uint32_t ykern_ynetlink_ynetlink_next_seq(struct ykern_ynetlink_ynetlink *self);

/*-----------------------------------------------------------------------------
 * Generic netlink namespace node — a child of the netlink transport.
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_namespace {
    struct ykern_ycore_object base;
    struct ykern_ynetlink_ynetlink *transport; /* weak: parent owns this */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_namespace_ptr,
                      struct ykern_ynetlink_genl_namespace *);

struct ykern_ynetlink_genl_namespace_ptr_result ykern_ynetlink_genl_namespace_create(
    struct ykern_ynetlink_ynetlink *transport);

/*-----------------------------------------------------------------------------
 * Generic netlink family node — a leaf for v1 (ops / attrs come later).
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_family {
    struct ykern_ycore_object base;
    char *name;       /* owned heap copy */
    uint16_t id;
    uint32_t version;
    uint32_t hdrsize;
    uint32_t maxattr;
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_family_ptr, struct ykern_ynetlink_genl_family *);

struct ykern_ynetlink_genl_family_ptr_result ykern_ynetlink_genl_family_create(
    const char *name, uint16_t id, uint32_t version, uint32_t hdrsize, uint32_t maxattr);

#endif /* YKERN_YNETLINK_YNETLINK_INTERNAL_H */
