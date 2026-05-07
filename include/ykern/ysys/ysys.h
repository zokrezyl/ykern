#ifndef YKERN_YSYS_YSYS_H
#define YKERN_YSYS_YSYS_H

/*
 * The /sys (sysfs) transport — exposes the kernel-objects-as-files tree
 * under /sys/sys/... in ykern's path-space.
 *
 * Path convention (per docs/paths.md): the first segment is the transport
 * handler ("sys"), and from segment 2 onward we reproduce the kernel's own
 * path verbatim.  That's why /sys/sys/class/net/eth0 maps to the on-disk
 * /sys/class/net/eth0 — the doubled "sys" is intentional.
 *
 * Browse a directory to list its entries; browse a file to see its
 * metadata; invoke a file to read its contents.
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ysys_ysys;

YKERN_YRESULT_DECLARE(ykern_ysys_ysys_ptr, struct ykern_ysys_ysys *);

/* Construct the /sys transport.  Caller hands the resulting object to
 * ykern_ycore_root_create alongside the netlink transport. */
struct ykern_ysys_ysys_ptr_result ykern_ysys_ysys_create(void);

/* Convenience upcast — the base object is the first field, but a typed
 * helper avoids reinterpret-style casts at call sites. */
struct ykern_ycore_object *ykern_ysys_ysys_as_object(struct ykern_ysys_ysys *self);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YSYS_YSYS_H */
