#ifndef YKERN_YCORE_ROOT_H
#define YKERN_YCORE_ROOT_H

/*
 * Root factory.
 *
 * The library does not maintain a global registry. Callers construct the
 * root explicitly by passing in an array of transport objects they want
 * exposed; the root takes ownership of those transports and destroys them
 * on shutdown.
 *
 * Typical use:
 *
 *     struct ykern_ynetlink_ynetlink_ptr_result nl =
 *         ykern_ynetlink_ynetlink_create();
 *     // ... check error ...
 *     struct ykern_ycore_object *transports[] = { &nl.value->base };
 *     struct ykern_ycore_object_ptr_result root =
 *         ykern_ycore_root_create(transports, 1);
 */

#include <stddef.h>

#include <ykern/ycore/object.h>
#include <ykern/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ycore_object_ptr_result ykern_ycore_root_create(
    struct ykern_ycore_object **transports, size_t transport_count);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_ROOT_H */
