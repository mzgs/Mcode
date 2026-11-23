#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "updater.h"
#include "app_config.h"
#include "app_log.h"
#include "platform.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
typedef CRITICAL_SECTION updater_mutex;
static void mutex_init(updater_mutex *m) { InitializeCriticalSection(m); }
static void mutex_destroy(updater_mutex *m) { DeleteCriticalSection(m); }
static void mutex_lock(updater_mutex *m) { EnterCriticalSection(m); }
static void mutex_unlock(updater_mutex *m) { LeaveCriticalSection(m); }
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
typedef pthread_mutex_t updater_mutex;
static void mutex_init(updater_mutex *m) { pthread_mutex_init(m, NULL); }
static void mutex_destroy(updater_mutex *m) { pthread_mutex_destroy(m); }
static void mutex_lock(updater_mutex *m) { pthread_mutex_lock(m); }
static void mutex_unlock(updater_mutex *m) { pthread_mutex_unlock(m); }
#endif

#ifndef APP_GITHUB_REPO
#define APP_GITHUB_REPO ""
#endif

struct app_updater {
    updater_mutex mu;
    int refcount;
    bool destroyed;
    bool has_worker;
    bool worker_is_install;
#ifdef _WIN32
    HANDLE worker;
#else
    pthread_t worker;
#endif
    char repo[128];
    char current_version[64];
    app_update_snapshot snap;
};

static bool is_slug_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-';
}

static bool is_tag_char(char c) {
    return is_slug_char(c) || c == '+';
}

bool app_updater_valid_repo(const char *repo) {
    if (!repo || !repo[0]) return false;
    const char *slash = strchr(repo, '/');
    if (!slash || slash == repo || !slash[1]) return false;
    if (strchr(slash + 1, '/')) return false;
    if (strstr(repo, "..")) return false;
    for (const char *p = repo; *p; ++p) {
        if (*p != '/' && !is_slug_char(*p)) return false;
    }
    size_t len = strlen(repo);
    return len < 128;
}

bool app_updater_extract_release_tag(const char *url, char *tag, size_t cap) {
    if (!url || !tag || cap == 0) return false;
    tag[0] = '\0';
    static const char marker[] = "/releases/tag/";
    const char *start = strstr(url, marker);
    if (!start) return false;
    start += sizeof(marker) - 1;

    const char *end = start;
    while (*end && *end != '/' && *end != '?' && *end != '#') ++end;
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= cap) return false;

    for (size_t i = 0; i < len; ++i) {
        if (!is_tag_char(start[i])) return false;
    }
    memcpy(tag, start, len);
    tag[len] = '\0';
    if (strcmp(tag, ".") == 0 || strcmp(tag, "..") == 0 || strstr(tag, "..")) {
        tag[0] = '\0';
        return false;
    }
    return true;
}

typedef struct {
    unsigned long parts[4];
    const char *prerelease;
    size_t prerelease_len;
} parsed_version;

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '-';
}

static bool validate_dot_identifiers(const char *start, const char *end) {
    if (start >= end) return false;
    const char *p = start;
    while (p < end) {
        const char *seg = p;
        while (p < end && *p != '.') {
            if (!is_ident_char(*p)) return false;
            ++p;
        }
        if (p == seg) return false;
        if (p < end) {
            ++p;
            if (p == end) return false;
        }
    }
    return true;
}

static bool parse_version(const char *str, parsed_version *out) {
    if (!str || !out) return false;
    memset(out, 0, sizeof *out);
    if (*str == 'v' || *str == 'V') ++str;
    if (!isdigit((unsigned char)*str)) return false;

    int count = 0;
    const char *p = str;
    while (count < 4) {
        if (!isdigit((unsigned char)*p)) return false;
        unsigned long val = 0;
        while (isdigit((unsigned char)*p)) {
            unsigned long digit = (unsigned long)(*p - '0');
            if (val > (ULONG_MAX - digit) / 10UL) return false;
            val = val * 10UL + digit;
            ++p;
        }
        out->parts[count++] = val;
        if (*p == '.') {
            ++p;
            if (count == 4) return false;
            continue;
        }
        break;
    }
    if (count < 2) return false;
    if (*p == '-') {
        const char *pre_start = p + 1;
        const char *plus = strchr(pre_start, '+');
        const char *pre_end = plus ? plus : pre_start + strlen(pre_start);
        if (!validate_dot_identifiers(pre_start, pre_end)) return false;
        out->prerelease = pre_start;
        out->prerelease_len = (size_t)(pre_end - pre_start);
        p = pre_end;
    }
    if (*p == '+') {
        const char *meta_start = p + 1;
        return validate_dot_identifiers(meta_start, meta_start + strlen(meta_start));
    }
    return *p == '\0';
}

static bool is_all_digits(const char *s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static int compare_prerelease_ident(const char *a, size_t alen, const char *b, size_t blen) {
    bool a_num = is_all_digits(a, alen);
    bool b_num = is_all_digits(b, blen);
    if (a_num && b_num) {
        while (alen > 1 && *a == '0') { ++a; --alen; }
        while (blen > 1 && *b == '0') { ++b; --blen; }
        if (alen != blen) return alen > blen ? 1 : -1;
        int cmp = memcmp(a, b, alen);
        return cmp == 0 ? 0 : (cmp > 0 ? 1 : -1);
    }
    if (a_num != b_num) return a_num ? -1 : 1;
    size_t min_len = alen < blen ? alen : blen;
    int cmp = memcmp(a, b, min_len);
    if (cmp != 0) return cmp > 0 ? 1 : -1;
    if (alen != blen) return alen > blen ? 1 : -1;
    return 0;
}

static int compare_prerelease(const char *a, size_t alen, const char *b, size_t blen) {
    const char *pa = a, *aend = a + alen;
    const char *pb = b, *bend = b + blen;
    while (pa < aend && pb < bend) {
        const char *na = pa;
        while (na < aend && *na != '.') ++na;
        const char *nb = pb;
        while (nb < bend && *nb != '.') ++nb;
        int cmp = compare_prerelease_ident(pa, (size_t)(na - pa), pb, (size_t)(nb - pb));
        if (cmp != 0) return cmp;
        pa = (na < aend) ? na + 1 : aend;
        pb = (nb < bend) ? nb + 1 : bend;
    }
    if (pa < aend) return 1;
    if (pb < bend) return -1;
    return 0;
}

int app_updater_compare_versions(const char *candidate, const char *current) {
    parsed_version a, b;
    if (!parse_version(candidate, &a) || !parse_version(current, &b)) return -2;
    for (int i = 0; i < 4; ++i) {
        if (a.parts[i] > b.parts[i]) return 1;
        if (a.parts[i] < b.parts[i]) return -1;
    }
    bool a_pre = a.prerelease_len > 0;
    bool b_pre = b.prerelease_len > 0;
    if (a_pre != b_pre) return a_pre ? -1 : 1;
    if (a_pre) return compare_prerelease(a.prerelease, a.prerelease_len,
                                         b.prerelease, b.prerelease_len);
    return 0;
}

bool app_updater_configured(void) {
    return app_updater_valid_repo(APP_GITHUB_REPO);
}

bool app_updater_check_due(int64_t last_checked, int64_t now) {
    if (last_checked <= 0 || now <= 0) return true;
    if (now < last_checked) return true;
    return (now - last_checked) >= APP_UPDATE_CHECK_INTERVAL_SEC;
}

static bool url_is_empty_releases_page(const char *url) {
    if (!url) return false;
    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/') --len;
    static const char suffix[] = "/releases";
    size_t slen = sizeof(suffix) - 1;
    return len >= slen && memcmp(url + len - slen, suffix, slen) == 0;
}

static void updater_release(app_updater *u) {
    bool should_free = false;
    mutex_lock(&u->mu);
    if (--u->refcount <= 0) should_free = true;
    mutex_unlock(&u->mu);
    if (should_free) {
        mutex_destroy(&u->mu);
        free(u);
    }
}

app_updater *app_updater_create(const char *repo, const char *current_version) {
    if (!app_updater_valid_repo(repo) || !current_version || !current_version[0]) return NULL;
    app_updater *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    mutex_init(&u->mu);
    u->refcount = 1;
    snprintf(u->repo, sizeof u->repo, "%s", repo);
    snprintf(u->current_version, sizeof u->current_version, "%s", current_version);
    return u;
}

static bool updater_should_abort(void *user) {
    app_updater *u = user;
    mutex_lock(&u->mu);
    bool aborted = u->destroyed;
    mutex_unlock(&u->mu);
    return aborted;
}

static void updater_join_worker(app_updater *u, bool wait_full) {
#ifdef _WIN32
    HANDLE handle = NULL;
    mutex_lock(&u->mu);
    if (u->has_worker) {
        handle = u->worker;
        u->worker = NULL;
        u->has_worker = false;
    }
    mutex_unlock(&u->mu);
    if (handle) {
        WaitForSingleObject(handle, wait_full ? INFINITE : 250);
        CloseHandle(handle);
    }
#else
    (void)wait_full;
    pthread_t tid;
    bool join = false;
    mutex_lock(&u->mu);
    if (u->has_worker) {
        tid = u->worker;
        u->has_worker = false;
        join = true;
    }
    mutex_unlock(&u->mu);
    if (join) {
        (void)pthread_join(tid, NULL);
    }
#endif
}

void app_updater_destroy(app_updater *updater) {
    if (!updater) return;
    mutex_lock(&updater->mu);
    updater->destroyed = true;
    mutex_unlock(&updater->mu);
    updater_join_worker(updater, false);
    updater_release(updater);
}

static void run_check_worker(app_updater *u) {
    char latest_url[256];
    snprintf(latest_url, sizeof latest_url, "https://github.com/%s/releases/latest", u->repo);

    char resolved_url[512] = "";
    char err[256] = "";
    bool fetched = app_http_resolve_redirect(latest_url, resolved_url, sizeof resolved_url,
                                             updater_should_abort, u, err, sizeof err);

    app_update_snapshot next;
    memset(&next, 0, sizeof next);
    if (!fetched) {
        next.state = APP_UPDATE_ERROR;
        snprintf(next.error, sizeof next.error, "%s",
                 err[0] ? err : "Could not check for updates.");
    } else {
        char tag[64] = "";
        if (!app_updater_extract_release_tag(resolved_url, tag, sizeof tag)) {
            if (url_is_empty_releases_page(resolved_url)) {
                next.state = APP_UPDATE_UP_TO_DATE;
                snprintf(next.release_url, sizeof next.release_url, "%s", resolved_url);
            } else {
                next.state = APP_UPDATE_ERROR;
                snprintf(next.error, sizeof next.error, "Could not determine latest release version.");
            }
        } else {
            const char *ver = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
            int cmp = app_updater_compare_versions(ver, u->current_version);
            snprintf(next.latest_tag, sizeof next.latest_tag, "%s", tag);
            snprintf(next.latest_version, sizeof next.latest_version, "%s", ver);
            snprintf(next.release_url, sizeof next.release_url, "%s", resolved_url);
            if (cmp > 0) {
                next.state = APP_UPDATE_AVAILABLE;
                next.has_update = true;
            } else if (cmp == 0 || cmp == -1) {
                next.state = APP_UPDATE_UP_TO_DATE;
            } else {
                next.state = APP_UPDATE_ERROR;
                snprintf(next.error, sizeof next.error, "Unrecognized release version (%s).", tag);
            }
        }
    }

    mutex_lock(&u->mu);
    bool destroyed = u->destroyed;
    if (!destroyed) u->snap = next;
    mutex_unlock(&u->mu);

    if (!destroyed) {
        if (next.state == APP_UPDATE_AVAILABLE) {
            app_log_info("Update available: %s (current %s)", next.latest_tag, u->current_version);
        } else if (next.state == APP_UPDATE_ERROR) {
            app_log_warn("Update check failed: %s", next.error[0] ? next.error : "unknown error");
        }
    }
}

static void run_install_worker(app_updater *u) {
    char tag[64];
    mutex_lock(&u->mu);
    snprintf(tag, sizeof tag, "%s", u->snap.latest_tag);
    mutex_unlock(&u->mu);

    char relaunch_path[4096] = "";
    char err[256] = "";
    bool ok = app_updater_install_release(u->repo, tag, relaunch_path, sizeof relaunch_path,
                                          updater_should_abort, u, err, sizeof err);

    mutex_lock(&u->mu);
    bool destroyed = u->destroyed;
    if (!destroyed) {
        if (ok) {
            u->snap.state = APP_UPDATE_READY_TO_RELAUNCH;
            u->snap.error[0] = '\0';
            snprintf(u->snap.relaunch_path, sizeof u->snap.relaunch_path, "%s", relaunch_path);
        } else {
            u->snap.state = APP_UPDATE_ERROR;
            snprintf(u->snap.error, sizeof u->snap.error, "%s",
                     err[0] ? err : "Could not install update.");
        }
    }
    mutex_unlock(&u->mu);

    if (!destroyed) {
        if (ok) {
            app_log_info("Installed update %s (%s)", tag, relaunch_path);
        } else {
            app_log_error("Update install failed (%s): %s", tag, err[0] ? err : "unknown error");
        }
    }
}

static void run_worker(app_updater *u) {
    if (u->worker_is_install) run_install_worker(u);
    else run_check_worker(u);
    updater_release(u);
}

#ifdef _WIN32
static DWORD WINAPI worker_thread_entry(LPVOID param) {
    run_worker((app_updater *)param);
    return 0;
}

static bool spawn_worker_locked(app_updater *u) {
    HANDLE h = CreateThread(NULL, 0, worker_thread_entry, u, 0, NULL);
    if (!h) return false;
    u->worker = h;
    u->has_worker = true;
    return true;
}
#else
static void *worker_thread_entry(void *param) {
    run_worker((app_updater *)param);
    return NULL;
}

static bool spawn_worker_locked(app_updater *u) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread_entry, u) != 0) return false;
    u->worker = tid;
    u->has_worker = true;
    return true;
}
#endif

static bool updater_start_worker(app_updater *updater, bool install) {
    if (!updater) return false;
    for (;;) {
        mutex_lock(&updater->mu);
        if (updater->destroyed ||
            updater->snap.state == APP_UPDATE_CHECKING ||
            updater->snap.state == APP_UPDATE_UPDATING ||
            updater->snap.state == APP_UPDATE_READY_TO_RELAUNCH ||
            (install && (!updater->snap.has_update || !updater->snap.latest_tag[0]))) {
            mutex_unlock(&updater->mu);
            return false;
        }
        if (updater->has_worker) {
            mutex_unlock(&updater->mu);
            updater_join_worker(updater, true);
            continue;
        }
        if (install) {
            updater->snap.state = APP_UPDATE_UPDATING;
            updater->snap.error[0] = '\0';
        } else {
            memset(&updater->snap, 0, sizeof updater->snap);
            updater->snap.state = APP_UPDATE_CHECKING;
        }
        updater->worker_is_install = install;
        updater->refcount++;
        bool spawned = spawn_worker_locked(updater);
        if (!spawned) {
            updater->snap.state = APP_UPDATE_ERROR;
            snprintf(updater->snap.error, sizeof updater->snap.error, "%s",
                     install ? "Could not start update download."
                             : "Could not start update check.");
        }
        mutex_unlock(&updater->mu);

        if (!spawned) {
            updater_release(updater);
            return false;
        }
        return true;
    }
}

bool app_updater_check(app_updater *updater) {
    return updater_start_worker(updater, false);
}

bool app_updater_start_install(app_updater *updater) {
    return updater_start_worker(updater, true);
}

void app_updater_snapshot(app_updater *updater, app_update_snapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!updater) return;
    mutex_lock(&updater->mu);
    *out = updater->snap;
    mutex_unlock(&updater->mu);
}

bool app_updater_is_busy(app_updater *updater) {
    if (!updater) return false;
    mutex_lock(&updater->mu);
    bool busy = updater->snap.state == APP_UPDATE_CHECKING ||
                updater->snap.state == APP_UPDATE_UPDATING;
    mutex_unlock(&updater->mu);
    return busy;
}

bool app_updater_relaunch(const char *relaunch_path) {
#ifdef _WIN32
    (void)relaunch_path;
    return false;
#else
    if (!relaunch_path || !relaunch_path[0]) return false;
    char pid_str[32];
    snprintf(pid_str, sizeof pid_str, "%ld", (long)getpid());
    long limit = sysconf(_SC_OPEN_MAX);
    int max_fd = limit < STDERR_FILENO + 1 ? 256 : (limit > 4096 ? 4096 : (int)limit);

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        (void)setsid();
        pid_t grand = fork();
        if (grand < 0) _exit(1);
        if (grand > 0) _exit(0);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) {
            close(fd);
        }
        static const char script[] =
            "pid=\"$1\"; target=\"$2\"; i=0; "
            "while kill -0 \"$pid\" 2>/dev/null && [ \"$i\" -lt 100 ]; do "
            "sleep 0.05; i=$((i + 1)); "
            "done; "
#ifdef __APPLE__
            "/usr/bin/open -n \"$target\"";
#else
            "exec \"$target\"";
#endif
        execl("/bin/sh", "sh", "-c", script, "relaunch", pid_str, relaunch_path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
