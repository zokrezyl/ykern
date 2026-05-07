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

/* Walks the parent chain of `node` until kind == TRANSPORT, returns the
 * containing ynetlink struct.  Returns NULL if the walk ends without finding
 * one (which would indicate a misconfigured tree). */
struct ykern_ynetlink_ynetlink *ykern_ynetlink_find_transport(
    struct ykern_ycore_object *node);

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
 * Generic netlink family node.
 *
 * Carries the scalar identity (name/id/version/hdrsize/maxattr) plus two
 * raw arrays parsed out of the CTRL_CMD_GETFAMILY dump:
 *
 *   - ops          — operations advertised by the family (cmd id + flags).
 *                    The kernel does not return op names; without policy or
 *                    YAML metadata we display them as "cmd-<id>".
 *   - mcast_groups — multicast groups the family broadcasts on (id + name).
 *
 * populate_children turns each entry into a child node.
 *---------------------------------------------------------------------------*/

struct ykern_ynetlink_genl_op_info {
    uint32_t id;
    uint32_t flags;
};

struct ykern_ynetlink_genl_mcast_info {
    uint32_t id;
    char *name; /* owned, NUL-terminated; may be NULL if kernel didn't send one */
};

struct ykern_ynetlink_genl_family_overlay; /* fwd; defined in genl-overlay.h */

struct ykern_ynetlink_genl_family {
    struct ykern_ycore_object base;
    char *name; /* owned */
    uint16_t id;
    uint32_t version;
    uint32_t hdrsize;
    uint32_t maxattr;

    struct ykern_ynetlink_genl_op_info *ops; /* owned */
    size_t op_count;

    struct ykern_ynetlink_genl_mcast_info *mcast_groups; /* owned */
    size_t mcast_count;

    struct ykern_ynetlink_genl_family_overlay *overlay; /* owned; NULL = none */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_family_ptr, struct ykern_ynetlink_genl_family *);

struct ykern_ynetlink_genl_family_ptr_result ykern_ynetlink_genl_family_create(
    const char *name, uint16_t id, uint32_t version, uint32_t hdrsize, uint32_t maxattr);

/* Hand the family ownership of the parsed op array. The family frees it on
 * destroy. Replacing previously-set ops is allowed (old array is freed). */
void ykern_ynetlink_genl_family_set_ops(struct ykern_ynetlink_genl_family *f,
                                        struct ykern_ynetlink_genl_op_info *ops, size_t count);

/* Same for multicast groups. The mcast_info entries' `name` fields are also
 * freed on destroy. */
void ykern_ynetlink_genl_family_set_mcast_groups(
    struct ykern_ynetlink_genl_family *f, struct ykern_ynetlink_genl_mcast_info *groups,
    size_t count);

/* Hand the family ownership of its YAML overlay (or NULL = no overlay). The
 * family destroys the overlay on destroy_impl. */
void ykern_ynetlink_genl_family_set_overlay(struct ykern_ynetlink_genl_family *f,
                                            struct ykern_ynetlink_genl_family_overlay *overlay);

/*-----------------------------------------------------------------------------
 * Operation node — one cmd advertised by a family.
 *
 * The overlay_* fields hold the YAML-supplied human name + descriptions, if
 * an overlay entry exists for this cmd_id. NULL means "fall back to the
 * synthetic representation built from cmd_id and flags".
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_operation {
    struct ykern_ycore_object base;
    uint32_t cmd_id;
    uint32_t flags; /* bitmask of GENL_*_PERM / GENL_CMD_CAP_* */
    char *overlay_name;  /* owned; NULL = use "cmd-<id>" */
    char *overlay_short; /* owned; NULL = use auto-generated */
    char *overlay_long;  /* owned; NULL = use auto-generated */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_operation_ptr,
                      struct ykern_ynetlink_genl_operation *);

struct ykern_ynetlink_genl_operation_ptr_result ykern_ynetlink_genl_operation_create(
    uint32_t cmd_id, uint32_t flags, const char *overlay_name, const char *overlay_short,
    const char *overlay_long);

/*-----------------------------------------------------------------------------
 * Multicast group node — one group a family broadcasts on.
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_mcast_group {
    struct ykern_ycore_object base;
    uint32_t group_id;
    char *name;          /* kernel-assigned; owned; may be NULL */
    char *overlay_short; /* owned; NULL = use auto-generated */
    char *overlay_long;  /* owned; NULL = use auto-generated */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_mcast_group_ptr,
                      struct ykern_ynetlink_genl_mcast_group *);

struct ykern_ynetlink_genl_mcast_group_ptr_result ykern_ynetlink_genl_mcast_group_create(
    uint32_t group_id, const char *name, const char *overlay_short, const char *overlay_long);

/*-----------------------------------------------------------------------------
 * Attribute node — leaf describing one named attribute of an operation.
 * Spawned as a child of the operation node so that browsing the op reveals
 * which --arg KEYs the user can pass.
 *---------------------------------------------------------------------------*/
struct ykern_ynetlink_genl_attribute {
    struct ykern_ycore_object base;
    uint32_t attr_id;
    char *name;      /* owned, e.g. "ADDR_IPV4", "IFNAME" */
    char *set_name;  /* owned, the attribute set this attr came from */
    char *type_hint; /* owned, kernel header trailing comment; may be NULL */
};

YKERN_YRESULT_DECLARE(ykern_ynetlink_genl_attribute_ptr,
                      struct ykern_ynetlink_genl_attribute *);

struct ykern_ynetlink_genl_attribute_ptr_result ykern_ynetlink_genl_attribute_create(
    uint32_t attr_id, const char *name, const char *set_name, const char *type_hint);

/*-----------------------------------------------------------------------------
 * rtnetlink (NETLINK_ROUTE) namespace — sibling of genl under /netlink/.
 * Defined in rtnl.c.
 *---------------------------------------------------------------------------*/
struct ykern_ycore_object_ptr_result ykern_ynetlink_rtnl_namespace_create(void);

#endif /* YKERN_YNETLINK_YNETLINK_INTERNAL_H */
