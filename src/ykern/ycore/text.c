#include <ykern/ycore/text.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ykern_ycore_text_result ykern_ycore_text_from_cstr(const char *s)
{
    if (!s) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_text_from_cstr: NULL source");
    }
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    if (!buf) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_text_from_cstr: malloc failed");
    }
    memcpy(buf, s, n + 1);
    return YKERN_OK(ykern_ycore_text, ((struct ykern_ycore_text){.data = buf, .size = n}));
}

struct ykern_ycore_text_result ykern_ycore_text_format(const char *fmt, ...)
{
    if (!fmt) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_text_format: NULL fmt");
    }

    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int needed = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);

    if (needed < 0) {
        va_end(ap2);
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_text_format: vsnprintf sizing failed");
    }

    size_t n = (size_t)needed;
    char *buf = malloc(n + 1);
    if (!buf) {
        va_end(ap2);
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_text_format: malloc failed");
    }
    vsnprintf(buf, n + 1, fmt, ap2);
    va_end(ap2);

    return YKERN_OK(ykern_ycore_text, ((struct ykern_ycore_text){.data = buf, .size = n}));
}

void ykern_ycore_text_destroy(struct ykern_ycore_text *t)
{
    if (!t) {
        return;
    }
    free(t->data);
    t->data = NULL;
    t->size = 0;
}
