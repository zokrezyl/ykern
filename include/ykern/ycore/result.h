#ifndef YKERN_YCORE_RESULT_H
#define YKERN_YCORE_RESULT_H

/*
 * Result type machinery — mirrors yetty's, renamed to ykern.
 *
 * - YKERN_YRESULT_DECLARE(name, value_type) generates struct <name>_result
 *   holding either a value of `value_type` or an error.
 * - struct ykern_ycore_error carries a heap-linked cause chain. The top of
 *   the chain lives by value inside a Result; deeper levels are heap-allocated.
 * - YKERN_ERR(type, msg [, prev_res]) builds an error Result. The 3-arg form
 *   transfers ownership of prev_res's cause chain into the new error.
 * - YKERN_RETURN_IF_ERR(type, res, msg) is the propagate-with-context idiom.
 *   Use it whenever there's no cleanup to do between the check and the return.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ykern_ycore_error {
    const char *msg;
    struct ykern_ycore_error *cause; /* heap; NULL = end of chain */
};

#define YKERN_YRESULT_DECLARE(type, value_type)                                                    \
    struct type##_result {                                                                        \
        int ok;                                                                                   \
        union {                                                                                   \
            value_type value;                                                                     \
            struct ykern_ycore_error error;                                                       \
        };                                                                                        \
    }

YKERN_YRESULT_DECLARE(ykern_ycore_void, int);
YKERN_YRESULT_DECLARE(ykern_ycore_int, int);
YKERN_YRESULT_DECLARE(ykern_ycore_size, size_t);

struct ykern_ycore_error *ykern_ycore_error_chain(struct ykern_ycore_error prev);
void ykern_ycore_error_destroy(struct ykern_ycore_error err);

#define YKERN_OK_VOID() ((struct ykern_ycore_void_result){.ok = 1, .value = 0})
#define YKERN_OK(type, val) ((struct type##_result){.ok = 1, .value = (val)})

#define YKERN_ERR(...) YKERN_ERR_DISPATCH(__VA_ARGS__, YKERN_ERR_3, YKERN_ERR_2)(__VA_ARGS__)
#define YKERN_ERR_DISPATCH(_1, _2, _3, NAME, ...) NAME

#define YKERN_ERR_2(type, err_msg)                                                                 \
    ((struct type##_result){.ok = 0, .error = {.msg = (err_msg), .cause = NULL}})

#define YKERN_ERR_3(type, err_msg, prev_res)                                                       \
    ((struct type##_result){                                                                       \
        .ok = 0,                                                                                   \
        .error = {.msg = (err_msg), .cause = ykern_ycore_error_chain((prev_res).error)}})

#define YKERN_IS_OK(res) ((res).ok)
#define YKERN_IS_ERR(res) (!(res).ok)

#if defined(__clang__) || defined(__GNUC__)
#define YKERN_EXTERNAL_CALLBACK __attribute__((annotate("ykern_external_callback")))
#else
#define YKERN_EXTERNAL_CALLBACK
#endif

#define YKERN_RETURN_IF_ERR(type, res, msg)                                                        \
    do {                                                                                           \
        if (YKERN_IS_ERR(res)) {                                                                   \
            return YKERN_ERR(type, msg, (res));                                                    \
        }                                                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_RESULT_H */
