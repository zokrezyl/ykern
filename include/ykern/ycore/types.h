#ifndef YKERN_YCORE_TYPES_H
#define YKERN_YCORE_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ykern_container_of(ptr, type, member)                                                      \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_TYPES_H */
