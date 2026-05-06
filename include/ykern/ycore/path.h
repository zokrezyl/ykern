#ifndef YKERN_YCORE_PATH_H
#define YKERN_YCORE_PATH_H

/*
 * Path helpers.
 *
 * Paths are simple slash-separated strings: "/<transport>/<...>". The first
 * segment names the transport handler; for filesystem-backed transports the
 * kernel's native path is preserved verbatim, which produces an intentional
 * doubled segment ("/proc/proc/cpuinfo"). See docs/paths.md.
 *
 * ykern_ycore_path_join builds a child path from a parent path and a segment.
 * The parent walks of object->parent in object.c use this.
 */

#include <ykern/ycore/text.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ycore_text_result ykern_ycore_path_join(const char *parent_path, const char *segment);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_PATH_H */
