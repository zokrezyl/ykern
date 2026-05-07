#ifndef YKERN_YPROC_YPROC_H
#define YKERN_YPROC_YPROC_H

/*
 * The /proc (procfs) transport — exposes the kernel-state-as-files tree
 * under /proc/proc/... in ykern's path-space.
 *
 * Path convention (per docs/paths.md): the first segment is the transport
 * handler ("proc"), and from segment 2 onward we reproduce the kernel's own
 * path verbatim.  /proc/proc/cpuinfo therefore maps to the on-disk
 * /proc/cpuinfo — the doubled "proc" is intentional.
 *
 * Browse a directory to list its entries; browse a file to see its
 * metadata; invoke a file to read its contents.
 */

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_yproc_yproc;

YKERN_YRESULT_DECLARE(ykern_yproc_yproc_ptr, struct ykern_yproc_yproc *);

/* Construct the /proc transport.  Caller hands the resulting object to
 * ykern_ycore_root_create alongside the netlink and sysfs transports. */
struct ykern_yproc_yproc_ptr_result ykern_yproc_yproc_create(void);

/* Convenience upcast — the base object is the first field, but a typed
 * helper avoids reinterpret-style casts at call sites. */
struct ykern_ycore_object *ykern_yproc_yproc_as_object(struct ykern_yproc_yproc *self);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YPROC_YPROC_H */
