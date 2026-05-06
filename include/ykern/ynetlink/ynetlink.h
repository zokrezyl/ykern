#ifndef YKERN_YNETLINK_YNETLINK_H
#define YKERN_YNETLINK_YNETLINK_H

/*
 * The netlink transport — root of every netlink-based view of the kernel.
 *
 * Currently exposes one child namespace: generic netlink (`/netlink/genl`).
 * Future extensions will add `rtnl`, `nlroute`, raw netlink families etc. as
 * additional namespace children.
 *
 * The transport opens its kernel socket lazily on first use and keeps it
 * alive for the lifetime of the object.
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ynetlink_ynetlink;

YKERN_YRESULT_DECLARE(ykern_ynetlink_ynetlink_ptr, struct ykern_ynetlink_ynetlink *);

/* Construct a netlink transport. Caller hands the resulting object to
 * ykern_ycore_root_create (cast to struct ykern_ycore_object *) which takes
 * ownership and destroys it on shutdown. */
struct ykern_ynetlink_ynetlink_ptr_result ykern_ynetlink_ynetlink_create(void);

/* Convenience upcast — base is the first field, but a typed helper avoids
 * sprinkling reinterpret-style casts at call sites. */
struct ykern_ycore_object *ykern_ynetlink_ynetlink_as_object(
    struct ykern_ynetlink_ynetlink *self);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YNETLINK_YNETLINK_H */
