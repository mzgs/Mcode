#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "settings.h"
#include "app_config.h"
#include "app_log.h"
#include "platform.h"
#include <yyjson.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <shlobj.h>
#include <wchar.h>
typedef wchar_t path_char;
/* shortcut: GetTempFileNameW uses MAX_PATH, switch to long-path-aware file creation if needed. */
enum { SETTINGS_PATH_CAP = MAX_PATH };
#else
#include <sys/stat.h>
#include <unistd.h>
typedef char path_char;
enum { SETTINGS_PATH_CAP = PATH_MAX };
#endif

static bool settings_path(path_char *path, const char *override) {
#ifdef _WIN32
    if (override) {
        if (!override[0]) return false;
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, override, -1, path, SETTINGS_PATH_CAP))
            return false;
        for (wchar_t *p = path; *p; ++p)
            if (*p == L'/') *p = L'\\';
        return true;
    }
    wchar_t name[SETTINGS_PATH_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, APP_NAME, -1, name, SETTINGS_PATH_CAP))
        return false;
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &base))) {
        CoTaskMemFree(base);
        return false;
    }
    int len = swprintf(path, SETTINGS_PATH_CAP, L"%ls\\%ls\\settings.json", base, name);
    CoTaskMemFree(base);
    return len >= 0 && len < SETTINGS_PATH_CAP;
#else
    if (override) {
        if (!override[0]) return false;
        int len = snprintf(path, SETTINGS_PATH_CAP, "%s", override);
        return len >= 0 && len < SETTINGS_PATH_CAP;
    }
#ifdef __APPLE__
    return macos_settings_path(path, SETTINGS_PATH_CAP);
#else
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *suffix = "";
    if (!base || base[0] != '/') {
        base = getenv("HOME");
        suffix = "/.config";
    }
    if (!base || base[0] != '/') return false;
    int len = snprintf(path, SETTINGS_PATH_CAP, "%s%s/%s/settings.json", base, suffix, APP_NAME);
    return len >= 0 && len < SETTINGS_PATH_CAP;
#endif
#endif
}

#ifdef _WIN32
static bool is_directory(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool create_directories(wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (CreateDirectoryW(path, NULL)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return is_directory(path);
    if (err != ERROR_PATH_NOT_FOUND) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || slash == path || (slash == path + 2 && path[1] == L':')) return false;
    *slash = L'\0';
    bool parent = create_directories(path);
    *slash = L'\\';
    if (!parent) return false;
    if (CreateDirectoryW(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && is_directory(path);
}
#else
static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool create_directories(char *path) {
    if (mkdir(path, 0700) == 0) return true;
    if (errno == EEXIST) return is_directory(path);
    if (errno != ENOENT) return false;
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return false;
    *slash = '\0';
    bool parent = create_directories(path);
    *slash = '/';
    if (!parent) return false;
    if (mkdir(path, 0700) == 0) return true;
    return errno == EEXIST && is_directory(path);
}
#endif

static bool parent_directory(const path_char *path, path_char *dir, bool create) {
#ifdef _WIN32
    wcscpy(dir, path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) {
        wcscpy(dir, L".");
        return true;
    }
    if (slash == dir || (slash == dir + 2 && dir[1] == L':')) slash[1] = L'\0';
    else *slash = L'\0';
    return !create || create_directories(dir);
#else
    strcpy(dir, path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        strcpy(dir, ".");
        return true;
    }
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    return !create || (strcmp(dir, "/") == 0) || create_directories(dir);
#endif
}

bool app_settings_dir(char *out, size_t capacity, const char *override, bool create) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    path_char path[SETTINGS_PATH_CAP];
    path_char dir[SETTINGS_PATH_CAP];
    if (!settings_path(path, override) || !parent_directory(path, dir, create)) return false;
#ifdef _WIN32
    int len = WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, (int)capacity, NULL, NULL);
    return len > 0 && (size_t)len <= capacity;
#else
    int len = snprintf(out, capacity, "%s", dir);
    return len >= 0 && (size_t)len < capacity;
#endif
}

int app_settings_find_keybinding(const app_settings *settings, const char *id) {
    if (!settings || !id || !id[0]) return -1;
    for (int i = 0; i < settings->keybinding_count; ++i) {
        if (strcmp(settings->keybindings[i].id, id) == 0) return i;
    }
    return -1;
}

static int find_deferred_keybinding(const app_settings *settings, const char *id) {
    if (!settings || !id || !id[0]) return -1;
    for (int i = 0; i < settings->deferred_keybinding_count; ++i) {
        if (strcmp(settings->deferred_keybindings[i].id, id) == 0) return i;
    }
    return -1;
}

int app_settings_register_keybinding(app_settings *settings, const char *id,
                                     const char *label, cui_key_combo default_combo) {
    if (!settings || !id || !id[0] || !label || !label[0]) return -1;
    size_t id_len = strlen(id);
    size_t label_len = strlen(label);
    if (id_len >= APP_KEYBINDING_ID_MAX || label_len >= APP_KEYBINDING_LABEL_MAX) return -1;
    if (default_combo.key == CUI_KEY_NONE) default_combo.mods = 0;

    int existing = app_settings_find_keybinding(settings, id);
    if (existing >= 0) {
        memcpy(settings->keybindings[existing].label, label, label_len + 1);
        settings->keybindings[existing].default_combo = default_combo;
        return existing;
    }
    if (settings->keybinding_count >= APP_MAX_KEYBINDINGS) return -1;

    cui_key_combo combo = default_combo;
    int deferred = find_deferred_keybinding(settings, id);
    if (deferred >= 0) {
        combo = settings->deferred_keybindings[deferred].combo;
        --settings->deferred_keybinding_count;
        if (deferred < settings->deferred_keybinding_count) {
            settings->deferred_keybindings[deferred] =
                settings->deferred_keybindings[settings->deferred_keybinding_count];
        }
    }

    int index = settings->keybinding_count++;
    app_keybinding *kb = &settings->keybindings[index];
    memcpy(kb->id, id, id_len + 1);
    memcpy(kb->label, label, label_len + 1);
    kb->combo = combo;
    kb->default_combo = default_combo;
    return index;
}

void app_settings_reset_keybindings(app_settings *settings) {
    if (!settings) return;
    for (int i = 0; i < settings->keybinding_count; ++i) {
        settings->keybindings[i].combo = settings->keybindings[i].default_combo;
    }
    settings->deferred_keybinding_count = 0;
}

bool app_keybinding_pressed(cui_ctx *ui, const app_settings *settings, int index) {
    if (!ui || !settings || index < 0 || index >= settings->keybinding_count) return false;
    if (cui_wants_keyboard(ui)) return false;
    cui_key_combo combo = settings->keybindings[index].combo;
    return combo.key != CUI_KEY_NONE && cui_shortcut(ui, combo.mods, combo.key);
}

static bool is_reserved_combo(cui_key_combo combo) {
    if (combo.key == CUI_KEY_COMMA && combo.mods == CUI_MOD_PRIMARY)
        return true;
#ifdef __APPLE__
    if (combo.key == CUI_KEY_F && combo.mods == (CUI_MOD_PRIMARY | CUI_MOD_CTRL))
        return true;
    if (combo.mods == CUI_MOD_PRIMARY &&
        (combo.key == CUI_KEY_Q || combo.key == CUI_KEY_W ||
         combo.key == CUI_KEY_M || combo.key == CUI_KEY_H))
        return true;
#else
    if (combo.key == CUI_KEY_Q && combo.mods == CUI_MOD_PRIMARY)
        return true;
    if (combo.key == CUI_KEY_F11 && combo.mods == 0)
        return true;
#endif
    return false;
}

int app_keybinding_conflict(const app_settings *settings, int index) {
    if (!settings || index < 0 || index >= settings->keybinding_count) return APP_CONFLICT_NONE;
    cui_key_combo combo = settings->keybindings[index].combo;
    if (combo.key == CUI_KEY_NONE) return APP_CONFLICT_NONE;
    if (is_reserved_combo(combo)) return APP_CONFLICT_SETTINGS;
    for (int i = 0; i < settings->keybinding_count; ++i) {
        if (i == index) continue;
        cui_key_combo other = settings->keybindings[i].combo;
        if (other.key == combo.key && other.mods == combo.mods) return i;
    }
    return APP_CONFLICT_NONE;
}

static bool write_settings(const path_char *path, yyjson_mut_doc *doc) {
    path_char temp[SETTINGS_PATH_CAP];
    path_char dir[SETTINGS_PATH_CAP];
    if (!parent_directory(path, dir, true)) return false;
#ifdef _WIN32
    if (!GetTempFileNameW(dir, L"cfg", 0, temp)) return false;
    FILE *file = _wfopen(temp, L"wb");
#else
    int len = snprintf(temp, sizeof temp, "%s.XXXXXX", path);
    if (len < 0 || (size_t)len >= sizeof temp) return false;
    int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    if (!file) close(fd);
#endif
    bool written = file && yyjson_mut_write_fp(file, doc, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL, NULL);
    if (file && fclose(file) != 0) written = false;
#ifdef _WIN32
    if (written && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp);
#else
    if (written && rename(temp, path) == 0) return true;
    unlink(temp);
#endif
    return false;
}

static const struct {
    int bit;
    const char *name;
} mod_entries[] = {
    { CUI_MOD_SHIFT, "shift" },
    { CUI_MOD_CTRL, "ctrl" },
    { CUI_MOD_ALT, "alt" },
    { CUI_MOD_SUPER, "super" },
};

static const char *const theme_names[APP_THEME_COUNT] = {
    "system",
    "light",
    "dark",
};

static double json_number(yyjson_val *value) {
    return yyjson_is_num(value) ? yyjson_get_num(value) : NAN;
}

static const char *json_cstr(yyjson_val *value, size_t *out_len) {
    if (!yyjson_is_str(value)) return NULL;
    const char *text = yyjson_get_str(value);
    size_t len = yyjson_get_len(value);
    if (!text || memchr(text, '\0', len)) return NULL;
    if (out_len) *out_len = len;
    return text;
}

static bool json_int_in_range(yyjson_val *value, int min, int max, int *out) {
    double n = json_number(value);
    if (!(n >= min && n <= max && n == floor(n))) return false;
    *out = (int)n;
    return true;
}

static bool str_eq_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool parse_theme_mode(yyjson_val *value, app_theme_mode *out) {
    const char *text = json_cstr(value, NULL);
    if (!text) return false;
    for (int i = 0; i < APP_THEME_COUNT; ++i) {
        if (str_eq_ignore_case(text, theme_names[i])) {
            *out = (app_theme_mode)i;
            return true;
        }
    }
    return false;
}

static bool parse_accent(yyjson_val *value, cui_accent *out) {
    const char *text = json_cstr(value, NULL);
    if (!text) return false;
    for (int i = 0; i < CUI_ACCENT_COUNT; ++i) {
        if (str_eq_ignore_case(text, cui_accent_name((cui_accent)i))) {
            *out = (cui_accent)i;
            return true;
        }
    }
    return false;
}

static bool parse_key(yyjson_val *value, cui_key *out) {
    size_t len = 0;
    const char *text = json_cstr(value, &len);
    if (!text) return false;
    if (len == 0) {
        *out = CUI_KEY_NONE;
        return true;
    }
    for (int k = 1; k < CUI_KEY_COUNT; ++k) {
        if (strcmp(cui_key_name((cui_key)k), text) == 0) {
            *out = (cui_key)k;
            return true;
        }
    }
    return false;
}

static bool parse_mods(yyjson_val *value, int *out) {
    const char *text = json_cstr(value, NULL);
    if (!text) return false;
    int bits = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') ++p;
        size_t tok_len = (size_t)(p - start);
        int matched = 0;
        for (size_t i = 0; i < sizeof mod_entries / sizeof mod_entries[0]; ++i) {
            if (strlen(mod_entries[i].name) == tok_len &&
                memcmp(mod_entries[i].name, start, tok_len) == 0) {
                matched = mod_entries[i].bit;
                break;
            }
        }
        if (!matched) return false;
        bits |= matched;
    }
    *out = bits;
    return true;
}

static void format_mods(int mods, char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof mod_entries / sizeof mod_entries[0]; ++i) {
        if (!(mods & mod_entries[i].bit)) continue;
        int n = snprintf(buf + used, cap - used, "%s%s", used ? " " : "", mod_entries[i].name);
        if (n > 0 && (size_t)n < cap - used) used += (size_t)n;
    }
}

bool app_settings_load(app_settings *settings, const char *override) {
    if (!settings) return false;
    path_char path[SETTINGS_PATH_CAP];
    if (!settings_path(path, override)) return false;
#ifdef _WIN32
    FILE *file = _wfopen(path, L"rb");
#else
    FILE *file = fopen(path, "rb");
#endif
    if (!file) {
        if (errno != ENOENT) app_log_warn("Could not read settings.json (errno=%d)", errno);
        return false;
    }
    yyjson_doc *doc = yyjson_read_fp(file, YYJSON_READ_ALLOW_BOM, NULL, NULL);
    fclose(file);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        app_log_warn("Invalid settings.json: root is not a JSON object");
        yyjson_doc_free(doc);
        return false;
    }

    app_settings loaded = *settings;
    bool applied = false;

    size_t len = 0;
    const char *text = json_cstr(yyjson_obj_get(root, "example_string"), &len);
    if (text && len < sizeof loaded.example_string) {
        memcpy(loaded.example_string, text, len + 1);
        applied = true;
    }
    double number = json_number(yyjson_obj_get(root, "example_number"));
    if (number >= EXAMPLE_NUMBER_MIN && number <= EXAMPLE_NUMBER_MAX) {
        loaded.example_number = number;
        applied = true;
    }

    yyjson_val *appearance = yyjson_obj_get(root, "appearance");
    if (yyjson_is_obj(appearance)) {
        if (parse_theme_mode(yyjson_obj_get(appearance, "theme"), &loaded.appearance.theme_mode)) {
            applied = true;
        }
        if (parse_accent(yyjson_obj_get(appearance, "accent"), &loaded.appearance.accent)) {
            applied = true;
        }
        double scale = json_number(yyjson_obj_get(appearance, "font_scale"));
        if (isfinite(scale) && scale >= FONT_SCALE_MIN - 1e-4 && scale <= FONT_SCALE_MAX + 1e-4) {
            if (scale < FONT_SCALE_MIN) scale = FONT_SCALE_MIN;
            if (scale > FONT_SCALE_MAX) scale = FONT_SCALE_MAX;
            loaded.appearance.font_scale = (float)scale;
            applied = true;
        }
        yyjson_val *reduced = yyjson_obj_get(appearance, "reduced_motion");
        if (yyjson_is_bool(reduced)) {
            loaded.appearance.reduced_motion = yyjson_get_bool(reduced);
            applied = true;
        }
    }

    yyjson_val *window = yyjson_obj_get(root, "window");
    if (yyjson_is_obj(window)) {
        int wx = 0, wy = 0;
        if (json_int_in_range(yyjson_obj_get(window, "x"), APP_WINDOW_POS_MIN, APP_WINDOW_POS_MAX, &wx) &&
            json_int_in_range(yyjson_obj_get(window, "y"), APP_WINDOW_POS_MIN, APP_WINDOW_POS_MAX, &wy)) {
            loaded.window.has_pos = true;
            loaded.window.x = wx;
            loaded.window.y = wy;
            applied = true;
        }
        if (json_int_in_range(yyjson_obj_get(window, "width"), WINDOW_WIDTH_MIN, WINDOW_WIDTH_MAX, &loaded.window.width)) {
            applied = true;
        }
        if (json_int_in_range(yyjson_obj_get(window, "height"), WINDOW_HEIGHT_MIN, WINDOW_HEIGHT_MAX, &loaded.window.height)) {
            applied = true;
        }
        yyjson_val *fullscreen = yyjson_obj_get(window, "fullscreen");
        if (yyjson_is_bool(fullscreen)) {
            loaded.window.fullscreen = yyjson_get_bool(fullscreen);
            applied = true;
        }
    }

    yyjson_val *updates = yyjson_obj_get(root, "updates");
    if (yyjson_is_obj(updates)) {
        yyjson_val *check_on_startup = yyjson_obj_get(updates, "check_on_startup");
        if (yyjson_is_bool(check_on_startup)) {
            loaded.updates.check_on_startup = yyjson_get_bool(check_on_startup);
            applied = true;
        }
        yyjson_val *last_checked = yyjson_obj_get(updates, "last_checked");
        if (yyjson_is_int(last_checked) &&
            (!yyjson_is_uint(last_checked) || yyjson_get_uint(last_checked) <= (uint64_t)INT64_MAX)) {
            int64_t ts = yyjson_get_sint(last_checked);
            if (ts >= 0) {
                loaded.updates.last_checked = ts;
                applied = true;
            }
        }
    }

    yyjson_val *keybindings = yyjson_obj_get(root, "keybindings");
    if (yyjson_is_obj(keybindings)) {
        size_t idx = 0, max = 0;
        yyjson_val *key_val = NULL, *entry = NULL;
        yyjson_obj_foreach(keybindings, idx, max, key_val, entry) {
            size_t id_len = 0;
            const char *id = json_cstr(key_val, &id_len);
            if (!id || id_len == 0 || id_len >= APP_KEYBINDING_ID_MAX || !yyjson_is_obj(entry))
                continue;
            cui_key key = CUI_KEY_NONE;
            int mods = 0;
            yyjson_val *mods_val = yyjson_obj_get(entry, "mods");
            bool mods_ok = mods_val ? parse_mods(mods_val, &mods) : true;
            if (!parse_key(yyjson_obj_get(entry, "key"), &key) || !mods_ok ||
                (key == CUI_KEY_NONE && mods != 0)) {
                continue;
            }
            cui_key_combo combo = { mods, key };
            int existing = app_settings_find_keybinding(&loaded, id);
            if (existing >= 0) {
                loaded.keybindings[existing].combo = combo;
                applied = true;
            } else if (loaded.defer_unknown_keybindings) {
                int def_idx = find_deferred_keybinding(&loaded, id);
                if (def_idx >= 0) {
                    loaded.deferred_keybindings[def_idx].combo = combo;
                    applied = true;
                } else if (loaded.deferred_keybinding_count < APP_MAX_KEYBINDINGS) {
                    app_deferred_keybinding *def =
                        &loaded.deferred_keybindings[loaded.deferred_keybinding_count++];
                    memcpy(def->id, id, id_len + 1);
                    def->combo = combo;
                    applied = true;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    if (!applied) return false;
    *settings = loaded;
    return true;
}

bool app_settings_save(const app_settings *settings, const char *override) {
    if (!settings) return false;
    path_char path[SETTINGS_PATH_CAP];
    if (!settings_path(path, override)) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root &&
        yyjson_mut_obj_add_str(doc, root, "example_string", settings->example_string) &&
        yyjson_mut_obj_add_real(doc, root, "example_number", settings->example_number);

    const char *theme_name = (settings->appearance.theme_mode >= 0 &&
                              settings->appearance.theme_mode < APP_THEME_COUNT)
        ? theme_names[settings->appearance.theme_mode] : theme_names[APP_THEME_SYSTEM];
    yyjson_mut_val *appearance = ok ? yyjson_mut_obj(doc) : NULL;
    ok = ok && appearance &&
        yyjson_mut_obj_add_str(doc, appearance, "theme", theme_name) &&
        yyjson_mut_obj_add_str(doc, appearance, "accent", cui_accent_name(settings->appearance.accent)) &&
        yyjson_mut_obj_add_real(doc, appearance, "font_scale",
                                round((double)settings->appearance.font_scale * 100.0) / 100.0) &&
        yyjson_mut_obj_add_bool(doc, appearance, "reduced_motion", settings->appearance.reduced_motion) &&
        yyjson_mut_obj_add_val(doc, root, "appearance", appearance);

    yyjson_mut_val *window = ok ? yyjson_mut_obj(doc) : NULL;
    ok = ok && window;
    if (ok && settings->window.has_pos) {
        ok = yyjson_mut_obj_add_int(doc, window, "x", settings->window.x) &&
             yyjson_mut_obj_add_int(doc, window, "y", settings->window.y);
    }
    ok = ok &&
        yyjson_mut_obj_add_int(doc, window, "width", settings->window.width) &&
        yyjson_mut_obj_add_int(doc, window, "height", settings->window.height) &&
        yyjson_mut_obj_add_bool(doc, window, "fullscreen", settings->window.fullscreen) &&
        yyjson_mut_obj_add_val(doc, root, "window", window);

    yyjson_mut_val *updates = ok ? yyjson_mut_obj(doc) : NULL;
    ok = ok && updates &&
        yyjson_mut_obj_add_bool(doc, updates, "check_on_startup", settings->updates.check_on_startup);
    if (ok && settings->updates.last_checked > 0) {
        ok = yyjson_mut_obj_add_sint(doc, updates, "last_checked", settings->updates.last_checked);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, root, "updates", updates);

    yyjson_mut_val *keybindings = ok ? yyjson_mut_obj(doc) : NULL;
    ok = ok && keybindings && yyjson_mut_obj_add_val(doc, root, "keybindings", keybindings);
    for (int i = 0; ok && i < settings->keybinding_count; ++i) {
        cui_key_combo combo = settings->keybindings[i].combo;
        char mods_buf[32];
        format_mods(combo.mods, mods_buf, sizeof mods_buf);
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        ok = entry &&
             yyjson_mut_obj_add_str(doc, entry, "key", cui_key_name(combo.key)) &&
             yyjson_mut_obj_add_strcpy(doc, entry, "mods", mods_buf) &&
             yyjson_mut_obj_add_val(doc, keybindings, settings->keybindings[i].id, entry);
    }
    bool saved = ok && write_settings(path, doc);
    yyjson_mut_doc_free(doc);
    if (!saved) app_log_error("Could not save settings.json");
    return saved;
}
