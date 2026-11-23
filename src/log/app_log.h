#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_LOG_DEBUG = 0,
    APP_LOG_INFO,
    APP_LOG_WARN,
    APP_LOG_ERROR,
} app_log_level;

/* Open (or reopen) the log file at UTF-8 path with a size cap in bytes.
 * Pass max_bytes = 0 for the default 1 MiB cap. When the cap would be exceeded,
 * the active log is rotated to "<path>.1" (replacing any previous backup). */
bool app_log_init(const char *path, size_t max_bytes);
void app_log_shutdown(void);

/* Install OS crash handlers (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL on POSIX;
 * unhandled exceptions and SIGABRT on Windows) that append a crash summary and
 * stack trace to the active log file and stderr before letting the OS terminate
 * the process normally. Safe to call multiple times. */
void app_log_install_crash_handler(void);
void app_log_uninstall_crash_handler(void);

/* Minimum level written to the log file (default: APP_LOG_INFO). */
void app_log_set_level(app_log_level level);
app_log_level app_log_get_level(void);

/* Mirror all enabled log levels to stderr (default: false; WARN/ERROR still mirror). */
void app_log_set_stderr(bool enabled);

/* Active log file path, or "" if file logging is inactive.
 * Note: returns a pointer to internal storage; use app_log_copy_path() when
 * app_log_init()/app_log_shutdown() may run concurrently. */
const char *app_log_path(void);
/* Thread-safe copy of the active log path; returns false if inactive or truncated. */
bool app_log_copy_path(char *out, size_t capacity);

#if defined(__GNUC__) || defined(__clang__)
#define APP_LOG_PRINTF_ATTR(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define APP_LOG_PRINTF_ATTR(fmt_idx, args_idx)
#endif

void app_log_write(app_log_level level, const char *fmt, ...) APP_LOG_PRINTF_ATTR(2, 3);

#define app_log_debug(...) app_log_write(APP_LOG_DEBUG, __VA_ARGS__)
#define app_log_info(...)  app_log_write(APP_LOG_INFO,  __VA_ARGS__)
#define app_log_warn(...)  app_log_write(APP_LOG_WARN,  __VA_ARGS__)
#define app_log_error(...) app_log_write(APP_LOG_ERROR, __VA_ARGS__)

#endif
