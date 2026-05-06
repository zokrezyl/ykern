#ifndef YKERN_YCORE_TEXT_H
#define YKERN_YCORE_TEXT_H

/*
 * struct ykern_ycore_text — owned, NUL-terminated string buffer.
 *
 * All description/getter functions in ykern return text by value through a
 * Result. `data` is heap-allocated and the caller owns it; free via
 * ykern_ycore_text_destroy. `size` is the byte length (strlen), not capacity.
 *
 * The library always allocates, even when the source is a static literal.
 * No conditional ownership — the caller never has to ask "do I free this?".
 */

#include <stddef.h>
#include <ykern/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ycore_text {
    char *data;
    size_t size;
};

YKERN_YRESULT_DECLARE(ykern_ycore_text, struct ykern_ycore_text);

/* Allocate a copy of a NUL-terminated source string. NULL source -> error. */
struct ykern_ycore_text_result ykern_ycore_text_from_cstr(const char *s);

/* Allocate by printf-style format. */
struct ykern_ycore_text_result ykern_ycore_text_format(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Free the underlying buffer and zero the struct fields. NULL- and
 * already-empty-safe. */
void ykern_ycore_text_destroy(struct ykern_ycore_text *t);

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_TEXT_H */
