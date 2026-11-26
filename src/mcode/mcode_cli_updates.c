#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_cli_updates.h"
#include "mcode_proc.h"
#include "app_log.h"
#include "platform.h"
#include "updater.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

/* ---- Pure helpers ---- */

const char *mcode_cli_github_repo(app_agent_kind agent) {
    switch (agent) {
    case APP_AGENT_CLAUDE: return "anthropics/claude-code";
    case APP_AGENT_CODEX:  return "openai/codex";
    case APP_AGENT_AGY:    return "google-antigravity/antigravity-cli";
    default:               return "";
    }
}

static void strip_ansi_copy(const char *src, size_t len, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < cap; ++i) {
        if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len && !((src[i] >= 'A' && src[i] <= 'Z') ||
                                (src[i] >= 'a' && src[i] <= 'z'))) {
                ++i;
            }
            continue;
        }
        dst[w++] = src[i];
    }
    dst[w] = '\0';
}

static bool is_ver_prefix_boundary(const char *start, const char *p) {
    if (p == start) return true;
    unsigned char prev = (unsigned char)p[-1];
    if (!isalnum(prev) && prev != '.') return true;
    /* Allow "v1.2.3", "V1.2.3", "rust-v0.172.0", etc. */
    if (prev == 'v' || prev == 'V') {
        if (p - 1 == start) return true;
        unsigned char before_v = (unsigned char)p[-2];
        if (!isalnum(before_v) && before_v != '.') return true;
    }
    return false;
}

bool mcode_cli_extract_version(const char *raw, char *out, size_t cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!raw || !raw[0] || !out || cap < 4) return false;

    char clean[512];
    strip_ansi_copy(raw, strlen(raw), clean, sizeof clean);

    for (const char *p = clean; *p; ++p) {
        if (!isdigit((unsigned char)*p)) continue;
        if (!is_ver_prefix_boundary(clean, p)) continue;

        const char *q = p;
        int parts = 0;
        while (parts < 4 && isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) ++q;
            ++parts;
            if (*q == '.' && isdigit((unsigned char)q[1]) && parts < 4) {
                ++q;
            } else {
                break;
            }
        }
        if (parts < 2) continue;
        /* Reject if immediately followed by another dot+digit (e.g. >4 numeric segments) or letter without '-'/'+' */
        if (*q == '.' && isdigit((unsigned char)q[1])) continue;
        if (isalpha((unsigned char)*q)) continue;

        /* Optional -prerelease */
        if (*q == '-' && isalnum((unsigned char)q[1])) {
            const char *pre_end = q + 1;
            while (*pre_end && (isalnum((unsigned char)*pre_end) ||
                                *pre_end == '.' || *pre_end == '-')) {
                ++pre_end;
            }
            while (pre_end > q + 1 && (pre_end[-1] == '.' || pre_end[-1] == '-')) {
                --pre_end;
            }
            if (pre_end > q + 1) q = pre_end;
        }
        /* Optional +build */
        if (*q == '+' && isalnum((unsigned char)q[1])) {
            const char *meta_end = q + 1;
            while (*meta_end && (isalnum((unsigned char)*meta_end) ||
                                 *meta_end == '.' || *meta_end == '-')) {
                ++meta_end;
            }
            while (meta_end > q + 1 && (meta_end[-1] == '.' || meta_end[-1] == '-')) {
                --meta_end;
            }
            if (meta_end > q + 1) q = meta_end;
        }

        size_t len = (size_t)(q - p);
        if (len == 0 || len >= cap) continue;
        char cand[64];
        if (len >= sizeof cand) continue;
        memcpy(cand, p, len);
        cand[len] = '\0';

        if (app_updater_compare_versions(cand, cand) == 0) {
            memcpy(out, cand, len + 1);
            return true;
        }
    }
    return false;
}

bool mcode_cli_parse_release_redirect(const char *resolved_url,
                                      char *tag_out, size_t tag_cap,
                                      char *ver_out, size_t ver_cap) {
    if (tag_out && tag_cap > 0) tag_out[0] = '\0';
    if (ver_out && ver_cap > 0) ver_out[0] = '\0';
    if (!resolved_url || !resolved_url[0]) return false;

    char tag[64] = "";
    if (!app_updater_extract_release_tag(resolved_url, tag, sizeof tag)) {
        return false;
    }
    char ver[64] = "";
    if (!mcode_cli_extract_version(tag, ver, sizeof ver)) {
        return false;
    }
    if (tag_out && tag_cap > 0) snprintf(tag_out, tag_cap, "%s", tag);
    if (ver_out && ver_cap > 0) snprintf(ver_out, ver_cap, "%s", ver);
    return true;
}

typedef enum {
    CLI_UPD_METHOD_SELF_SUBCMD = 0, /* `<bin> update` */
    CLI_UPD_METHOD_BREW_CASK,       /* `brew upgrade --cask <pkg>` */
    CLI_UPD_METHOD_BREW_FORMULA,    /* `brew upgrade <pkg>` */
    CLI_UPD_METHOD_BUN_GLOBAL,      /* `bun add -g <pkg>@latest` */
    CLI_UPD_METHOD_PNPM_GLOBAL,     /* `pnpm add -g <pkg>@latest` */
    CLI_UPD_METHOD_NPM_GLOBAL,      /* `npm install -g <pkg>@latest` */
} cli_update_method;

static cli_update_method detect_update_method(app_agent_kind agent,
                                              const char *resolved_path,
                                              const char *real_path,
                                              const char **out_pkg) {
    if (out_pkg) *out_pkg = "";
    const char *p1 = resolved_path ? resolved_path : "";
    const char *p2 = (real_path && real_path[0]) ? real_path : p1;

    if (agent == APP_AGENT_CLAUDE) {
        if (strstr(p2, "/Caskroom/claude-code/") || strstr(p1, "/Caskroom/claude-code/")) {
            if (out_pkg) *out_pkg = "claude-code";
            return CLI_UPD_METHOD_BREW_CASK;
        }
        if (strstr(p2, "/Cellar/claude-code/") || strstr(p1, "/Cellar/claude-code/")) {
            if (out_pkg) *out_pkg = "claude-code";
            return CLI_UPD_METHOD_BREW_FORMULA;
        }
        return CLI_UPD_METHOD_SELF_SUBCMD;
    }

    if (agent == APP_AGENT_CODEX) {
        if (strstr(p2, "/Caskroom/codex/") || strstr(p1, "/Caskroom/codex/")) {
            if (out_pkg) *out_pkg = "codex";
            return CLI_UPD_METHOD_BREW_CASK;
        }
        if (strstr(p2, "/Cellar/codex/") || strstr(p1, "/Cellar/codex/")) {
            if (out_pkg) *out_pkg = "codex";
            return CLI_UPD_METHOD_BREW_FORMULA;
        }
        if (strstr(p2, "/.bun/") || strstr(p1, "/.bun/")) {
            if (out_pkg) *out_pkg = "@openai/codex@latest";
            return CLI_UPD_METHOD_BUN_GLOBAL;
        }
        if (strstr(p2, "/pnpm/") || strstr(p1, "/pnpm/") ||
            strstr(p2, "/.pnpm") || strstr(p1, "/.pnpm")) {
            if (out_pkg) *out_pkg = "@openai/codex@latest";
            return CLI_UPD_METHOD_PNPM_GLOBAL;
        }
        if (out_pkg) *out_pkg = "@openai/codex@latest";
        return CLI_UPD_METHOD_NPM_GLOBAL;
    }

    /* APP_AGENT_AGY */
    return CLI_UPD_METHOD_SELF_SUBCMD;
}

void mcode_cli_format_update_cmd(app_agent_kind agent,
                                 const char *resolved_path,
                                 const char *real_path,
                                 char *cmd_out, size_t cmd_cap) {
    if (!cmd_out || cmd_cap == 0) return;
    cmd_out[0] = '\0';

    const char *pkg = "";
    cli_update_method method = detect_update_method(agent, resolved_path, real_path, &pkg);
    switch (method) {
    case CLI_UPD_METHOD_SELF_SUBCMD: {
        const char *short_bin = (agent == APP_AGENT_CLAUDE) ? "claude"
                              : (agent == APP_AGENT_AGY)    ? "agy"
                                                            : "codex";
        snprintf(cmd_out, cmd_cap, "%s update", short_bin);
        break;
    }
    case CLI_UPD_METHOD_BREW_CASK:
        snprintf(cmd_out, cmd_cap, "brew upgrade --cask %s", pkg);
        break;
    case CLI_UPD_METHOD_BREW_FORMULA:
        snprintf(cmd_out, cmd_cap, "brew upgrade %s", pkg);
        break;
    case CLI_UPD_METHOD_BUN_GLOBAL:
        snprintf(cmd_out, cmd_cap, "bun add -g %s", pkg);
        break;
    case CLI_UPD_METHOD_PNPM_GLOBAL:
        snprintf(cmd_out, cmd_cap, "pnpm add -g %s", pkg);
        break;
    case CLI_UPD_METHOD_NPM_GLOBAL:
        snprintf(cmd_out, cmd_cap, "npm install -g %s", pkg);
        break;
    }
}

static void resolve_realpath_safe(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
#ifndef _WIN32
    char buf[PATH_MAX];
    if (realpath(path, buf)) {
        snprintf(out, cap, "%s", buf);
        return;
    }
#endif
    snprintf(out, cap, "%s", path);
}

static void find_sibling_or_path_tool(const char *cli_path, const char *tool,
                                      char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!tool || !tool[0]) return;

    /* Prefer a package manager living beside the CLI binary (e.g. nvm/fnm/volta/homebrew). */
    if (cli_path && cli_path[0]) {
        const char *slash = strrchr(cli_path, '/');
        if (slash && slash > cli_path) {
            char cand[APP_CLI_PATH_MAX];
            size_t dlen = (size_t)(slash - cli_path);
            if (dlen + 1 + strlen(tool) < sizeof cand) {
                snprintf(cand, sizeof cand, "%.*s/%s", (int)dlen, cli_path, tool);
                if (mcode_find_executable(cand, out, cap)) return;
            }
        }
    }
    if (mcode_find_executable(tool, out, cap)) return;
    snprintf(out, cap, "%s", tool);
}

static void sleep_short_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
#endif
}

/* ---- Background version + release check job ---- */

typedef struct mcode_cli_check_job {
    mcode_cli_updates *owner;
    app_agent_kind agent;
    bool local_only;
    bool post_update_verify;
    char bin_path[APP_CLI_PATH_MAX];
    char cached_latest_ver[64];
    char cached_latest_tag[64];
    char cached_release_url[512];

    pthread_t thread;
    bool thread_started;

    /* Guarded by owner->mu */
    mcode_proc *ver_proc;
    bool ver_proc_done;
    mcode_str ver_buf;
    bool finished;
} mcode_cli_check_job;

static bool check_should_abort(void *user) {
    mcode_cli_updates *u = (mcode_cli_updates *)user;
    if (!u) return true;
    pthread_mutex_lock(&u->mu);
    bool ab = u->abort_requested;
    pthread_mutex_unlock(&u->mu);
    return ab;
}

static void on_ver_proc_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    mcode_cli_check_job *job = (mcode_cli_check_job *)user;
    if (!job || !job->owner || !data || len == 0) return;
    pthread_mutex_lock(&job->owner->mu);
    if (job->ver_buf.len < 4096) {
        mcode_str_append(&job->ver_buf, data, len);
    }
    pthread_mutex_unlock(&job->owner->mu);
}

static void on_ver_proc_exit(int exit_code, bool killed, void *user) {
    (void)exit_code;
    (void)killed;
    mcode_cli_check_job *job = (mcode_cli_check_job *)user;
    if (!job || !job->owner) return;
    pthread_mutex_lock(&job->owner->mu);
    job->ver_proc_done = true;
    pthread_mutex_unlock(&job->owner->mu);
}

static bool run_version_probe(mcode_cli_check_job *job, char *ver_out, size_t ver_cap) {
    if (ver_out && ver_cap > 0) ver_out[0] = '\0';
    mcode_cli_updates *u = job->owner;
    if (check_should_abort(u)) return false;

    const char *argv[] = { job->bin_path, "--version", NULL };
    pthread_mutex_lock(&u->mu);
    job->ver_proc_done = false;
    mcode_str_clear(&job->ver_buf);
    pthread_mutex_unlock(&u->mu);

    mcode_proc *proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = true,
        .open_stdin = false,
        .on_bytes = on_ver_proc_bytes,
        .on_exit = on_ver_proc_exit,
        .user = job,
    });
    if (!proc) return false;

    pthread_mutex_lock(&u->mu);
    job->ver_proc = proc;
    bool aborted = u->abort_requested;
    pthread_mutex_unlock(&u->mu);
    if (aborted) {
        mcode_proc_kill(proc);
    }

    double started = mcode_now_mono();
    bool timed_out = false;
    for (;;) {
        pthread_mutex_lock(&u->mu);
        bool done = job->ver_proc_done;
        bool ab = u->abort_requested;
        pthread_mutex_unlock(&u->mu);
        if (done) break;
        if (ab) {
            mcode_proc_kill(proc);
            break;
        }
        if (!timed_out && (mcode_now_mono() - started) > 5.0) {
            timed_out = true;
            mcode_proc_kill(proc);
        }
        sleep_short_ms(15);
    }

    pthread_mutex_lock(&u->mu);
    job->ver_proc = NULL;
    pthread_mutex_unlock(&u->mu);

    mcode_proc_destroy_timeout(proc, 0);

    if (check_should_abort(u) || timed_out) return false;

    char raw[512] = "";
    pthread_mutex_lock(&u->mu);
    if (job->ver_buf.len > 0) {
        snprintf(raw, sizeof raw, "%s", mcode_str_cstr(&job->ver_buf));
    }
    pthread_mutex_unlock(&u->mu);

    return mcode_cli_extract_version(raw, ver_out, ver_cap);
}

static void *check_worker_main(void *arg) {
    mcode_cli_check_job *job = (mcode_cli_check_job *)arg;
    mcode_cli_updates *u = job->owner;
    app_agent_kind ag = job->agent;

    char real_bin[APP_CLI_PATH_MAX] = "";
    resolve_realpath_safe(job->bin_path, real_bin, sizeof real_bin);

    char cmd_disp[256] = "";
    mcode_cli_format_update_cmd(ag, job->bin_path, real_bin, cmd_disp, sizeof cmd_disp);

    char installed_ver[64] = "";
    bool got_installed = run_version_probe(job, installed_ver, sizeof installed_ver);

    char latest_ver[64] = "";
    char latest_tag[64] = "";
    char release_url[512] = "";
    char err_msg[160] = "";
    bool got_latest = false;

    if (!check_should_abort(u)) {
        if (job->local_only) {
            if (job->cached_latest_ver[0]) {
                snprintf(latest_ver, sizeof latest_ver, "%s", job->cached_latest_ver);
                snprintf(latest_tag, sizeof latest_tag, "%s", job->cached_latest_tag);
                snprintf(release_url, sizeof release_url, "%s", job->cached_release_url);
                got_latest = true;
            }
        } else {
            const char *repo = mcode_cli_github_repo(ag);
            if (repo && repo[0]) {
                char url[256];
                snprintf(url, sizeof url, "https://github.com/%s/releases/latest", repo);
                char resolved[512] = "";
                char http_err[256] = "";
                if (app_http_resolve_redirect(url, resolved, sizeof resolved,
                                              check_should_abort, u,
                                              http_err, sizeof http_err)) {
                    if (mcode_cli_parse_release_redirect(resolved,
                                                         latest_tag, sizeof latest_tag,
                                                         latest_ver, sizeof latest_ver)) {
                        snprintf(release_url, sizeof release_url, "%s", resolved);
                        got_latest = true;
                    } else {
                        snprintf(err_msg, sizeof err_msg,
                                 "Could not determine latest %s version.",
                                 mcode_agent_short_label(ag));
                    }
                } else if (!check_should_abort(u)) {
                    snprintf(err_msg, sizeof err_msg, "%s",
                             http_err[0] ? http_err : "Could not check for updates.");
                }
            }
        }
    }

    pthread_mutex_lock(&u->mu);
    if (!u->abort_requested) {
        mcode_cli_update_info *info = &u->items[ag];
        info->checking = false;
        info->installed = true;
        if (cmd_disp[0]) {
            snprintf(info->update_cmd_display, sizeof info->update_cmd_display, "%s", cmd_disp);
        }
        if (got_installed) {
            snprintf(info->installed_version, sizeof info->installed_version, "%s", installed_ver);
        }
        if (got_latest) {
            snprintf(info->latest_version, sizeof info->latest_version, "%s", latest_ver);
            snprintf(info->latest_tag, sizeof info->latest_tag, "%s", latest_tag);
            snprintf(info->release_url, sizeof info->release_url, "%s", release_url);
        }
        info->checked_epoch = mcode_now_epoch();

        if (!info->updating) {
            if (info->installed_version[0] && info->latest_version[0]) {
                int cmp = app_updater_compare_versions(info->latest_version, info->installed_version);
                if (cmp > 0) {
                    info->update_available = true;
                    info->state = MCODE_CLI_UPD_AVAILABLE;
                    info->error[0] = '\0';
                } else if (cmp == 0 || cmp == -1) {
                    info->update_available = false;
                    info->state = MCODE_CLI_UPD_UP_TO_DATE;
                    info->error[0] = '\0';
                } else {
                    info->update_available = false;
                    info->state = MCODE_CLI_UPD_ERROR;
                    snprintf(info->error, sizeof info->error, "Unrecognized version format.");
                }
            } else if (!got_installed) {
                info->update_available = false;
                info->state = MCODE_CLI_UPD_ERROR;
                snprintf(info->error, sizeof info->error, "Could not read installed CLI version.");
            } else if (err_msg[0]) {
                /* Preserve previous update_available status only if we already knew latest_version. */
                if (!info->latest_version[0]) {
                    info->update_available = false;
                    info->state = MCODE_CLI_UPD_ERROR;
                }
                snprintf(info->error, sizeof info->error, "%s", err_msg);
            }
        }

        if (job->post_update_verify) {
            mcode_cli_update_result *res = &u->finished_results[ag];
            memset(res, 0, sizeof *res);
            res->agent = ag;
            snprintf(res->installed_version, sizeof res->installed_version, "%s",
                     info->installed_version);
            snprintf(res->latest_version, sizeof res->latest_version, "%s",
                     info->latest_version);
            if (!info->update_available && info->installed_version[0]) {
                res->ok = true;
                snprintf(res->message, sizeof res->message, "Updated %s to %s.",
                         mcode_agent_label(ag), info->installed_version);
            } else if (info->installed_version[0] && info->latest_version[0]) {
                res->ok = false;
                snprintf(res->message, sizeof res->message,
                         "%s is still on %s (latest %s). Try running `%s` in Terminal.",
                         mcode_agent_label(ag), info->installed_version, info->latest_version,
                         info->update_cmd_display[0] ? info->update_cmd_display : "update");
                snprintf(info->error, sizeof info->error, "Still on %s after update.",
                         info->installed_version);
            } else {
                res->ok = true;
                snprintf(res->message, sizeof res->message, "Updated %s.", mcode_agent_label(ag));
            }
            u->finished_result_ready[ag] = true;
        }

        u->dirty = true;
    }
    job->finished = true;
    pthread_mutex_unlock(&u->mu);
    return NULL;
}

static void check_job_destroy(mcode_cli_check_job *job) {
    if (!job) return;
    if (job->thread_started) {
        (void)pthread_join(job->thread, NULL);
        job->thread_started = false;
    }
    mcode_str_free(&job->ver_buf);
    free(job);
}

/* ---- Background update install job ---- */

typedef struct mcode_cli_install_job {
    mcode_cli_updates *owner;
    app_agent_kind agent;
    mcode_proc *proc;
    char bin_path[APP_CLI_PATH_MAX];
    char last_line[160];
    bool finished;
    bool killed;
    int exit_code;
} mcode_cli_install_job;

static void extract_last_nonempty_line(const char *data, size_t len, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!data || len == 0) return;

    size_t seg_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || data[i] == '\r' || data[i] == '\n') {
            if (i > seg_start) {
                char clean[256];
                strip_ansi_copy(data + seg_start, i - seg_start, clean, sizeof clean);
                char *s = clean;
                while (*s && isspace((unsigned char)*s)) ++s;
                size_t slen = strlen(s);
                while (slen > 0 && isspace((unsigned char)s[slen - 1])) s[--slen] = '\0';
                if (slen > 0) {
                    snprintf(out, cap, "%s", s);
                }
            }
            seg_start = i + 1;
        }
    }
}

static void on_install_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    mcode_cli_install_job *job = (mcode_cli_install_job *)user;
    if (!job || !job->owner || !data || len == 0) return;

    char line[160] = "";
    extract_last_nonempty_line(data, len, line, sizeof line);
    if (!line[0]) return;

    pthread_mutex_lock(&job->owner->mu);
    snprintf(job->last_line, sizeof job->last_line, "%s", line);
    snprintf(job->owner->items[job->agent].status_line,
             sizeof job->owner->items[job->agent].status_line, "%s", line);
    job->owner->dirty = true;
    pthread_mutex_unlock(&job->owner->mu);
}

static void on_install_exit(int exit_code, bool killed, void *user) {
    mcode_cli_install_job *job = (mcode_cli_install_job *)user;
    if (!job || !job->owner) return;

    pthread_mutex_lock(&job->owner->mu);
    job->exit_code = exit_code;
    job->killed = killed;
    job->finished = true;
    job->owner->dirty = true;
    pthread_mutex_unlock(&job->owner->mu);
}

static void install_job_destroy(mcode_cli_install_job *job, int grace_ms) {
    if (!job) return;
    if (job->proc) {
        mcode_proc_destroy_timeout(job->proc, grace_ms);
        job->proc = NULL;
    }
    free(job);
}

/* ---- Public API ---- */

void mcode_cli_updates_init(mcode_cli_updates *u) {
    if (!u) return;
    memset(u, 0, sizeof *u);
    pthread_mutex_init(&u->mu, NULL);
    u->initialized = true;
}

void mcode_cli_updates_begin_shutdown(mcode_cli_updates *u) {
    if (!u || !u->initialized) return;
    mcode_proc *procs_to_kill[APP_AGENT_COUNT * 2];
    int kill_count = 0;

    pthread_mutex_lock(&u->mu);
    u->abort_requested = true;
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->check_jobs[i] && u->check_jobs[i]->ver_proc) {
            procs_to_kill[kill_count++] = u->check_jobs[i]->ver_proc;
        }
        if (u->install_jobs[i] && u->install_jobs[i]->proc) {
            procs_to_kill[kill_count++] = u->install_jobs[i]->proc;
        }
    }
    pthread_mutex_unlock(&u->mu);

    for (int i = 0; i < kill_count; ++i) {
        mcode_proc_kill(procs_to_kill[i]);
    }
}

void mcode_cli_updates_free(mcode_cli_updates *u) {
    if (!u || !u->initialized) return;
    mcode_cli_updates_begin_shutdown(u);
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->check_jobs[i]) {
            check_job_destroy(u->check_jobs[i]);
            u->check_jobs[i] = NULL;
        }
        if (u->install_jobs[i]) {
            install_job_destroy(u->install_jobs[i], 0);
            u->install_jobs[i] = NULL;
        }
    }
    pthread_mutex_destroy(&u->mu);
    u->initialized = false;
}

static void start_check_job(mcode_cli_updates *u, app_agent_kind agent,
                            const char *bin_path, bool local_only,
                            bool post_update_verify) {
    if (!u || !u->initialized || agent < 0 || agent >= APP_AGENT_COUNT) return;

    pthread_mutex_lock(&u->mu);
    if (u->abort_requested) {
        pthread_mutex_unlock(&u->mu);
        return;
    }
    if (!bin_path || !bin_path[0]) {
        mcode_cli_update_info *info = &u->items[agent];
        if (!u->install_jobs[agent] &&
            (info->installed || info->update_available || info->state != MCODE_CLI_UPD_IDLE)) {
            memset(info, 0, sizeof *info);
            u->dirty = true;
        }
        pthread_mutex_unlock(&u->mu);
        return;
    }

    /* Reap an already-finished previous check job before starting a new one. */
    mcode_cli_check_job *old_done = NULL;
    if (u->check_jobs[agent]) {
        if (u->check_jobs[agent]->finished) {
            old_done = u->check_jobs[agent];
            u->check_jobs[agent] = NULL;
        } else {
            pthread_mutex_unlock(&u->mu);
            return;
        }
    }
    if (u->install_jobs[agent] && !post_update_verify) {
        pthread_mutex_unlock(&u->mu);
        check_job_destroy(old_done);
        return;
    }

    mcode_cli_check_job *job = calloc(1, sizeof *job);
    if (!job) {
        pthread_mutex_unlock(&u->mu);
        check_job_destroy(old_done);
        return;
    }
    job->owner = u;
    job->agent = agent;
    job->local_only = local_only;
    job->post_update_verify = post_update_verify;
    snprintf(job->bin_path, sizeof job->bin_path, "%s", bin_path);
    snprintf(job->cached_latest_ver, sizeof job->cached_latest_ver, "%s",
             u->items[agent].latest_version);
    snprintf(job->cached_latest_tag, sizeof job->cached_latest_tag, "%s",
             u->items[agent].latest_tag);
    snprintf(job->cached_release_url, sizeof job->cached_release_url, "%s",
             u->items[agent].release_url);
    mcode_str_init(&job->ver_buf);

    u->items[agent].installed = true;
    u->items[agent].checking = true;
    if (u->items[agent].state == MCODE_CLI_UPD_IDLE) {
        u->items[agent].state = MCODE_CLI_UPD_CHECKING;
    }
    u->check_jobs[agent] = job;

    if (pthread_create(&job->thread, NULL, check_worker_main, job) == 0) {
        job->thread_started = true;
    } else {
        u->check_jobs[agent] = NULL;
        u->items[agent].checking = false;
        mcode_str_free(&job->ver_buf);
        free(job);
    }
    pthread_mutex_unlock(&u->mu);

    check_job_destroy(old_done);
}

void mcode_cli_updates_check_agent(mcode_cli_updates *u, const mcode_cli_status *clis,
                                   app_agent_kind agent, bool local_only) {
    if (!u || !clis || agent < 0 || agent >= APP_AGENT_COUNT) return;
    const char *bin = (clis->available[agent] && clis->resolved_path[agent][0])
        ? clis->resolved_path[agent] : NULL;
    start_check_job(u, agent, bin, local_only, false);
}

void mcode_cli_updates_check_all(mcode_cli_updates *u, const mcode_cli_status *clis,
                                 bool local_only) {
    if (!u || !clis) return;
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        mcode_cli_updates_check_agent(u, clis, (app_agent_kind)i, local_only);
    }
}

bool mcode_cli_updates_start_install(mcode_cli_updates *u, const mcode_cli_status *clis,
                                     app_agent_kind agent) {
    if (!u || !clis || agent < 0 || agent >= APP_AGENT_COUNT) return false;
    if (!clis->available[agent] || !clis->resolved_path[agent][0]) return false;

    const char *bin = clis->resolved_path[agent];
    char real_bin[APP_CLI_PATH_MAX] = "";
    resolve_realpath_safe(bin, real_bin, sizeof real_bin);

    const char *pkg = "";
    cli_update_method method = detect_update_method(agent, bin, real_bin, &pkg);

    char tool_bin[APP_CLI_PATH_MAX] = "";
    const char *argv[8];
    int argc = 0;
    switch (method) {
    case CLI_UPD_METHOD_SELF_SUBCMD:
        argv[argc++] = bin;
        argv[argc++] = "update";
        break;
    case CLI_UPD_METHOD_BREW_CASK:
        find_sibling_or_path_tool(bin, "brew", tool_bin, sizeof tool_bin);
        argv[argc++] = tool_bin;
        argv[argc++] = "upgrade";
        argv[argc++] = "--cask";
        argv[argc++] = pkg;
        break;
    case CLI_UPD_METHOD_BREW_FORMULA:
        find_sibling_or_path_tool(bin, "brew", tool_bin, sizeof tool_bin);
        argv[argc++] = tool_bin;
        argv[argc++] = "upgrade";
        argv[argc++] = pkg;
        break;
    case CLI_UPD_METHOD_BUN_GLOBAL:
        find_sibling_or_path_tool(bin, "bun", tool_bin, sizeof tool_bin);
        argv[argc++] = tool_bin;
        argv[argc++] = "add";
        argv[argc++] = "-g";
        argv[argc++] = pkg;
        break;
    case CLI_UPD_METHOD_PNPM_GLOBAL:
        find_sibling_or_path_tool(bin, "pnpm", tool_bin, sizeof tool_bin);
        argv[argc++] = tool_bin;
        argv[argc++] = "add";
        argv[argc++] = "-g";
        argv[argc++] = pkg;
        break;
    case CLI_UPD_METHOD_NPM_GLOBAL:
        find_sibling_or_path_tool(bin, "npm", tool_bin, sizeof tool_bin);
        argv[argc++] = tool_bin;
        argv[argc++] = "install";
        argv[argc++] = "-g";
        argv[argc++] = pkg;
        break;
    }
    argv[argc] = NULL;

    char cmd_disp[256] = "";
    mcode_cli_format_update_cmd(agent, bin, real_bin, cmd_disp, sizeof cmd_disp);

    pthread_mutex_lock(&u->mu);
    if (u->abort_requested || u->install_jobs[agent] != NULL) {
        pthread_mutex_unlock(&u->mu);
        return false;
    }

    mcode_cli_install_job *job = calloc(1, sizeof *job);
    if (!job) {
        pthread_mutex_unlock(&u->mu);
        return false;
    }
    job->owner = u;
    job->agent = agent;
    snprintf(job->bin_path, sizeof job->bin_path, "%s", bin);

    u->install_jobs[agent] = job;
    u->items[agent].updating = true;
    u->items[agent].state = MCODE_CLI_UPD_UPDATING;
    u->items[agent].error[0] = '\0';
    snprintf(u->items[agent].update_cmd_display, sizeof u->items[agent].update_cmd_display,
             "%s", cmd_disp);
    snprintf(u->items[agent].status_line, sizeof u->items[agent].status_line,
             "Running `%s`...", cmd_disp);
    u->dirty = true;
    pthread_mutex_unlock(&u->mu);

    job->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = true,
        .open_stdin = false,
        .on_bytes = on_install_bytes,
        .on_exit = on_install_exit,
        .user = job,
    });
    if (!job->proc) {
        pthread_mutex_lock(&u->mu);
        u->install_jobs[agent] = NULL;
        u->items[agent].updating = false;
        u->items[agent].state = u->items[agent].update_available
            ? MCODE_CLI_UPD_AVAILABLE : MCODE_CLI_UPD_ERROR;
        u->items[agent].status_line[0] = '\0';
        snprintf(u->items[agent].error, sizeof u->items[agent].error,
                 "Could not start `%s`.", cmd_disp);
        u->dirty = true;
        pthread_mutex_unlock(&u->mu);
        free(job);
        return false;
    }

    app_log_info("Started CLI update for %s: %s", mcode_agent_label(agent), cmd_disp);
    return true;
}

bool mcode_cli_updates_poll(mcode_cli_updates *u, const mcode_cli_status *clis) {
    if (!u) return false;

    mcode_cli_check_job *done_checks[APP_AGENT_COUNT] = { 0 };
    mcode_cli_install_job *done_installs[APP_AGENT_COUNT] = { 0 };
    bool verify_agents[APP_AGENT_COUNT] = { 0 };
    char verify_bins[APP_AGENT_COUNT][APP_CLI_PATH_MAX];
    memset(verify_bins, 0, sizeof verify_bins);

    pthread_mutex_lock(&u->mu);
    bool changed = u->dirty;
    u->dirty = false;

    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->check_jobs[i] && u->check_jobs[i]->finished) {
            done_checks[i] = u->check_jobs[i];
            u->check_jobs[i] = NULL;
            changed = true;
        }
        if (u->install_jobs[i] && u->install_jobs[i]->finished) {
            mcode_cli_install_job *ij = u->install_jobs[i];
            u->install_jobs[i] = NULL;
            done_installs[i] = ij;
            changed = true;

            mcode_cli_update_info *info = &u->items[i];
            info->updating = false;
            info->status_line[0] = '\0';

            if (!ij->killed && ij->exit_code == 0) {
                info->error[0] = '\0';
                u->pending_post_verify[i] = true;
                const char *bin = (clis && clis->available[i] && clis->resolved_path[i][0])
                    ? clis->resolved_path[i] : ij->bin_path;
                snprintf(u->pending_post_bin[i], sizeof u->pending_post_bin[i], "%s", bin);
            } else if (!ij->killed) {
                info->state = info->update_available ? MCODE_CLI_UPD_AVAILABLE : MCODE_CLI_UPD_ERROR;
                if (ij->last_line[0]) {
                    snprintf(info->error, sizeof info->error, "%s", ij->last_line);
                } else {
                    snprintf(info->error, sizeof info->error,
                             "Update exited with code %d.", ij->exit_code);
                }
                mcode_cli_update_result *res = &u->finished_results[i];
                memset(res, 0, sizeof *res);
                res->agent = (app_agent_kind)i;
                res->ok = false;
                snprintf(res->message, sizeof res->message, "%s update failed: %s",
                         mcode_agent_label((app_agent_kind)i), info->error);
                u->finished_result_ready[i] = true;
            }
        }
        if (u->pending_post_verify[i] && !u->check_jobs[i]) {
            u->pending_post_verify[i] = false;
            verify_agents[i] = true;
            snprintf(verify_bins[i], sizeof verify_bins[i], "%s", u->pending_post_bin[i]);
        }
    }
    pthread_mutex_unlock(&u->mu);

    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (done_checks[i]) check_job_destroy(done_checks[i]);
        if (done_installs[i]) install_job_destroy(done_installs[i], 0);
        if (verify_agents[i]) {
            start_check_job(u, (app_agent_kind)i, verify_bins[i], true, true);
        }
    }
    return changed;
}

bool mcode_cli_updates_take_result(mcode_cli_updates *u, mcode_cli_update_result *out) {
    if (!u) return false;
    bool found = false;
    pthread_mutex_lock(&u->mu);
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->finished_result_ready[i]) {
            u->finished_result_ready[i] = false;
            if (out) *out = u->finished_results[i];
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&u->mu);
    return found;
}

void mcode_cli_updates_snapshot_get(mcode_cli_updates *u, mcode_cli_updates_snapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!u) return;

    pthread_mutex_lock(&u->mu);
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        out->items[i] = u->items[i];
        if (u->items[i].installed && u->items[i].update_available) {
            out->available_count++;
        }
        if (u->items[i].checking) out->checking_count++;
        if (u->items[i].updating) out->updating_count++;
    }
    pthread_mutex_unlock(&u->mu);
}

bool mcode_cli_updates_is_busy(mcode_cli_updates *u) {
    if (!u) return false;
    pthread_mutex_lock(&u->mu);
    bool busy = false;
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->check_jobs[i] || u->install_jobs[i] || u->pending_post_verify[i]) {
            busy = true;
            break;
        }
    }
    pthread_mutex_unlock(&u->mu);
    return busy;
}

int mcode_cli_updates_available_count(mcode_cli_updates *u) {
    if (!u) return 0;
    pthread_mutex_lock(&u->mu);
    int n = 0;
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (u->items[i].installed && u->items[i].update_available) ++n;
    }
    pthread_mutex_unlock(&u->mu);
    return n;
}
