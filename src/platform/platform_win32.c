#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "app_config.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

int app_system_dark(void) {
    HKEY key;
    DWORD value = 1, size = sizeof value;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return -1;
    LONG r = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return -1;
    return value == 0 ? 1 : 0;
}

bool app_os_info(char *out, size_t capacity) {
    if (!out || capacity == 0) return false;
#if defined(_M_ARM64) || defined(__aarch64__)
    const char *arch = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    const char *arch = "x64";
#elif defined(_M_IX86) || defined(__i386__)
    const char *arch = "x86";
#else
    const char *arch = "unknown";
#endif
    int n = snprintf(out, capacity, "Windows (%s)", arch);
    return n > 0 && (size_t)n < capacity;
}

typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

static HWND g_hook_hwnd = NULL;
static WNDPROC g_prev_wndproc = NULL;
static app_window_hooks g_window_hooks = { 0 };

static LRESULT CALLBACK win32_window_hook_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC prev = g_prev_wndproc;
    app_window_hooks hooks = g_window_hooks;
    if (msg == WM_CLOSE && hooks.on_closing) {
        hooks.on_closing(hooks.user);
    } else if (msg == WM_EXITSIZEMOVE && hooks.on_moved) {
        hooks.on_moved(hooks.user);
    } else if (msg == WM_SETTINGCHANGE && lp &&
               lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0 &&
               hooks.on_theme_changed) {
        hooks.on_theme_changed(hooks.user);
    } else if (msg == WM_NCDESTROY && hwnd == g_hook_hwnd) {
        if (prev) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)prev);
        g_hook_hwnd = NULL;
        g_prev_wndproc = NULL;
        memset(&g_window_hooks, 0, sizeof g_window_hooks);
    }
    return prev ? CallWindowProcW(prev, hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

bool app_window_is_normal(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return true;
    HWND hwnd = (HWND)native_window;
    return !IsIconic(hwnd) && !IsZoomed(hwnd);
}

bool app_window_get_pos(void *native_window, const char *backend, int *x, int *y) {
    if (!native_window || !backend || !x || !y || strcmp(backend, "win32") != 0) return false;
    HWND hwnd = (HWND)native_window;
    if (IsIconic(hwnd) || IsZoomed(hwnd)) return false;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    *x = (int)r.left;
    *y = (int)r.top;
    return true;
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
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return false;
    if (x < APP_WINDOW_POS_MIN || x > APP_WINDOW_POS_MAX ||
        y < APP_WINDOW_POS_MIN || y > APP_WINDOW_POS_MAX)
        return false;
    HWND hwnd = (HWND)native_window;
    RECT cur;
    if (!GetWindowRect(hwnd, &cur)) return false;
    LONG w = cur.right - cur.left;
    LONG h = cur.bottom - cur.top;
    LONG bar_h = h < 32 ? h : 32;
    RECT titlebar = { x, y, x + w, y + bar_h };
    HMONITOR mon = MonitorFromRect(&titlebar, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi = { .cbSize = sizeof mi };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT overlap;
    if (!IntersectRect(&overlap, &titlebar, &mi.rcWork) ||
        (overlap.right - overlap.left) < 64 || (overlap.bottom - overlap.top) < 16)
        return false;
    return SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

void app_window_center(void *native_window, const char *backend, int width, int height) {
    (void)width;
    (void)height;
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return;
    HWND hwnd = (HWND)native_window;
    RECT r;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { .cbSize = sizeof mi };
    if (GetWindowRect(hwnd, &r) && mon && GetMonitorInfoW(mon, &mi)) {
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        int cx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        int cy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        SetWindowPos(hwnd, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void app_window_apply_metadata(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return;
    HWND hwnd = (HWND)native_window;
    HINSTANCE inst = GetModuleHandleW(NULL);
    HANDLE small_icon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR | LR_SHARED);
    if (small_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    }
    HANDLE big_icon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON),
                                 LR_DEFAULTCOLOR | LR_SHARED);
    if (big_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big_icon);
    }
}

void app_window_set_hooks(void *native_window, const char *backend, const app_window_hooks *hooks) {
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return;
    HWND hwnd = (HWND)native_window;
    if (!hooks) {
        if (hwnd == g_hook_hwnd && g_prev_wndproc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_wndproc);
            g_hook_hwnd = NULL;
            g_prev_wndproc = NULL;
            memset(&g_window_hooks, 0, sizeof g_window_hooks);
        }
        return;
    }
    if (hwnd != g_hook_hwnd) {
        if (g_hook_hwnd && IsWindow(g_hook_hwnd) && g_prev_wndproc) {
            SetWindowLongPtrW(g_hook_hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_wndproc);
        }
        WNDPROC prev = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                            (LONG_PTR)win32_window_hook_proc);
        if (!prev) return;
        g_hook_hwnd = hwnd;
        g_prev_wndproc = prev;
    }
    g_window_hooks = *hooks;
}

void app_window_set_dark(void *native_window, const char *backend, bool dark) {
    if (!native_window || !backend || strcmp(backend, "win32") != 0) return;
    static PFN_DwmSetWindowAttribute set_attr = NULL;
    static bool tried = false;
    static HWND last_hwnd = NULL;
    static int last_dark = -1;
    HWND hwnd = (HWND)native_window;
    int want = dark ? 1 : 0;
    if (hwnd == last_hwnd && want == last_dark) return;
    if (!tried) {
        tried = true;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            set_attr = (PFN_DwmSetWindowAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        }
    }
    if (!set_attr) return;
    BOOL v = dark ? TRUE : FALSE;
    if (FAILED(set_attr(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof v))) {
        set_attr(hwnd, 19 /* pre-20H1 DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof v);
    }
    last_hwnd = hwnd;
    last_dark = want;
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
    if (g_hook_hwnd && IsWindow(g_hook_hwnd) && g_prev_wndproc) {
        SetWindowLongPtrW(g_hook_hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_wndproc);
    }
    g_hook_hwnd = NULL;
    g_prev_wndproc = NULL;
    memset(&g_window_hooks, 0, sizeof g_window_hooks);
}

static void win32_set_err(char *err, size_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg ? msg : "Unknown error.");
}

bool app_http_resolve_redirect(const char *url, char *final_url, size_t capacity,
                               app_updater_abort_fn should_abort, void *abort_user,
                               char *err, size_t err_cap) {
    if (!url || !final_url || capacity == 0) {
        win32_set_err(err, err_cap, "Invalid update check request.");
        return false;
    }
    final_url[0] = '\0';
    if (should_abort && should_abort(abort_user)) {
        win32_set_err(err, err_cap, "Update check cancelled.");
        return false;
    }

    wchar_t wide_url[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url,
                            (int)(sizeof wide_url / sizeof wide_url[0])) <= 0) {
        win32_set_err(err, err_cap, "Invalid update URL.");
        return false;
    }

    wchar_t host[256] = { 0 }, path[1536] = { 0 };
    URL_COMPONENTSW parts = {
        .dwStructSize = sizeof parts,
        .lpszHostName = host,
        .dwHostNameLength = (DWORD)(sizeof host / sizeof host[0]),
        .lpszUrlPath = path,
        .dwUrlPathLength = (DWORD)(sizeof path / sizeof path[0]),
    };
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) || !host[0]) {
        win32_set_err(err, err_cap, "Invalid update URL.");
        return false;
    }

    bool ok = false;
    HINTERNET session = NULL, connect = NULL, request = NULL;

    do {
        wchar_t ua_buf[256];
        const wchar_t *user_agent =
            MultiByteToWideChar(CP_UTF8, 0, APP_NAME "/" APP_VERSION, -1,
                                ua_buf, (int)(sizeof ua_buf / sizeof ua_buf[0])) > 0
                ? ua_buf : L"CUIApp";
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
        session = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session && GetLastError() == ERROR_INVALID_PARAMETER) {
            session = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
#else
        session = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
        if (!session) {
            win32_set_err(err, err_cap, "Could not initialize HTTP client.");
            break;
        }
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

        if (should_abort && should_abort(abort_user)) {
            win32_set_err(err, err_cap, "Update check cancelled.");
            break;
        }

        connect = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connect) {
            win32_set_err(err, err_cap, "Could not connect to GitHub.");
            break;
        }

        DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connect, L"HEAD", path[0] ? path : L"/",
                                     NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            win32_set_err(err, err_cap, "Could not create HTTP request.");
            break;
        }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, NULL)) {
            win32_set_err(err, err_cap, "Could not reach GitHub.");
            break;
        }
        if (should_abort && should_abort(abort_user)) {
            win32_set_err(err, err_cap, "Update check cancelled.");
            break;
        }

        DWORD status = 0;
        DWORD status_size = sizeof status;
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX) ||
            status < 200 || status >= 400) {
            char buf[64];
            snprintf(buf, sizeof buf, "Server returned HTTP %lu.", (unsigned long)status);
            win32_set_err(err, err_cap, buf);
            break;
        }

        wchar_t resolved[2048];
        DWORD resolved_bytes = (DWORD)sizeof resolved;
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, resolved, &resolved_bytes)) {
            win32_set_err(err, err_cap, "Could not read resolved update URL.");
            break;
        }
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, resolved, -1,
                                           final_url, (int)capacity, NULL, NULL);
        if (utf8_len <= 0) {
            final_url[0] = '\0';
            win32_set_err(err, err_cap, "Resolved URL is too long.");
            break;
        }
        ok = true;
    } while (0);

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool app_updater_can_self_update(void) {
    return false;
}

bool app_updater_install_release(const char *repo, const char *tag,
                                 char *relaunch_path, size_t relaunch_cap,
                                 app_updater_abort_fn should_abort, void *abort_user,
                                 char *err, size_t err_cap) {
    (void)repo;
    (void)tag;
    (void)relaunch_path;
    (void)relaunch_cap;
    (void)should_abort;
    (void)abort_user;
    win32_set_err(err, err_cap, "In-place update is not supported on Windows.");
    return false;
}
