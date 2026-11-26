#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_proc.h"
#include "mcode_types.h"
#include "app_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Minimal Win32 stubs so non-POSIX builds still compile cleanly. */
struct mcode_proc {
    int dummy;
};
mcode_proc *mcode_proc_spawn(const mcode_proc_desc *desc) { (void)desc; return NULL; }
bool mcode_proc_write(mcode_proc *proc, const char *data, size_t len) {
    (void)proc; (void)data; (void)len; return false;
}
void mcode_proc_close_stdin(mcode_proc *proc) { (void)proc; }
mcode_proc *mcode_proc_spawn_shell(const char *command, const char *cwd,
                                   mcode_proc_bytes_fn on_bytes,
                                   mcode_proc_exit_fn on_exit, void *user) {
    (void)command; (void)cwd; (void)on_bytes; (void)on_exit; (void)user;
    return NULL;
}
void mcode_proc_stop(mcode_proc *proc) { (void)proc; }
void mcode_proc_kill(mcode_proc *proc) { (void)proc; }
bool mcode_proc_is_running(const mcode_proc *proc) { (void)proc; return false; }
void mcode_proc_destroy(mcode_proc *proc) { free(proc); }
void mcode_proc_destroy_timeout(mcode_proc *proc, int grace_ms) { (void)grace_ms; free(proc); }
bool mcode_proc_run_capture(const char *const *argv, const char *cwd,
                            bool merge_stderr, int *out_exit,
                            char **out_buf, size_t *out_len) {
    (void)argv; (void)cwd; (void)merge_stderr; (void)out_exit; (void)out_buf; (void)out_len;
    return false;
}
bool mcode_find_executable(const char *name_or_path, char *out, size_t cap) {
    (void)name_or_path; if (out && cap) out[0] = '\0'; return false;
}
#else

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct mcode_proc {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int wake_pipe[2];
    mcode_proc_bytes_fn on_bytes;
    mcode_proc_exit_fn on_exit;
    void *user;

    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mu;
    pthread_cond_t done_cond;
    pthread_mutex_t write_mu;
    bool running;
    bool stop_requested;
    bool abort_io;
};

static bool is_executable_file(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return access(path, X_OK) == 0;
}

bool mcode_find_executable(const char *name_or_path, char *out, size_t cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!name_or_path || !name_or_path[0]) return false;

    /* Explicit path (contains '/') or ~ expansion */
    if (name_or_path[0] == '~' && name_or_path[1] == '/') {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char expanded[1024];
            int n = snprintf(expanded, sizeof expanded, "%s%s", home, name_or_path + 1);
            if (n > 0 && (size_t)n < sizeof expanded && is_executable_file(expanded)) {
                if (out && cap > 0) snprintf(out, cap, "%s", expanded);
                return true;
            }
        }
        return false;
    }
    if (strchr(name_or_path, '/')) {
        if (is_executable_file(name_or_path)) {
            if (out && cap > 0) snprintf(out, cap, "%s", name_or_path);
            return true;
        }
        return false;
    }

    /* Search PATH */
    const char *path_env = getenv("PATH");
    if (path_env && path_env[0]) {
        const char *p = path_env;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t dir_len = colon ? (size_t)(colon - p) : strlen(p);
            if (dir_len > 0 && dir_len < 900) {
                char candidate[1024];
                snprintf(candidate, sizeof candidate, "%.*s/%s", (int)dir_len, p, name_or_path);
                if (is_executable_file(candidate)) {
                    if (out && cap > 0) snprintf(out, cap, "%s", candidate);
                    return true;
                }
            }
            if (!colon) break;
            p = colon + 1;
        }
    }

    /* Check common macOS/Unix user & Homebrew install locations (important when launched from .app bundle) */
    const char *home = getenv("HOME");
    static const char *const extra_dirs[] = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
        "/usr/bin",
        "/bin",
    };
    if (home && home[0]) {
        static const char *const home_suffixes[] = {
            "/.local/bin",
            "/.cargo/bin",
            "/.npm-global/bin",
            "/bin",
            "/.gemini/bin",
            "/.antigravity/bin",
        };
        for (size_t i = 0; i < sizeof home_suffixes / sizeof home_suffixes[0]; ++i) {
            char candidate[1024];
            snprintf(candidate, sizeof candidate, "%s%s/%s", home, home_suffixes[i], name_or_path);
            if (is_executable_file(candidate)) {
                if (out && cap > 0) snprintf(out, cap, "%s", candidate);
                return true;
            }
        }
    }
    for (size_t i = 0; i < sizeof extra_dirs / sizeof extra_dirs[0]; ++i) {
        char candidate[1024];
        snprintf(candidate, sizeof candidate, "%s/%s", extra_dirs[i], name_or_path);
        if (is_executable_file(candidate)) {
            if (out && cap > 0) snprintf(out, cap, "%s", candidate);
            return true;
        }
    }
    return false;
}

static void augment_child_path(void) {
    const char *cur = getenv("PATH");
    const char *home = getenv("HOME");
    char next_path[4096];
    if (home && home[0]) {
        snprintf(next_path, sizeof next_path,
                 "%s/.local/bin:%s/.gemini/bin:/opt/homebrew/bin:/usr/local/bin:%s",
                 home, home, (cur && cur[0]) ? cur : "/usr/bin:/bin");
    } else {
        snprintf(next_path, sizeof next_path,
                 "/opt/homebrew/bin:/usr/local/bin:%s",
                 (cur && cur[0]) ? cur : "/usr/bin:/bin");
    }
    setenv("PATH", next_path, 1);
}

static void set_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void clear_cloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC)) (void)fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void set_nonblock(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void proc_wake(mcode_proc *proc) {
    if (!proc || proc->wake_pipe[1] < 0) return;
    char b = 1;
    ssize_t ignored = write(proc->wake_pipe[1], &b, 1);
    (void)ignored;
}

static void proc_drain_fd(mcode_proc *proc, int *fd_p, bool is_stderr) {
    char buf[4096];
    while (*fd_p >= 0) {
        pthread_mutex_lock(&proc->mu);
        bool aborted = proc->abort_io;
        pthread_mutex_unlock(&proc->mu);
        if (aborted) return;

        ssize_t n = read(*fd_p, buf, sizeof buf);
        if (n > 0) {
            if (proc->on_bytes) proc->on_bytes(buf, (size_t)n, is_stderr, proc->user);
        } else if (n == 0) {
            close(*fd_p);
            *fd_p = -1;
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close(*fd_p);
            *fd_p = -1;
            return;
        }
    }
}

static void *proc_reader_main(void *arg) {
    mcode_proc *proc = (mcode_proc *)arg;
    int fd_out = proc->stdout_fd;
    int fd_err = proc->stderr_fd;
    int fd_wake = proc->wake_pipe[0];
    int status = 0;
    bool reaped = false;

    while (fd_out >= 0 || fd_err >= 0) {
        pthread_mutex_lock(&proc->mu);
        bool aborted = proc->abort_io;
        pthread_mutex_unlock(&proc->mu);
        if (aborted) break;

        struct pollfd pfds[3];
        nfds_t nfds = 0;
        int idx_wake = -1, idx_out = -1, idx_err = -1;
        if (fd_wake >= 0) {
            idx_wake = (int)nfds;
            pfds[nfds++] = (struct pollfd){ .fd = fd_wake, .events = POLLIN, .revents = 0 };
        }
        if (fd_out >= 0) {
            idx_out = (int)nfds;
            pfds[nfds++] = (struct pollfd){ .fd = fd_out, .events = POLLIN, .revents = 0 };
        }
        if (fd_err >= 0) {
            idx_err = (int)nfds;
            pfds[nfds++] = (struct pollfd){ .fd = fd_err, .events = POLLIN, .revents = 0 };
        }

        int ready = poll(pfds, nfds, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (idx_wake >= 0 && (pfds[idx_wake].revents & (POLLIN | POLLHUP | POLLERR))) {
            char discard[64];
            while (read(fd_wake, discard, sizeof discard) > 0) {}
            pthread_mutex_lock(&proc->mu);
            aborted = proc->abort_io;
            pthread_mutex_unlock(&proc->mu);
            if (aborted) break;
        }
        if (ready == 0) {
            /* If the direct child already exited while a detached grandchild keeps
             * stdout/stderr open, drain any buffered output and finish cleanly. */
            if (!reaped && proc->pid > 0) {
                pid_t w = waitpid(proc->pid, &status, WNOHANG);
                if (w == proc->pid) {
                    reaped = true;
                    if (fd_out >= 0) proc_drain_fd(proc, &fd_out, false);
                    if (fd_err >= 0) proc_drain_fd(proc, &fd_err, true);
                    break;
                } else if (w < 0 && errno == ECHILD) {
                    reaped = true;
                    break;
                }
            }
            continue;
        }

        if (idx_out >= 0 && (pfds[idx_out].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            if (pfds[idx_out].revents & POLLNVAL) {
                close(fd_out);
                fd_out = -1;
            } else {
                proc_drain_fd(proc, &fd_out, false);
                if (fd_out >= 0 && (pfds[idx_out].revents & POLLERR)) {
                    close(fd_out);
                    fd_out = -1;
                }
            }
        }
        if (idx_err >= 0 && (pfds[idx_err].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            if (pfds[idx_err].revents & POLLNVAL) {
                close(fd_err);
                fd_err = -1;
            } else {
                proc_drain_fd(proc, &fd_err, true);
                if (fd_err >= 0 && (pfds[idx_err].revents & POLLERR)) {
                    close(fd_err);
                    fd_err = -1;
                }
            }
        }
    }

    if (fd_out >= 0) close(fd_out);
    if (fd_err >= 0) close(fd_err);
    proc->stdout_fd = -1;
    proc->stderr_fd = -1;

    while (!reaped && proc->pid > 0) {
        pid_t w = waitpid(proc->pid, &status, 0);
        if (w == proc->pid) {
            reaped = true;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    int exit_code = -1;
    bool killed = false;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
        killed = true;
    }

    pthread_mutex_lock(&proc->mu);
    bool skip_cb = proc->abort_io;
    if (proc->stop_requested) killed = true;
    proc->pid = -1;
    proc->running = false;
    pthread_cond_broadcast(&proc->done_cond);
    pthread_mutex_unlock(&proc->mu);

    if (!skip_cb && proc->on_exit) proc->on_exit(exit_code, killed, proc->user);
    return NULL;
}

mcode_proc *mcode_proc_spawn(const mcode_proc_desc *desc) {
    if (!desc || !desc->argv || !desc->argv[0] || !desc->argv[0][0]) return NULL;

    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    int wake_pipe[2] = { -1, -1 };
    if (desc->open_stdin) {
        if (pipe(in_pipe) != 0) return NULL;
    }
    if (pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        return NULL;
    }
    if (!desc->merge_stderr) {
        if (pipe(err_pipe) != 0) {
            if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
            close(out_pipe[0]);
            close(out_pipe[1]);
            return NULL;
        }
    }
    if (pipe(wake_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (err_pipe[0] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
        return NULL;
    }

    if (in_pipe[0] >= 0) set_cloexec(in_pipe[0]);
    if (in_pipe[1] >= 0) {
        set_cloexec(in_pipe[1]);
#ifdef F_SETNOSIGPIPE
        (void)fcntl(in_pipe[1], F_SETNOSIGPIPE, 1);
#endif
    }
    set_cloexec(out_pipe[0]);
    set_cloexec(out_pipe[1]);
    set_nonblock(out_pipe[0]);
    if (err_pipe[0] >= 0) {
        set_cloexec(err_pipe[0]);
        set_nonblock(err_pipe[0]);
    }
    if (err_pipe[1] >= 0) set_cloexec(err_pipe[1]);
    set_cloexec(wake_pipe[0]);
    set_cloexec(wake_pipe[1]);
    set_nonblock(wake_pipe[0]);
    set_nonblock(wake_pipe[1]);

    /* Copy argv into mutable array before fork for execvp */
    size_t argc = 0;
    while (desc->argv[argc]) ++argc;
    char **child_argv = calloc(argc + 1, sizeof(char *));
    if (!child_argv) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        close(out_pipe[0]); close(out_pipe[1]);
        if (err_pipe[0] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
        close(wake_pipe[0]); close(wake_pipe[1]);
        return NULL;
    }
    for (size_t i = 0; i < argc; ++i) {
        child_argv[i] = strdup(desc->argv[i]);
    }
    char *child_cwd = (desc->cwd && desc->cwd[0]) ? strdup(desc->cwd) : NULL;

    pid_t pid = fork();
    if (pid < 0) {
        for (size_t i = 0; i < argc; ++i) free(child_argv[i]);
        free(child_argv);
        free(child_cwd);
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        close(out_pipe[0]); close(out_pipe[1]);
        if (err_pipe[0] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
        close(wake_pipe[0]); close(wake_pipe[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child process */
        setpgid(0, 0);
        if (in_pipe[0] >= 0) {
            if (in_pipe[0] != STDIN_FILENO) dup2(in_pipe[0], STDIN_FILENO);
            clear_cloexec(STDIN_FILENO);
            if (in_pipe[0] > STDERR_FILENO) close(in_pipe[0]);
            if (in_pipe[1] > STDERR_FILENO) close(in_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        if (out_pipe[1] != STDOUT_FILENO) dup2(out_pipe[1], STDOUT_FILENO);
        clear_cloexec(STDOUT_FILENO);
        if (desc->merge_stderr) {
            dup2(out_pipe[1], STDERR_FILENO);
        } else {
            if (err_pipe[1] != STDERR_FILENO) dup2(err_pipe[1], STDERR_FILENO);
        }
        clear_cloexec(STDERR_FILENO);
        if (out_pipe[0] > STDERR_FILENO) close(out_pipe[0]);
        if (out_pipe[1] > STDERR_FILENO) close(out_pipe[1]);
        if (err_pipe[0] > STDERR_FILENO) close(err_pipe[0]);
        if (err_pipe[1] > STDERR_FILENO) close(err_pipe[1]);
        if (wake_pipe[0] > STDERR_FILENO) close(wake_pipe[0]);
        if (wake_pipe[1] > STDERR_FILENO) close(wake_pipe[1]);

        if (child_cwd && chdir(child_cwd) != 0) {
            _exit(126);
        }
        augment_child_path();
        setenv("TERM", "xterm-256color", 0);
        execvp(child_argv[0], child_argv);
        _exit(127);
    }

    /* Parent */
    for (size_t i = 0; i < argc; ++i) free(child_argv[i]);
    free(child_argv);
    free(child_cwd);

    if (in_pipe[0] >= 0) close(in_pipe[0]);
    close(out_pipe[1]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);

    mcode_proc *proc = calloc(1, sizeof *proc);
    if (!proc) {
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        close(out_pipe[0]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    proc->pid = pid;
    proc->stdin_fd = in_pipe[1];
    proc->stdout_fd = out_pipe[0];
    proc->stderr_fd = err_pipe[0];
    proc->wake_pipe[0] = wake_pipe[0];
    proc->wake_pipe[1] = wake_pipe[1];
    proc->on_bytes = desc->on_bytes;
    proc->on_exit = desc->on_exit;
    proc->user = desc->user;
    proc->running = true;
    pthread_mutex_init(&proc->mu, NULL);
    pthread_cond_init(&proc->done_cond, NULL);
    pthread_mutex_init(&proc->write_mu, NULL);

    if (pthread_create(&proc->thread, NULL, proc_reader_main, proc) != 0) {
        if (proc->stdin_fd >= 0) close(proc->stdin_fd);
        close(proc->stdout_fd);
        if (proc->stderr_fd >= 0) close(proc->stderr_fd);
        close(proc->wake_pipe[0]);
        close(proc->wake_pipe[1]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&proc->write_mu);
        pthread_cond_destroy(&proc->done_cond);
        pthread_mutex_destroy(&proc->mu);
        free(proc);
        return NULL;
    }
    proc->thread_started = true;
    return proc;
}

bool mcode_proc_write(mcode_proc *proc, const char *data, size_t len) {
    if (!proc || !data || len == 0) return false;
    pthread_mutex_lock(&proc->write_mu);
    int fd = proc->stdin_fd;
    if (fd < 0) {
        pthread_mutex_unlock(&proc->write_mu);
        return false;
    }
    const char *p = data;
    size_t rem = len;
    bool ok = true;
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n > 0) {
            p += (size_t)n;
            rem -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
            break;
        }
    }
    pthread_mutex_unlock(&proc->write_mu);
    return ok;
}

void mcode_proc_close_stdin(mcode_proc *proc) {
    if (!proc) return;
    pthread_mutex_lock(&proc->write_mu);
    if (proc->stdin_fd >= 0) {
        close(proc->stdin_fd);
        proc->stdin_fd = -1;
    }
    pthread_mutex_unlock(&proc->write_mu);
}

mcode_proc *mcode_proc_spawn_shell(const char *command, const char *cwd,
                                   mcode_proc_bytes_fn on_bytes,
                                   mcode_proc_exit_fn on_exit, void *user) {
    if (!command || !command[0]) return NULL;
    const char *shell = getenv("SHELL");
    if (!shell || !shell[0]) shell = "/bin/zsh";
    if (!is_executable_file(shell)) shell = "/bin/sh";
    const char *argv[] = { shell, "-c", command, NULL };
    return mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = cwd,
        .merge_stderr = true,
        .on_bytes = on_bytes,
        .on_exit = on_exit,
        .user = user,
    });
}

void mcode_proc_stop(mcode_proc *proc) {
    if (!proc) return;
    mcode_proc_close_stdin(proc);
    pthread_mutex_lock(&proc->mu);
    bool running = proc->running;
    proc->stop_requested = true;
    pid_t pid = proc->pid;
    pthread_mutex_unlock(&proc->mu);
    if (running && pid > 0) {
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
    }
}

void mcode_proc_kill(mcode_proc *proc) {
    if (!proc) return;
    mcode_proc_close_stdin(proc);
    pthread_mutex_lock(&proc->mu);
    bool running = proc->running;
    proc->stop_requested = true;
    proc->abort_io = true;
    pid_t pid = proc->pid;
    pthread_mutex_unlock(&proc->mu);
    if (running && pid > 0) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    proc_wake(proc);
}

bool mcode_proc_is_running(const mcode_proc *proc) {
    if (!proc) return false;
    mcode_proc *mp = (mcode_proc *)proc;
    pthread_mutex_lock(&mp->mu);
    bool r = mp->running;
    pthread_mutex_unlock(&mp->mu);
    return r;
}

static void proc_wait_exit(mcode_proc *proc, int timeout_ms) {
    if (!proc || timeout_ms <= 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&proc->mu);
    while (proc->running) {
        int rc = pthread_cond_timedwait(&proc->done_cond, &proc->mu, &ts);
        if (rc == ETIMEDOUT) break;
        if (rc != 0 && rc != EINTR) break;
    }
    pthread_mutex_unlock(&proc->mu);
}

static void proc_free_resources(mcode_proc *proc) {
    if (!proc) return;
    mcode_proc_close_stdin(proc);
    if (proc->wake_pipe[0] >= 0) close(proc->wake_pipe[0]);
    if (proc->wake_pipe[1] >= 0) close(proc->wake_pipe[1]);
    pthread_mutex_destroy(&proc->write_mu);
    pthread_cond_destroy(&proc->done_cond);
    pthread_mutex_destroy(&proc->mu);
    free(proc);
}

void mcode_proc_destroy_timeout(mcode_proc *proc, int grace_ms) {
    if (!proc) return;
    mcode_proc_close_stdin(proc);
    pthread_mutex_lock(&proc->mu);
    bool running = proc->running;
    bool already_killing = proc->abort_io;
    pthread_mutex_unlock(&proc->mu);
    if (running && !already_killing) {
        if (grace_ms > 0) {
            mcode_proc_stop(proc);
            proc_wait_exit(proc, grace_ms);
        }
        if (mcode_proc_is_running(proc)) {
            mcode_proc_kill(proc);
        }
    }
    if (proc->thread_started) {
        pthread_join(proc->thread, NULL);
        proc->thread_started = false;
    }
    proc_free_resources(proc);
}

void mcode_proc_destroy(mcode_proc *proc) {
    mcode_proc_destroy_timeout(proc, 150);
}

typedef struct {
    mcode_str buf;
    pthread_mutex_t mu;
    int exit_code;
} capture_ctx;

static void capture_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    capture_ctx *ctx = (capture_ctx *)user;
    pthread_mutex_lock(&ctx->mu);
    mcode_str_append(&ctx->buf, data, len);
    pthread_mutex_unlock(&ctx->mu);
}

static void capture_exit(int exit_code, bool killed, void *user) {
    (void)killed;
    capture_ctx *ctx = (capture_ctx *)user;
    pthread_mutex_lock(&ctx->mu);
    ctx->exit_code = exit_code;
    pthread_mutex_unlock(&ctx->mu);
}

bool mcode_proc_run_capture(const char *const *argv, const char *cwd,
                            bool merge_stderr, int *out_exit,
                            char **out_buf, size_t *out_len) {
    if (out_exit) *out_exit = -1;
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;

    capture_ctx ctx;
    mcode_str_init(&ctx.buf);
    pthread_mutex_init(&ctx.mu, NULL);
    ctx.exit_code = -1;

    mcode_proc *proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = cwd,
        .merge_stderr = merge_stderr,
        .on_bytes = capture_bytes,
        .on_exit = capture_exit,
        .user = &ctx,
    });
    if (!proc) {
        pthread_mutex_destroy(&ctx.mu);
        mcode_str_free(&ctx.buf);
        return false;
    }

    if (proc->thread_started) {
        pthread_join(proc->thread, NULL);
        proc->thread_started = false;
    }
    proc_free_resources(proc);
    pthread_mutex_destroy(&ctx.mu);

    if (out_exit) *out_exit = ctx.exit_code;
    if (out_buf) {
        if (!ctx.buf.data) {
            *out_buf = strdup("");
        } else {
            *out_buf = ctx.buf.data;
            ctx.buf.data = NULL;
        }
    } else {
        mcode_str_free(&ctx.buf);
    }
    if (out_len) *out_len = ctx.buf.len;
    return true;
}

#endif
