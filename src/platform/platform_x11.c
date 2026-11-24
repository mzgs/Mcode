#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "app_config.h"
#include "platform.h"
#include <cui/cui_app.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/Xatom.h>) && __has_include(<X11/Xutil.h>)
#define APP_HAVE_X11 1
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#endif

int app_system_dark(void) {
    const char *gtk = getenv("GTK_THEME");
    if (!gtk || !gtk[0]) return -1;
    for (const char *p = gtk; *p; ++p) {
        if (tolower((unsigned char)p[0]) == 'd' &&
            tolower((unsigned char)p[1]) == 'a' &&
            tolower((unsigned char)p[2]) == 'r' &&
            tolower((unsigned char)p[3]) == 'k')
            return 1;
    }
    return 0;
}

bool app_os_info(char *out, size_t capacity) {
    if (!out || capacity == 0) return false;
    struct utsname u;
    if (uname(&u) == 0) {
        int n = snprintf(out, capacity, "%s %s (%s)", u.sysname, u.release, u.machine);
        return n > 0 && (size_t)n < capacity;
    }
    int n = snprintf(out, capacity, "Linux");
    return n > 0 && (size_t)n < capacity;
}

#ifdef APP_HAVE_X11
static Display *g_x11_dpy = NULL;
static bool g_x11_tried = false;

static Display *x11_display(void) {
    if (!g_x11_tried) {
        g_x11_tried = true;
        g_x11_dpy = XOpenDisplay(NULL);
        if (g_x11_dpy) {
            int fd = ConnectionNumber(g_x11_dpy);
            if (fd >= 0) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    return g_x11_dpy;
}

static void x11_frame_extents(Display *dpy, Window win, int *left, int *top) {
    *left = 0;
    *top = 0;
    Atom prop = XInternAtom(dpy, "_NET_FRAME_EXTENTS", True);
    if (!prop) return;
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, prop, 0, 4, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success && data) {
        if (actual_format == 32 && nitems >= 4) {
            long *extents = (long *)data;
            *left = (int)extents[0];
            *top = (int)extents[2];
        }
        XFree(data);
    }
}

static bool x11_window_has_abnormal_state(Display *dpy, Window win) {
    Atom prop = XInternAtom(dpy, "_NET_WM_STATE", True);
    if (!prop) return false;
    Atom hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", True);
    Atom max_h = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
    Atom max_v = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", True);
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    bool abnormal = false;
    if (XGetWindowProperty(dpy, win, prop, 0, 64, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success && data) {
        if (actual_type == XA_ATOM && actual_format == 32) {
            Atom *atoms = (Atom *)data;
            for (unsigned long i = 0; i < nitems; ++i) {
                if ((hidden && atoms[i] == hidden) ||
                    (max_h && atoms[i] == max_h) ||
                    (max_v && atoms[i] == max_v)) {
                    abnormal = true;
                    break;
                }
            }
        }
        XFree(data);
    }
    return abnormal;
}
#endif

bool app_window_is_normal(void *native_window, const char *backend) {
    if (!native_window || !backend) return true;
#ifdef APP_HAVE_X11
    if (strcmp(backend, "x11") == 0) {
        Display *dpy = x11_display();
        if (!dpy) return true;
        Window win = (Window)(uintptr_t)native_window;
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, win, &attr) || attr.map_state != IsViewable) return false;
        return !x11_window_has_abnormal_state(dpy, win);
    }
#endif
    return true;
}

bool app_window_get_pos(void *native_window, const char *backend, int *x, int *y) {
    if (!native_window || !backend || !x || !y) return false;
#ifdef APP_HAVE_X11
    if (strcmp(backend, "x11") == 0) {
        Display *dpy = x11_display();
        if (!dpy) return false;
        Window win = (Window)(uintptr_t)native_window;
        XWindowAttributes attr;
        Window child = 0;
        int rx = 0, ry = 0;
        if (!XGetWindowAttributes(dpy, win, &attr) || !attr.root || attr.map_state != IsViewable)
            return false;
        if (x11_window_has_abnormal_state(dpy, win)) return false;
        if (!XTranslateCoordinates(dpy, win, attr.root, 0, 0, &rx, &ry, &child)) return false;
        int left = 0, top = 0;
        x11_frame_extents(dpy, win, &left, &top);
        *x = rx - left;
        *y = ry - top;
        return true;
    }
#endif
    return false;
}

bool app_window_get_bounds(void *native_window, const char *backend,
                           bool *fullscreen, bool *normal,
                           int *width, int *height,
                           bool *has_pos, int *x, int *y) {
    (void)native_window;
    (void)backend;
    (void)fullscreen;
    (void)normal;
    (void)width;
    (void)height;
    (void)has_pos;
    (void)x;
    (void)y;
    return false;
}

bool app_window_set_pos(void *native_window, const char *backend, int x, int y) {
    if (!native_window || !backend) return false;
    if (x < APP_WINDOW_POS_MIN || x > APP_WINDOW_POS_MAX ||
        y < APP_WINDOW_POS_MIN || y > APP_WINDOW_POS_MAX)
        return false;
#ifdef APP_HAVE_X11
    if (strcmp(backend, "x11") == 0) {
        Display *dpy = x11_display();
        if (!dpy) return false;
        Window win = (Window)(uintptr_t)native_window;
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, win, &attr)) return false;
        int scr = DefaultScreen(dpy);
        int sw = DisplayWidth(dpy, scr);
        int sh = DisplayHeight(dpy, scr);
        if (x + attr.width < 64 || x > sw - 64 || y < 0 || y > sh - 48) return false;
        XMoveWindow(dpy, win, x, y);
        XFlush(dpy);
        return true;
    }
#endif
    return false;
}

void app_window_center(void *native_window, const char *backend, int width, int height) {
    if (!native_window || !backend) return;
#ifdef APP_HAVE_X11
    if (strcmp(backend, "x11") == 0) {
        Display *dpy = x11_display();
        if (!dpy) return;
        Window win = (Window)(uintptr_t)native_window;
        int win_w = 0, win_h = 0;
        if (width > 0 && height > 0) {
            cui_ctx *ui = cui_app_ui(NULL);
            float scale = ui ? cui_dpi_scale(ui) : 1.0f;
            if (scale <= 0.0f) scale = 1.0f;
            win_w = (int)((float)width * scale + 0.5f);
            win_h = (int)((float)height * scale + 0.5f);
        } else {
            XWindowAttributes attr;
            if (!XGetWindowAttributes(dpy, win, &attr)) return;
            win_w = attr.width;
            win_h = attr.height;
        }
        int scr = DefaultScreen(dpy);
        int cx = (DisplayWidth(dpy, scr) - win_w) / 2;
        int cy = (DisplayHeight(dpy, scr) - win_h) / 2;
        XMoveWindow(dpy, win, cx > 0 ? cx : 0, cy > 0 ? cy : 0);
        XFlush(dpy);
    }
#else
    (void)width;
    (void)height;
#endif
}

void app_window_apply_metadata(void *native_window, const char *backend) {
    if (!native_window || !backend) return;
#ifdef APP_HAVE_X11
    if (strcmp(backend, "x11") == 0 && APP_BUNDLE_ID[0] != '\0') {
        Display *dpy = x11_display();
        if (!dpy) return;
        Window win = (Window)(uintptr_t)native_window;
        XClassHint *hint = XAllocClassHint();
        if (!hint) return;
        char wm_class[256];
        snprintf(wm_class, sizeof wm_class, "%s", APP_BUNDLE_ID);
        hint->res_name = wm_class;
        hint->res_class = wm_class;
        XSetClassHint(dpy, win, hint);
        XFree(hint);
        XFlush(dpy);
    }
#endif
}

void app_window_set_hooks(void *native_window, const char *backend, const app_window_hooks *hooks) {
    (void)native_window;
    (void)backend;
    (void)hooks;
}

void app_window_set_dark(void *native_window, const char *backend, bool dark) {
    (void)native_window;
    (void)backend;
    (void)dark;
}

bool app_window_enable_integrated_titlebar(void *native_window, const char *backend, float height) {
    (void)native_window;
    (void)backend;
    (void)height;
    return false;
}

float app_window_titlebar_left_inset(void *native_window, const char *backend) {
    (void)native_window;
    (void)backend;
    return 0.0f;
}

void app_window_start_drag(void *native_window, const char *backend) {
    (void)native_window;
    (void)backend;
}

void app_window_titlebar_double_click(void *native_window, const char *backend) {
    (void)native_window;
    (void)backend;
}

void app_platform_shutdown(void) {
#ifdef APP_HAVE_X11
    if (g_x11_dpy) {
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
    g_x11_tried = false;
#endif
}

static void linux_set_err(char *err, size_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg ? msg : "Unknown error.");
}

static const char *linux_release_arch(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x64";
#else
    return NULL;
#endif
}

static bool linux_run_process(char *const argv[], char *out, size_t out_cap,
                              app_updater_abort_fn should_abort, void *abort_user,
                              const char *cancel_msg, const char *fail_msg,
                              char *err, size_t err_cap) {
    if (out && out_cap > 0) out[0] = '\0';
    if (should_abort && should_abort(abort_user)) {
        linux_set_err(err, err_cap, cancel_msg);
        return false;
    }
    int pipefd[2] = { -1, -1 };
    if (out && out_cap > 0 && pipe(pipefd) != 0) {
        linux_set_err(err, err_cap, fail_msg);
        return false;
    }

    long limit = sysconf(_SC_OPEN_MAX);
    int max_fd = limit < STDERR_FILENO + 1 ? 256 : (limit > 4096 ? 4096 : (int)limit);
    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        linux_set_err(err, err_cap, fail_msg);
        return false;
    }
    if (pid == 0) {
        if (pipefd[1] >= 0) {
            if (pipefd[1] != STDOUT_FILENO) {
                (void)dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (pipefd[0] >= 0 && pipefd[0] != STDOUT_FILENO && pipefd[0] != pipefd[1]) {
                close(pipefd[0]);
            }
        }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            if (pipefd[1] < 0) (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) {
            close(fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (pipefd[1] >= 0) close(pipefd[1]);
    bool truncated = false;
    bool aborted = false;
    bool reaped = false;
    int status = 0;

    if (pipefd[0] >= 0) {
        size_t len = 0;
        for (;;) {
            if (should_abort && should_abort(abort_user)) {
                aborted = true;
                (void)kill(pid, SIGTERM);
                break;
            }
            struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
            int pr = poll(&pfd, 1, 50);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;
            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                char chunk[256];
                ssize_t r = read(pipefd[0], chunk, sizeof chunk);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) break;
                if (len + (size_t)r < out_cap) {
                    memcpy(out + len, chunk, (size_t)r);
                    len += (size_t)r;
                } else {
                    truncated = true;
                }
            }
        }
        close(pipefd[0]);
        out[len] = '\0';
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
            out[--len] = '\0';
        }
    }

    int abort_waits = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, should_abort ? WNOHANG : 0);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!aborted && should_abort && should_abort(abort_user)) {
            aborted = true;
            (void)kill(pid, SIGTERM);
        } else if (aborted && ++abort_waits >= 20) {
            (void)kill(pid, SIGKILL);
        }
        struct timespec ts = { 0, 25000000L };
        (void)nanosleep(&ts, NULL);
    }

    bool cancelled = aborted || (should_abort && should_abort(abort_user));
    bool ok = reaped && !cancelled && !truncated &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
              (!out || out_cap == 0 || out[0] != '\0');
    if (!ok) linux_set_err(err, err_cap, cancelled ? cancel_msg : fail_msg);
    return ok;
}

static bool linux_exe_paths(char *exe_path, size_t exe_cap,
                            char *parent_dir, size_t parent_cap,
                            const char **exe_name) {
    if (!exe_path || exe_cap < 2 || !parent_dir || parent_cap < 2) return false;
    ssize_t n = readlink("/proc/self/exe", exe_path, exe_cap - 1);
    if (n <= 0 || (size_t)n >= exe_cap - 1) return false;
    exe_path[n] = '\0';
    if (strstr(exe_path, " (deleted)")) return false;
    const char *slash = strrchr(exe_path, '/');
    if (!slash || !slash[1]) return false;
    size_t plen = (size_t)(slash - exe_path);
    if (plen == 0) {
        snprintf(parent_dir, parent_cap, "/");
    } else if (plen < parent_cap) {
        memcpy(parent_dir, exe_path, plen);
        parent_dir[plen] = '\0';
    } else {
        return false;
    }
    if (exe_name) *exe_name = slash + 1;
    return true;
}

static void linux_remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        (void)unlink(path);
        return;
    }
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            int n = snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            if (n > 0 && (size_t)n < sizeof child) linux_remove_tree(child);
        }
        closedir(dir);
    }
    (void)rmdir(path);
}

static bool linux_copy_executable(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return false;
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (out_fd < 0) {
        close(in_fd);
        return false;
    }

    bool ok = true;
    char buf[16384];
    for (;;) {
        ssize_t r = read(in_fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (r == 0) break;
        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(out_fd, buf + written, (size_t)(r - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }
    if (ok && fchmod(out_fd, 0755) != 0) ok = false;
    if (ok && fsync(out_fd) != 0) ok = false;
    close(in_fd);
    if (close(out_fd) != 0) ok = false;
    if (!ok) (void)unlink(dst);
    return ok;
}

bool app_http_resolve_redirect(const char *url, char *final_url, size_t capacity,
                               app_updater_abort_fn should_abort, void *abort_user,
                               char *err, size_t err_cap) {
    if (!url || !final_url || capacity == 0) {
        linux_set_err(err, err_cap, "Invalid update check request.");
        return false;
    }
    char *argv[] = {
        "curl", "-fsSLI", "--max-time", "15",
        "-o", "/dev/null", "-w", "%{url_effective}",
        "--", (char *)url, NULL
    };
    return linux_run_process(argv, final_url, capacity, should_abort, abort_user,
                             "Update check cancelled.",
                             "Could not reach GitHub (check network connection and curl).",
                             err, err_cap);
}

bool app_updater_can_self_update(void) {
    char exe_path[PATH_MAX], parent_dir[PATH_MAX];
    return linux_release_arch() &&
           linux_exe_paths(exe_path, sizeof exe_path, parent_dir, sizeof parent_dir, NULL) &&
           access(parent_dir, W_OK) == 0;
}

bool app_updater_install_release(const char *repo, const char *tag,
                                 char *relaunch_path, size_t relaunch_cap,
                                 app_updater_abort_fn should_abort, void *abort_user,
                                 char *err, size_t err_cap) {
    if (!repo || !tag || !app_updater_can_self_update()) {
        linux_set_err(err, err_cap, "In-place update is unavailable for this installation.");
        return false;
    }
    const char *slash = strchr(repo, '/');
    const char *arch = linux_release_arch();
    if (!slash || !slash[1] || !arch) {
        linux_set_err(err, err_cap, "Invalid repository or architecture.");
        return false;
    }

    char exe_path[PATH_MAX], parent_dir[PATH_MAX];
    const char *exe_name = NULL;
    if (!linux_exe_paths(exe_path, sizeof exe_path, parent_dir, sizeof parent_dir, &exe_name)) {
        linux_set_err(err, err_cap, "Could not locate current executable.");
        return false;
    }

    char url[1024];
    int un = snprintf(url, sizeof url,
                      "https://github.com/%s/releases/download/%s/%s-%s-linux-%s.tar.gz",
                      repo, tag, slash + 1, tag, arch);
    if (un < 0 || (size_t)un >= sizeof url) {
        linux_set_err(err, err_cap, "Release download URL is too long.");
        return false;
    }

    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || !tmp_base[0]) tmp_base = "/tmp";
    char tmp_dir[PATH_MAX];
    int tn = snprintf(tmp_dir, sizeof tmp_dir, "%s/app-update.XXXXXX", tmp_base);
    if (tn < 0 || (size_t)tn >= sizeof tmp_dir || !mkdtemp(tmp_dir)) {
        linux_set_err(err, err_cap, "Could not create a temporary directory.");
        return false;
    }

    char archive_path[PATH_MAX], extract_dir[PATH_MAX], staged_path[PATH_MAX] = "";
    bool ok = false;

    do {
        if (snprintf(archive_path, sizeof archive_path, "%s/update.tar.gz", tmp_dir) >= (int)sizeof archive_path ||
            snprintf(extract_dir, sizeof extract_dir, "%s/extract", tmp_dir) >= (int)sizeof extract_dir) {
            linux_set_err(err, err_cap, "Temporary path is too long.");
            break;
        }

        char *curl_argv[] = {
            "curl", "-fsSL", "--max-time", "180",
            "-o", archive_path, "--", url, NULL
        };
        if (!linux_run_process(curl_argv, NULL, 0, should_abort, abort_user,
                               "Update cancelled.",
                               "Could not download the Linux release archive.",
                               err, err_cap))
            break;
        if (mkdir(extract_dir, 0700) != 0) {
            linux_set_err(err, err_cap, "Could not create extraction directory.");
            break;
        }

        char *tar_argv[] = { "tar", "-xzf", archive_path, "-C", extract_dir, NULL };
        if (!linux_run_process(tar_argv, NULL, 0, should_abort, abort_user,
                               "Update cancelled.",
                               "Could not extract update archive.",
                               err, err_cap))
            break;

        char src_bin[PATH_MAX] = "";
        DIR *dir = opendir(extract_dir);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                char candidate[PATH_MAX];
                int cn = snprintf(candidate, sizeof candidate, "%s/%s", extract_dir, ent->d_name);
                if (cn <= 0 || (size_t)cn >= sizeof candidate) continue;
                struct stat st;
                if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
                    if (strcmp(ent->d_name, exe_name) == 0) {
                        snprintf(src_bin, sizeof src_bin, "%s", candidate);
                        break;
                    }
                    if (!src_bin[0]) snprintf(src_bin, sizeof src_bin, "%s", candidate);
                }
            }
            closedir(dir);
        }
        if (!src_bin[0]) {
            linux_set_err(err, err_cap, "Downloaded archive does not contain an executable.");
            break;
        }

        if (snprintf(staged_path, sizeof staged_path, "%s/.%s.updating.%ld",
                     parent_dir, exe_name, (long)getpid()) >= (int)sizeof staged_path) {
            staged_path[0] = '\0';
            linux_set_err(err, err_cap, "Destination path is too long.");
            break;
        }
        (void)unlink(staged_path);
        if (!linux_copy_executable(src_bin, staged_path)) {
            staged_path[0] = '\0';
            linux_set_err(err, err_cap, "Could not stage updated executable.");
            break;
        }
        if (should_abort && should_abort(abort_user)) {
            linux_set_err(err, err_cap, "Update cancelled.");
            break;
        }
        if (rename(staged_path, exe_path) != 0) {
            linux_set_err(err, err_cap, "Could not replace the installed executable.");
            break;
        }
        staged_path[0] = '\0';
        if (relaunch_path && relaunch_cap > 0) {
            snprintf(relaunch_path, relaunch_cap, "%s", exe_path);
        }
        ok = true;
    } while (0);

    if (staged_path[0]) (void)unlink(staged_path);
    linux_remove_tree(tmp_dir);
    return ok;
}

const char *app_reveal_in_file_manager_label(void) {
    return "Show in File Manager";
}

static bool linux_file_uri_encode(const char *path, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    if (!path || !path[0] || !out || cap < 10) return false;
    size_t o = (size_t)snprintf(out, cap, "file://");
    if (o >= cap) return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                    c == '_' || c == '.' || c == '~';
        if (safe) {
            if (o + 1 >= cap) return false;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return false;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

bool app_reveal_in_file_manager(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return false;

    char resolved[PATH_MAX];
    const char *abs_path = realpath(path, resolved) ? resolved : path;

    char uri[PATH_MAX * 3 + 16];
    char arg_uri[PATH_MAX * 3 + 32];
    if (linux_file_uri_encode(abs_path, uri, sizeof uri) &&
        snprintf(arg_uri, sizeof arg_uri, "array:string:%s", uri) < (int)sizeof arg_uri) {
        char *argv[] = {
            "dbus-send", "--session", "--print-reply", "--reply-timeout=800",
            "--dest=org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItems",
            arg_uri, "string:", NULL
        };
        if (linux_run_process(argv, NULL, 0, NULL, NULL, NULL, NULL, NULL, 0)) {
            return true;
        }
    }

    if (S_ISDIR(st.st_mode)) {
        return cui_app_open_url(abs_path);
    }
    const char *slash = strrchr(abs_path, '/');
    if (slash && slash > abs_path) {
        char parent[PATH_MAX];
        size_t plen = (size_t)(slash - abs_path);
        if (plen < sizeof parent) {
            memcpy(parent, abs_path, plen);
            parent[plen] = '\0';
            return cui_app_open_url(parent);
        }
    }
    return cui_app_open_url("/");
}
