#ifndef YKERN_YCORE_TRACE_H
#define YKERN_YCORE_TRACE_H

/*
 * ytrace — switchable trace points with negligible cost when off.
 *
 * Same API as yetty's ytrace: ytrace/ydebug/yinfo/ywarn/yerror printf-style
 * macros plus runtime control via ytrace_set_*_enabled. Set the environment
 * variable YTRACE_DEFAULT_ON=yes to flip every registered point on at startup.
 *
 * Each call site declares a local static bool that is registered on first
 * execution; subsequent calls just check the bool. Disabled points cost a
 * single branch.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YTRACE_C_ENABLED
#define YTRACE_C_ENABLED 1
#endif

#ifndef YTRACE_C_ENABLE_TRACE
#define YTRACE_C_ENABLE_TRACE 1
#endif
#ifndef YTRACE_C_ENABLE_DEBUG
#define YTRACE_C_ENABLE_DEBUG 1
#endif
#ifndef YTRACE_C_ENABLE_INFO
#define YTRACE_C_ENABLE_INFO 1
#endif
#ifndef YTRACE_C_ENABLE_WARN
#define YTRACE_C_ENABLE_WARN 1
#endif
#ifndef YTRACE_C_ENABLE_ERROR
#define YTRACE_C_ENABLE_ERROR 1
#endif

#if YTRACE_C_ENABLED

#ifndef YTRACE_C_MAX_POINTS
#define YTRACE_C_MAX_POINTS 4096
#endif

typedef struct {
    bool *enabled;
    const char *file;
    int line;
    const char *function;
    const char *level;
    const char *message;
} ytrace_point_t;

void ytrace_init(void);
void ytrace_shutdown(void);

bool ytrace_register(bool *enabled, const char *file, int line, const char *func, const char *level,
                     const char *message);

void ytrace_output(const char *level, const char *file, int line, const char *func, const char *fmt,
                   ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

void ytrace_set_all_enabled(bool enabled);
void ytrace_set_level_enabled(const char *level, bool enabled);
void ytrace_set_file_enabled(const char *file, bool enabled);
void ytrace_set_function_enabled(const char *function, bool enabled);

size_t ytrace_get_point_count(void);
const ytrace_point_t *ytrace_get_points(void);

void ytrace_list(void);

/*
 * Each level expands to its own variadic-macro definition. The repetition is
 * deliberate — `__VA_ARGS__` is only legal inside the definition of a
 * variadic macro, so factoring the body out doesn't compile.
 */

#if YTRACE_C_ENABLE_TRACE
#define ytrace(fmt, ...)                                                                           \
    do {                                                                                           \
        static bool _ytrace_enabled_ = false;                                                      \
        static bool _ytrace_registered_ = false;                                                   \
        if (!_ytrace_registered_) {                                                                \
            _ytrace_enabled_ =                                                                     \
                ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "trace", fmt);    \
            _ytrace_registered_ = true;                                                            \
        }                                                                                          \
        if (_ytrace_enabled_) {                                                                    \
            ytrace_output("trace", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);              \
        }                                                                                          \
    } while (0)
#else
#define ytrace(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_DEBUG
#define ydebug(fmt, ...)                                                                           \
    do {                                                                                           \
        static bool _ytrace_enabled_ = false;                                                      \
        static bool _ytrace_registered_ = false;                                                   \
        if (!_ytrace_registered_) {                                                                \
            _ytrace_enabled_ =                                                                     \
                ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "debug", fmt);    \
            _ytrace_registered_ = true;                                                            \
        }                                                                                          \
        if (_ytrace_enabled_) {                                                                    \
            ytrace_output("debug", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);              \
        }                                                                                          \
    } while (0)
#else
#define ydebug(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_INFO
#define yinfo(fmt, ...)                                                                            \
    do {                                                                                           \
        static bool _ytrace_enabled_ = false;                                                      \
        static bool _ytrace_registered_ = false;                                                   \
        if (!_ytrace_registered_) {                                                                \
            _ytrace_enabled_ =                                                                     \
                ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "info", fmt);     \
            _ytrace_registered_ = true;                                                            \
        }                                                                                          \
        if (_ytrace_enabled_) {                                                                    \
            ytrace_output("info", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);               \
        }                                                                                          \
    } while (0)
#else
#define yinfo(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_WARN
#define ywarn(fmt, ...)                                                                            \
    do {                                                                                           \
        static bool _ytrace_enabled_ = false;                                                      \
        static bool _ytrace_registered_ = false;                                                   \
        if (!_ytrace_registered_) {                                                                \
            _ytrace_enabled_ =                                                                     \
                ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "warn", fmt);     \
            _ytrace_registered_ = true;                                                            \
        }                                                                                          \
        if (_ytrace_enabled_) {                                                                    \
            ytrace_output("warn", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);               \
        }                                                                                          \
    } while (0)
#else
#define ywarn(fmt, ...) ((void)0)
#endif

#if YTRACE_C_ENABLE_ERROR
#define yerror(fmt, ...)                                                                           \
    do {                                                                                           \
        static bool _ytrace_enabled_ = false;                                                      \
        static bool _ytrace_registered_ = false;                                                   \
        if (!_ytrace_registered_) {                                                                \
            _ytrace_enabled_ =                                                                     \
                ytrace_register(&_ytrace_enabled_, __FILE__, __LINE__, __func__, "error", fmt);    \
            _ytrace_registered_ = true;                                                            \
        }                                                                                          \
        if (_ytrace_enabled_) {                                                                    \
            ytrace_output("error", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);              \
        }                                                                                          \
    } while (0)
#else
#define yerror(fmt, ...) ((void)0)
#endif

#else /* !YTRACE_C_ENABLED */

#define ytrace(fmt, ...) ((void)0)
#define ydebug(fmt, ...) ((void)0)
#define yinfo(fmt, ...) ((void)0)
#define ywarn(fmt, ...) ((void)0)
#define yerror(fmt, ...) ((void)0)

static inline void ytrace_init(void) {}
static inline void ytrace_shutdown(void) {}
static inline void ytrace_set_all_enabled(bool enabled) { (void)enabled; }
static inline void ytrace_set_level_enabled(const char *level, bool enabled)
{ (void)level; (void)enabled; }
static inline void ytrace_set_file_enabled(const char *file, bool enabled)
{ (void)file; (void)enabled; }
static inline void ytrace_set_function_enabled(const char *function, bool enabled)
{ (void)function; (void)enabled; }
static inline size_t ytrace_get_point_count(void) { return 0; }
static inline const ytrace_point_t *ytrace_get_points(void) { return NULL; }
static inline void ytrace_list(void) {}

#endif /* YTRACE_C_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* YKERN_YCORE_TRACE_H */
