/*
 * Result-type chain helpers — heap-copy a cause and walk-and-free a chain.
 * Mirrors yetty's implementation, renamed to ykern.
 */

#include <ykern/ycore/result.h>

#include <stdlib.h>

struct ykern_ycore_error *ykern_ycore_error_chain(struct ykern_ycore_error prev)
{
    struct ykern_ycore_error *p = malloc(sizeof(*p));
    if (!p) {
        /* OOM during error wrapping — drop the inner chain so we don't leak.
         * The outer error still surfaces; debug context is lost. */
        ykern_ycore_error_destroy(prev);
        return NULL;
    }
    *p = prev;
    return p;
}

void ykern_ycore_error_destroy(struct ykern_ycore_error err)
{
    struct ykern_ycore_error *p = err.cause;
    while (p) {
        struct ykern_ycore_error *next = p->cause;
        free(p);
        p = next;
    }
}
