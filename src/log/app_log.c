#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "app_log.h"
#include "app_config.h"
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
#define APP_LOG_HAVE_EXECINFO 1
#include <execinfo.h>
#endif
#endif

#ifndef APP_NAME
#define APP_NAME "App"
#endif
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

enum {
    APP_LOG_DEFAULT_MAX_BYTES = 1024 * 1024, /* 1 MiB */
    APP_LOG_PATH_CAP = 4096,
};

#ifdef _WIN32
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;

static BOOL CALLBACK init_lock_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void log_lock(void) {
    InitOnceExecuteOnce(&g_lock_once, init_lock_cb, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void log_unlock(void) {
    LeaveCriticalSection(&g_lock);
}
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_lock(void) {
    pthread_mutex_lock(&g_lock);
}

static void log_unlock(void) {
    pthread_mutex_unlock(&g_lock);
}
#endif

static FILE *g_file = NULL;
static char g_path[APP_LOG_PATH_CAP] = "";
static size_t g_max_bytes = APP_LOG_DEFAULT_MAX_BYTES;
static size_t g_rotate_threshold = APP_LOG_DEFAULT_MAX_BYTES;
static size_t g_bytes_written = 0;
static app_log_level g_level = APP_LOG_INFO;
static bool g_mirror_all_stderr = false;
#ifdef _WIN32
static wchar_t g_crash_wpath[APP_LOG_PATH_CAP];
static volatile LONG g_crash_has_path = 0;
#else
static volatile sig_atomic_t g_crash_fd = -1;
#endif

static size_t rotate_backoff_step(void) {
    return g_max_bytes > 65536 ? 65536 : g_max_bytes;
}

static const char *level_tag(app_log_level level) {
    switch (level) {
    case APP_LOG_DEBUG: return "DEBUG";
    case APP_LOG_INFO:  return "INFO";
    case APP_LOG_WARN:  return "WARN";
    case APP_LOG_ERROR: return "ERROR";
    default:            return "INFO";
    }
}

#ifdef _WIN32
static bool utf8_to_wide(const char *src, wchar_t *dst, size_t cap) {
    if (!src || !src[0] || !dst || cap == 0) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, (int)cap) > 0;
}

static FILE *open_log_file(const char *path, size_t *out_size) {
    wchar_t wpath[APP_LOG_PATH_CAP];
    if (!utf8_to_wide(path, wpath, APP_LOG_PATH_CAP)) return NULL;
    FILE *f = _wfopen(wpath, L"ab");
    if (!f) return NULL;
    if (out_size) {
        *out_size = 0;
        if (_fseeki64(f, 0, SEEK_END) == 0) {
            __int64 pos = _ftelli64(f);
            if (pos > 0) *out_size = (size_t)pos;
        }
    }
    return f;
}

static bool rotate_log_file_locked(void) {
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    bool moved = false;
    char backup[APP_LOG_PATH_CAP];
    int n = snprintf(backup, sizeof backup, "%s.1", g_path);
    if (n > 0 && (size_t)n < sizeof backup) {
        wchar_t wsrc[APP_LOG_PATH_CAP];
        wchar_t wdst[APP_LOG_PATH_CAP];
        if (utf8_to_wide(g_path, wsrc, APP_LOG_PATH_CAP) &&
            utf8_to_wide(backup, wdst, APP_LOG_PATH_CAP)) {
            moved = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING) != 0;
        }
    }
    g_bytes_written = 0;
    g_file = open_log_file(g_path, &g_bytes_written);
    if (moved) {
        g_rotate_threshold = g_max_bytes;
    } else {
        size_t step = rotate_backoff_step();
        g_rotate_threshold = (g_bytes_written <= SIZE_MAX - step)
            ? g_bytes_written + step : SIZE_MAX;
    }
    return g_file != NULL;
}
#else
static FILE *open_log_file(const char *path, size_t *out_size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "ab");
    if (!f) {
        close(fd);
        return NULL;
    }
    if (out_size) {
        *out_size = 0;
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            *out_size = (size_t)st.st_size;
        }
    }
    return f;
}

static bool rotate_log_file_locked(void) {
    if (g_file) {
        g_crash_fd = -1;
        fclose(g_file);
        g_file = NULL;
    }
    bool moved = false;
    char backup[APP_LOG_PATH_CAP];
    int n = snprintf(backup, sizeof backup, "%s.1", g_path);
    if (n > 0 && (size_t)n < sizeof backup) {
        moved = (rename(g_path, backup) == 0);
    }
    g_bytes_written = 0;
    g_file = open_log_file(g_path, &g_bytes_written);
    g_crash_fd = g_file ? (sig_atomic_t)fileno(g_file) : -1;
    if (moved) {
        g_rotate_threshold = g_max_bytes;
    } else {
        size_t step = rotate_backoff_step();
        g_rotate_threshold = (g_bytes_written <= SIZE_MAX - step)
            ? g_bytes_written + step : SIZE_MAX;
    }
    return g_file != NULL;
}
#endif

bool app_log_init(const char *path, size_t max_bytes) {
    if (!path || !path[0]) return false;
    size_t len = strlen(path);
    if (len + 3 >= sizeof g_path) return false;

    log_lock();
#ifdef _WIN32
    InterlockedExchange(&g_crash_has_path, 0);
#else
    g_crash_fd = -1;
#endif
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    memcpy(g_path, path, len + 1);
    g_max_bytes = max_bytes > 0 ? max_bytes : APP_LOG_DEFAULT_MAX_BYTES;
    g_rotate_threshold = g_max_bytes;
    g_bytes_written = 0;

    g_file = open_log_file(g_path, &g_bytes_written);
    if (g_file && g_bytes_written >= g_max_bytes) {
        (void)rotate_log_file_locked();
    }
    bool ok = (g_file != NULL);
    if (ok) {
#ifdef _WIN32
        if (utf8_to_wide(g_path, g_crash_wpath, APP_LOG_PATH_CAP)) {
            InterlockedExchange(&g_crash_has_path, 1);
        }
#else
        g_crash_fd = (sig_atomic_t)fileno(g_file);
#endif
    } else {
        g_path[0] = '\0';
    }
    log_unlock();
    return ok;
}

void app_log_shutdown(void) {
    log_lock();
#ifdef _WIN32
    InterlockedExchange(&g_crash_has_path, 0);
#else
    g_crash_fd = -1;
#endif
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    g_path[0] = '\0';
    g_bytes_written = 0;
    g_rotate_threshold = g_max_bytes;
    log_unlock();
}

void app_log_set_level(app_log_level level) {
    log_lock();
    g_level = level;
    log_unlock();
}

app_log_level app_log_get_level(void) {
    log_lock();
    app_log_level level = g_level;
    log_unlock();
    return level;
}

void app_log_set_stderr(bool enabled) {
    log_lock();
    g_mirror_all_stderr = enabled;
    log_unlock();
}

const char *app_log_path(void) {
    return g_path;
}

bool app_log_copy_path(char *out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    log_lock();
    size_t len = strlen(g_path);
    bool ok = (len > 0 && len < capacity);
    if (ok) memcpy(out, g_path, len + 1);
    log_unlock();
    return ok;
}

static void format_timestamp(char *buf, size_t cap) {
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    if (localtime_s(&tm_buf, &now) != 0) {
        snprintf(buf, cap, "0000-00-00 00:00:00");
        return;
    }
#else
    if (!localtime_r(&now, &tm_buf)) {
        snprintf(buf, cap, "0000-00-00 00:00:00");
        return;
    }
#endif
    if (strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
        snprintf(buf, cap, "0000-00-00 00:00:00");
    }
}

void app_log_write(app_log_level level, const char *fmt, ...) {
    if (!fmt) return;
    if (level < app_log_get_level()) return;

    char msg_buf[2048];
    va_list args;
    va_start(args, fmt);
    int msg_len = vsnprintf(msg_buf, sizeof msg_buf, fmt, args);
    va_end(args);
    if (msg_len < 0) return;

    size_t actual_len = (size_t)msg_len < sizeof msg_buf ? (size_t)msg_len : sizeof msg_buf - 1;
    while (actual_len > 0 && (msg_buf[actual_len - 1] == '\n' || msg_buf[actual_len - 1] == '\r')) {
        msg_buf[--actual_len] = '\0';
    }

    char ts[32];
    format_timestamp(ts, sizeof ts);

    char line[2128];
    int line_len = snprintf(line, sizeof line, "%s [%s] %s\n", ts, level_tag(level), msg_buf);
    if (line_len <= 0) return;
    size_t write_len = (size_t)line_len < sizeof line ? (size_t)line_len : sizeof line - 1;

    log_lock();
    if (level < g_level) {
        log_unlock();
        return;
    }
    bool to_stderr = g_mirror_all_stderr || level >= APP_LOG_WARN;
    if (to_stderr) {
        fwrite(line, 1, write_len, stderr);
        fflush(stderr);
    }
    if (g_file) {
        if (g_bytes_written > 0 && g_bytes_written + write_len > g_rotate_threshold) {
            (void)rotate_log_file_locked();
        }
        if (g_file) {
            size_t n = fwrite(line, 1, write_len, g_file);
            g_bytes_written += n;
            fflush(g_file);
        }
    }
    log_unlock();
}

/* Async-signal-safe crash formatting helpers (no heap, stdio, or locks). */

static void crash_append_char(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos < cap) buf[(*pos)++] = c;
}

static void crash_append_str(char *buf, size_t cap, size_t *pos, const char *s) {
    if (!s) return;
    while (*s && *pos < cap) buf[(*pos)++] = *s++;
}

static void crash_append_uint(char *buf, size_t cap, size_t *pos, uint64_t v) {
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0 && n < sizeof tmp);
    while (n > 0) crash_append_char(buf, cap, pos, tmp[--n]);
}

static void crash_append_int(char *buf, size_t cap, size_t *pos, int v) {
    uint64_t mag;
    if (v < 0) {
        crash_append_char(buf, cap, pos, '-');
        mag = (uint64_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint64_t)v;
    }
    crash_append_uint(buf, cap, pos, mag);
}

static void crash_append_dec_pad(char *buf, size_t cap, size_t *pos, unsigned v, int width) {
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u && n < (int)sizeof tmp);
    while (n < width) crash_append_char(buf, cap, pos, '0'), --width;
    while (n > 0) crash_append_char(buf, cap, pos, tmp[--n]);
}

static void crash_append_hex(char *buf, size_t cap, size_t *pos, uint64_t v, int min_digits) {
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = hex[v & 0xfu];
        v >>= 4;
    } while (v > 0 && n < (int)sizeof tmp);
    crash_append_str(buf, cap, pos, "0x");
    while (n < min_digits) crash_append_char(buf, cap, pos, '0'), --min_digits;
    while (n > 0) crash_append_char(buf, cap, pos, tmp[--n]);
}

static void crash_append_utc_timestamp(char *buf, size_t cap, size_t *pos) {
    time_t now = time(NULL);
    int64_t secs = now > 0 ? (int64_t)now : 0;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    unsigned hour = (unsigned)(rem / 3600);
    unsigned min = (unsigned)((rem % 3600) / 60);
    unsigned sec = (unsigned)(rem % 60);

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp < 10u ? mp + 3u : mp - 9u;
    y += (m <= 2u) ? 1 : 0;

    crash_append_dec_pad(buf, cap, pos, y > 0 ? (unsigned)y : 0u, 4);
    crash_append_char(buf, cap, pos, '-');
    crash_append_dec_pad(buf, cap, pos, m, 2);
    crash_append_char(buf, cap, pos, '-');
    crash_append_dec_pad(buf, cap, pos, d, 2);
    crash_append_char(buf, cap, pos, ' ');
    crash_append_dec_pad(buf, cap, pos, hour, 2);
    crash_append_char(buf, cap, pos, ':');
    crash_append_dec_pad(buf, cap, pos, min, 2);
    crash_append_char(buf, cap, pos, ':');
    crash_append_dec_pad(buf, cap, pos, sec, 2);
    crash_append_str(buf, cap, pos, " UTC");
}

#ifdef _WIN32

static volatile LONG g_in_crash = 0;
static char g_win32_crash_buf[3072];
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_exception_filter = NULL;
static void (*g_prev_sigabrt)(int) = SIG_DFL;
static bool g_crash_handler_installed = false;

static const char *win32_exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    default:                                 return "UNHANDLED_EXCEPTION";
    }
}

static void write_crash_handle(HANDLE h, const char *buf, size_t len) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (len - off > 65536u) ? 65536u : (DWORD)(len - off);
        DWORD written = 0;
        if (!WriteFile(h, buf + off, chunk, &written, NULL) || written == 0) break;
        off += written;
    }
}

static void write_crash_win32(const char *buf, size_t len) {
    write_crash_handle(GetStdHandle(STD_ERROR_HANDLE), buf, len);
    if (InterlockedCompareExchange(&g_crash_has_path, 1, 1) == 1) {
        HANDLE f = CreateFileW(g_crash_wpath, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            write_crash_handle(f, buf, len);
            FlushFileBuffers(f);
            CloseHandle(f);
        }
    }
}

static void append_win32_backtrace(char *buf, size_t cap, size_t *pos) {
    void *frames[64];
    USHORT count = CaptureStackBackTrace(0, 64, frames, NULL);
    if (count == 0) return;
    crash_append_str(buf, cap, pos, "Backtrace (");
    crash_append_uint(buf, cap, pos, count);
    crash_append_str(buf, cap, pos, " frames):\n");
    for (USHORT i = 0; i < count; ++i) {
        crash_append_str(buf, cap, pos, "  #");
        crash_append_uint(buf, cap, pos, i);
        crash_append_char(buf, cap, pos, ' ');
        crash_append_hex(buf, cap, pos, (uintptr_t)frames[i], (int)(sizeof(void *) * 2));
        crash_append_char(buf, cap, pos, '\n');
    }
}

static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *ep) {
    if (InterlockedExchange(&g_in_crash, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char *buf = g_win32_crash_buf;
    size_t cap = sizeof g_win32_crash_buf;
    size_t pos = 0;
    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : NULL;

    crash_append_utc_timestamp(buf, cap, &pos);
    crash_append_str(buf, cap, &pos, " [FATAL] Crash in " APP_NAME " " APP_VERSION ": exception ");
    crash_append_str(buf, cap, &pos, win32_exception_name(code));
    crash_append_str(buf, cap, &pos, " (");
    crash_append_hex(buf, cap, &pos, code, 8);
    crash_append_char(buf, cap, &pos, ')');
    if (addr) {
        crash_append_str(buf, cap, &pos, " at ");
        crash_append_hex(buf, cap, &pos, (uintptr_t)addr, (int)(sizeof(void *) * 2));
    }
    crash_append_char(buf, cap, &pos, '\n');
    if (code != EXCEPTION_STACK_OVERFLOW) {
        append_win32_backtrace(buf, cap, &pos);
    }
    write_crash_win32(buf, pos);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_sigabrt_handler(int sig) {
    if (InterlockedExchange(&g_in_crash, 1) == 0) {
        char *buf = g_win32_crash_buf;
        size_t cap = sizeof g_win32_crash_buf;
        size_t pos = 0;
        crash_append_utc_timestamp(buf, cap, &pos);
        crash_append_str(buf, cap, &pos, " [FATAL] Crash in " APP_NAME " " APP_VERSION ": signal SIGABRT (");
        crash_append_int(buf, cap, &pos, sig);
        crash_append_str(buf, cap, &pos, ")\n");
        append_win32_backtrace(buf, cap, &pos);
        write_crash_win32(buf, pos);
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void app_log_install_crash_handler(void) {
    log_lock();
    if (!g_crash_handler_installed) {
        InterlockedExchange(&g_in_crash, 0);
        g_prev_exception_filter = SetUnhandledExceptionFilter(crash_exception_filter);
        g_prev_sigabrt = signal(SIGABRT, crash_sigabrt_handler);
        if (g_prev_sigabrt == SIG_ERR) g_prev_sigabrt = SIG_DFL;
        g_crash_handler_installed = true;
    }
    log_unlock();
}

void app_log_uninstall_crash_handler(void) {
    log_lock();
    if (g_crash_handler_installed) {
        SetUnhandledExceptionFilter(g_prev_exception_filter);
        g_prev_exception_filter = NULL;
        signal(SIGABRT, g_prev_sigabrt != SIG_ERR ? g_prev_sigabrt : SIG_DFL);
        g_prev_sigabrt = SIG_DFL;
        g_crash_handler_installed = false;
    }
    log_unlock();
}

#else

static const int k_crash_signals[] = {
    SIGSEGV,
    SIGABRT,
    SIGFPE,
    SIGILL,
#ifdef SIGBUS
    SIGBUS,
#endif
};

enum { APP_LOG_CRASH_SIG_COUNT = (int)(sizeof k_crash_signals / sizeof k_crash_signals[0]) };

static struct sigaction g_prev_sigactions[APP_LOG_CRASH_SIG_COUNT];
static unsigned char g_alt_stack[128 * 1024];
static stack_t g_prev_alt_stack;
static bool g_alt_stack_installed = false;
static bool g_crash_handler_installed = false;
static volatile sig_atomic_t g_in_crash = 0;

static const char *posix_signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
#ifdef SIGBUS
    case SIGBUS:  return "SIGBUS";
#endif
    default:      return "SIGNAL";
    }
}

static bool signal_has_fault_addr(int sig) {
    switch (sig) {
    case SIGSEGV:
    case SIGFPE:
    case SIGILL:
#ifdef SIGBUS
    case SIGBUS:
#endif
        return true;
    default:
        return false;
    }
}

static void write_all_fd(int fd, const char *buf, size_t len) {
    if (fd < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void crash_reraise_default(int sig) {
    if (sig > 0) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        (void)sigaction(sig, &dfl, NULL);

        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        (void)pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

        (void)raise(sig);
    }
    _exit(128 + (sig & 0x7f));
}

static void crash_signal_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
#if defined(__GNUC__) || defined(__clang__)
    if (__atomic_exchange_n(&g_in_crash, 1, __ATOMIC_ACQ_REL) != 0) {
        crash_reraise_default(sig);
    }
#else
    if (g_in_crash) {
        crash_reraise_default(sig);
    }
    g_in_crash = 1;
#endif

    char buf[512];
    size_t pos = 0;
    crash_append_utc_timestamp(buf, sizeof buf, &pos);
    crash_append_str(buf, sizeof buf, &pos, " [FATAL] Crash in " APP_NAME " " APP_VERSION ": signal ");
    crash_append_str(buf, sizeof buf, &pos, posix_signal_name(sig));
    crash_append_str(buf, sizeof buf, &pos, " (");
    crash_append_int(buf, sizeof buf, &pos, sig);
    crash_append_char(buf, sizeof buf, &pos, ')');
    if (info && signal_has_fault_addr(sig)) {
        crash_append_str(buf, sizeof buf, &pos, " at ");
        crash_append_hex(buf, sizeof buf, &pos, (uintptr_t)info->si_addr, 1);
    }
    crash_append_char(buf, sizeof buf, &pos, '\n');

    int fd = (int)g_crash_fd;
    if (fd >= 0) write_all_fd(fd, buf, pos);
    if (fd != STDERR_FILENO) write_all_fd(STDERR_FILENO, buf, pos);

#ifdef APP_LOG_HAVE_EXECINFO
    void *frames[64];
    int nframes = backtrace(frames, 64);
    if (nframes > 0) {
        char hdr[64];
        size_t hpos = 0;
        crash_append_str(hdr, sizeof hdr, &hpos, "Backtrace (");
        crash_append_int(hdr, sizeof hdr, &hpos, nframes);
        crash_append_str(hdr, sizeof hdr, &hpos, " frames):\n");
        if (fd >= 0) {
            write_all_fd(fd, hdr, hpos);
            backtrace_symbols_fd(frames, nframes, fd);
        }
        if (fd != STDERR_FILENO) {
            write_all_fd(STDERR_FILENO, hdr, hpos);
            backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
        }
    }
#endif

    crash_reraise_default(sig);
}

void app_log_install_crash_handler(void) {
    log_lock();
    if (!g_crash_handler_installed) {
        g_in_crash = 0;
#ifdef APP_LOG_HAVE_EXECINFO
        /* Warm up libgcc/unwinder outside signal context so backtrace() won't dlopen on crash. */
        void *warmup[1];
        (void)backtrace(warmup, 1);
#endif
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_sp = g_alt_stack;
        ss.ss_size = sizeof g_alt_stack;
        ss.ss_flags = 0;
        g_alt_stack_installed = (sigaltstack(&ss, &g_prev_alt_stack) == 0);

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | (g_alt_stack_installed ? SA_ONSTACK : 0);
        for (int i = 0; i < APP_LOG_CRASH_SIG_COUNT; ++i) {
            (void)sigaction(k_crash_signals[i], &sa, &g_prev_sigactions[i]);
        }
        g_crash_handler_installed = true;
    }
    log_unlock();
}

void app_log_uninstall_crash_handler(void) {
    log_lock();
    if (g_crash_handler_installed) {
        for (int i = 0; i < APP_LOG_CRASH_SIG_COUNT; ++i) {
            (void)sigaction(k_crash_signals[i], &g_prev_sigactions[i], NULL);
        }
        if (g_alt_stack_installed) {
            if (sigaltstack(&g_prev_alt_stack, NULL) != 0) {
                stack_t disable;
                memset(&disable, 0, sizeof disable);
                disable.ss_flags = SS_DISABLE;
                (void)sigaltstack(&disable, NULL);
            }
            g_alt_stack_installed = false;
        }
        g_crash_handler_installed = false;
    }
    log_unlock();
}

#endif
