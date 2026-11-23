#include "chrome_internal.h"
#include "platform.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include "app_menu.h"
#endif

const char *const chrome_theme_icons[APP_THEME_COUNT] = {
    CUI_ICON_MONITOR, CUI_ICON_SUN, CUI_ICON_MOON
};

const char *const chrome_section_labels[SETTINGS_SECTION_COUNT] = {
    "General", "Appearance", "Keybindings", "About"
};

const char *const chrome_section_icons[SETTINGS_SECTION_COUNT] = {
    CUI_ICON_SETTINGS, CUI_ICON_PALETTE, CUI_ICON_KEYBOARD, CUI_ICON_INFO
};

static const app_settings k_default_settings = {
    .mcode = {
        .default_agent = APP_AGENT_CLAUDE,
        .use_worktrees_by_default = true,
        .auto_approve_permissions = false,
        .claude_bin = "claude",
        .codex_bin = "codex",
        .agy_bin = "agy",
        .default_claude_model = "",
        .default_codex_model = "",
        .default_agy_model = "",
    },
    .appearance = {
        .theme_mode = APP_THEME_SYSTEM,
        .accent = CUI_ACCENT_NEUTRAL,
        .font_scale = 1.0f,
        .reduced_motion = false,
    },
    .window = {
        .has_pos = false,
        .x = 0,
        .y = 0,
        .width = 1360,
        .height = 860,
        .fullscreen = false,
    },
    .updates = {
        .check_on_startup = false,
    },
};

app_settings chrome_default_settings(void) {
    app_settings s = k_default_settings;
    app_settings_register_keybinding(&s, APP_KEYBINDING_COMMAND_PALETTE,
                                     "Command Palette",
                                     (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_K });
    return s;
}

void chrome_reset_settings(app_chrome *chrome) {
    if (!chrome) return;
    app_settings defaults = chrome_default_settings();
    defaults.keybinding_count = chrome->settings.keybinding_count;
    memcpy(defaults.keybindings, chrome->settings.keybindings, sizeof defaults.keybindings);
    app_settings_reset_keybindings(&defaults);
    chrome->settings = defaults;
    chrome->command_palette_kb =
        app_settings_find_keybinding(&chrome->settings, APP_KEYBINDING_COMMAND_PALETTE);
}

bool app_contains_ignore_case(const char *text, const char *query) {
    for (; *text; ++text) {
        const char *a = text, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return !*query;
}

static cui_theme_mode resolve_theme_mode(app_chrome *chrome, cui_ctx *ui,
                                         app_theme_mode mode, cui_theme_mode fallback) {
    if (mode == APP_THEME_LIGHT) return CUI_THEME_LIGHT;
    if (mode == APP_THEME_DARK) return CUI_THEME_DARK;
    int dark = -1;
    if (chrome && ui) {
        double now = cui_time(ui);
        if (!chrome->has_cached_system_dark || now < chrome->system_theme_checked_at ||
            (now - chrome->system_theme_checked_at) >= 0.5) {
            chrome->cached_system_dark = app_system_dark();
            chrome->system_theme_checked_at = now;
            chrome->has_cached_system_dark = true;
        }
        dark = chrome->cached_system_dark;
    } else {
        dark = app_system_dark();
    }
    if (dark >= 0) return dark ? CUI_THEME_DARK : CUI_THEME_LIGHT;
    return fallback;
}

void app_chrome_notify(app_chrome *chrome, cui_ctx *ui, const char *message, cui_tone tone) {
    (void)chrome;
    if (ui && message) cui_toast(ui, message, .tone = tone);
}

const app_settings *app_chrome_settings(const app_chrome *chrome) {
    return &chrome->settings;
}

void chrome_open_settings_folder(app_chrome *chrome, cui_ctx *ui) {
    char dir[4096];
    if (!app_settings_dir(dir, sizeof dir, chrome->cli.settings_path, true) || !cui_app_open_url(dir)) {
        app_chrome_notify(chrome, ui, "Couldn’t open settings folder.", CUI_TONE_DANGER);
    }
}

void chrome_open_log_file(app_chrome *chrome, cui_ctx *ui) {
    char path[4096];
    if (!app_log_copy_path(path, sizeof path) || !cui_app_open_url(path)) {
        app_chrome_notify(chrome, ui, "Couldn’t open log file.", CUI_TONE_DANGER);
    }
}

void chrome_format_debug_info(const app_chrome *chrome, cui_ctx *ui, char *out, size_t cap) {
    if (!out || cap == 0) return;
    static const char *const mode_names[APP_THEME_COUNT] = { "System", "Light", "Dark" };
    char os_info[128] = "unknown";
    app_os_info(os_info, sizeof os_info);

    const char *backend = cui_app_backend(NULL);
    const char *renderer = cui_app_is_software(NULL) ? "Software" : "OpenGL";
    const char *resolved_theme = (ui && cui_get_theme_mode(ui) == CUI_THEME_DARK) ? "Dark" : "Light";
    app_theme_mode mode = chrome ? chrome->settings.appearance.theme_mode : APP_THEME_SYSTEM;
    const char *mode_name = (mode >= 0 && mode < APP_THEME_COUNT) ? mode_names[mode] : "System";
    int scale_pct = chrome ? (int)lroundf(chrome->settings.appearance.font_scale * 100.0f) : 100;
    cui_vec2 win = cui_app_ui(NULL) ? cui_app_size(NULL) : (cui_vec2){ 0, 0 };
    int win_w = (int)lroundf(win.x);
    int win_h = (int)lroundf(win.y);
    if (win_w <= 0 && chrome) win_w = chrome->settings.window.width;
    if (win_h <= 0 && chrome) win_h = chrome->settings.window.height;

    char settings_dir[4096] = "";
    if (chrome) app_settings_dir(settings_dir, sizeof settings_dir, chrome->cli.settings_path, false);
    char log_path[4096] = "";
    (void)app_log_copy_path(log_path, sizeof log_path);

    snprintf(out, cap,
             "App: %s %s\n"
             "Bundle ID: %s\n"
             "OS: %s\n"
             "Backend: %s (%s)\n"
             "Theme: %s (%s) · Scale: %d%%\n"
             "Window: %dx%d%s\n"
             "Settings dir: %s\n"
             "Log file: %s",
             APP_NAME, APP_VERSION,
             APP_BUNDLE_ID[0] ? APP_BUNDLE_ID : "none",
             os_info,
             backend ? backend : "unknown", renderer,
             resolved_theme, mode_name, scale_pct,
             win_w, win_h, (cui_app_ui(NULL) && cui_app_is_fullscreen(NULL)) ? " (fullscreen)" : "",
             settings_dir[0] ? settings_dir : "unknown",
             log_path[0] ? log_path : "disabled");
}

void chrome_toggle_fullscreen(void) {
    if (cui_app_ui(NULL)) cui_app_set_fullscreen(NULL, !cui_app_is_fullscreen(NULL));
}

static int find_action_by_id(const app_chrome *chrome, const char *id) {
    if (!chrome || !id || !id[0]) return -1;
    for (int i = 0; i < chrome->action_count; ++i) {
        if (chrome->actions[i].id[0] && strcmp(chrome->actions[i].id, id) == 0)
            return i;
    }
    return -1;
}

cui_key_combo chrome_action_combo(const app_chrome *chrome, const app_registered_action *action) {
    if (!chrome || !action) return (cui_key_combo){ 0, CUI_KEY_NONE };
    if (action->keybinding_index >= 0 && action->keybinding_index < chrome->settings.keybinding_count) {
        return chrome->settings.keybindings[action->keybinding_index].combo;
    }
    return action->default_combo;
}

cui_key_combo chrome_palette_combo(const app_chrome *chrome) {
    if (!chrome || chrome->command_palette_kb < 0 ||
        chrome->command_palette_kb >= chrome->settings.keybinding_count) {
        return (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_K };
    }
    return chrome->settings.keybindings[chrome->command_palette_kb].combo;
}

int chrome_keybinding_conflict(const app_chrome *chrome, int index) {
    if (!chrome) return APP_CONFLICT_NONE;
    int conflict = app_keybinding_conflict(&chrome->settings, index);
    if (conflict != APP_CONFLICT_NONE) return conflict;
    if (index < 0 || index >= chrome->settings.keybinding_count) return APP_CONFLICT_NONE;
    cui_key_combo combo = chrome->settings.keybindings[index].combo;
    if (combo.key == CUI_KEY_NONE) return APP_CONFLICT_NONE;
    for (int i = 0; i < chrome->action_count; ++i) {
        const app_registered_action *action = &chrome->actions[i];
        if (action->keybinding_index >= 0) continue;
        if (action->default_combo.key == combo.key &&
            action->default_combo.mods == combo.mods) {
            return APP_CONFLICT_SETTINGS;
        }
    }
    return APP_CONFLICT_NONE;
}

bool app_chrome_register_action(app_chrome *chrome, const app_action_desc *desc) {
    if (!chrome || !desc || !desc->title || !desc->title[0]) return false;
    if (chrome->action_count >= APP_MAX_ACTIONS) {
        app_log_warn("Cannot register action '%s': maximum actions (%d) reached",
                     desc->title, APP_MAX_ACTIONS);
        return false;
    }

    size_t title_len = strlen(desc->title);
    if (title_len >= APP_KEYBINDING_LABEL_MAX) {
        app_log_warn("Cannot register action: title too long");
        return false;
    }

    const char *id = (desc->id && desc->id[0]) ? desc->id : "";
    size_t id_len = strlen(id);
    if (id_len >= APP_KEYBINDING_ID_MAX) {
        app_log_warn("Cannot register action '%s': id too long", desc->title);
        return false;
    }
    if (id_len > 0) {
        if (strcmp(id, APP_KEYBINDING_COMMAND_PALETTE) == 0 ||
            find_action_by_id(chrome, id) >= 0) {
            app_log_warn("Cannot register action '%s': duplicate or reserved id '%s'",
                         desc->title, id);
            return false;
        }
    }

    const char *group = (desc->group && desc->group[0]) ? desc->group : "Actions";
    size_t group_len = strlen(group);
    if (group_len >= APP_ACTION_GROUP_MAX) {
        app_log_warn("Cannot register action '%s': group name too long", desc->title);
        return false;
    }

    const char *icon = (desc->icon && desc->icon[0]) ? desc->icon : "";
    size_t icon_len = strlen(icon);
    if (icon_len >= APP_ACTION_ICON_MAX) {
        app_log_warn("Cannot register action '%s': icon string too long", desc->title);
        return false;
    }

    cui_key_combo default_combo = desc->default_combo;
    if (default_combo.key == CUI_KEY_NONE) default_combo.mods = 0;

    int kb_index = -1;
    if (!desc->hide_from_keybindings && id_len > 0) {
        kb_index = app_settings_register_keybinding(&chrome->settings, id, desc->title, default_combo);
        if (kb_index < 0) {
            app_log_warn("Cannot register keybinding for action '%s'", id);
            return false;
        }
    }

    app_registered_action *slot = &chrome->actions[chrome->action_count++];
    memset(slot, 0, sizeof *slot);
    if (id_len > 0) memcpy(slot->id, id, id_len + 1);
    memcpy(slot->title, desc->title, title_len + 1);
    if (icon_len > 0) memcpy(slot->icon, icon, icon_len + 1);
    memcpy(slot->group, group, group_len + 1);
    slot->menu = desc->menu;
    slot->default_combo = default_combo;
    slot->keybinding_index = kb_index;
    slot->hide_from_palette = desc->hide_from_palette;
    slot->enabled = !desc->disabled;
    slot->checked = desc->checked;
    slot->on_trigger = desc->on_trigger;
    slot->user = desc->user;
    return true;
}

int app_chrome_register_actions(app_chrome *chrome, const app_action_desc *descs,
                                size_t count, void *default_user) {
    if (!chrome || !descs || count == 0) return 0;
    int registered = 0;
    for (size_t i = 0; i < count; ++i) {
        app_action_desc copy = descs[i];
        if (!copy.user) copy.user = default_user;
        if (app_chrome_register_action(chrome, &copy)) ++registered;
    }
    return registered;
}

bool chrome_trigger_action_index(app_chrome *chrome, cui_ctx *ui, int index) {
    if (!chrome || index < 0 || index >= chrome->action_count) return false;
    app_registered_action *action = &chrome->actions[index];
    if (!action->enabled) return false;
    if (action->on_trigger) action->on_trigger(ui, action->user);
    return true;
}

bool app_chrome_trigger_action(app_chrome *chrome, cui_ctx *ui, const char *id) {
    if (!chrome || !id || !id[0]) return false;
    if (strcmp(id, APP_KEYBINDING_COMMAND_PALETTE) == 0) {
        chrome->palette_open = true;
        return true;
    }
    int index = find_action_by_id(chrome, id);
    return chrome_trigger_action_index(chrome, ui, index);
}

bool app_chrome_set_action_enabled(app_chrome *chrome, const char *id, bool enabled) {
    int index = find_action_by_id(chrome, id);
    if (index < 0) return false;
    chrome->actions[index].enabled = enabled;
    return true;
}

bool app_chrome_set_action_checked(app_chrome *chrome, const char *id, bool checked) {
    int index = find_action_by_id(chrome, id);
    if (index < 0) return false;
    chrome->actions[index].checked = checked;
    return true;
}

void app_chrome_set_palette_provider(app_chrome *chrome,
                                     app_palette_provider provider, void *user) {
    if (!chrome) return;
    chrome->palette_provider = provider;
    chrome->palette_provider_user = user;
}

void chrome_apply_appearance(app_chrome *chrome, cui_ctx *ui) {
    bool first = (chrome->appearance_ui != ui);
    if (first) {
        chrome->appearance_ui = ui;
        chrome->has_cached_system_dark = false;
    }

    if (!chrome->env_theme_override) {
        cui_theme_mode target = resolve_theme_mode(chrome, ui, chrome->settings.appearance.theme_mode, CUI_THEME_LIGHT);
        cui_set_theme(ui, target);
    }
    if (cui_app_ui(NULL) == ui) {
        app_window_set_dark(cui_app_native_window(NULL), cui_app_backend(NULL),
                            cui_get_theme_mode(ui) == CUI_THEME_DARK);
    }
    if (!chrome->env_accent_override) {
        cui_set_accent(ui, chrome->settings.appearance.accent);
    }
    float scale = chrome->settings.appearance.font_scale;
    if (scale < FONT_SCALE_MIN || scale > FONT_SCALE_MAX || !isfinite(scale)) scale = 1.0f;
    cui_theme_get(ui)->font_scale = scale;

    if (chrome->appearance_force_motion || (first && chrome->settings.appearance.reduced_motion)) {
        cui_set_reduced_motion(ui, chrome->settings.appearance.reduced_motion);
        chrome->appearance_force_motion = false;
    }
}

void chrome_reload_appearance(app_chrome *chrome, cui_ctx *ui) {
    chrome->env_theme_override = false;
    chrome->env_accent_override = false;
    chrome->has_cached_system_dark = false;
    chrome->appearance_force_motion = true;
    chrome_apply_appearance(chrome, ui);
}

bool chrome_save_settings(app_chrome *chrome, cui_ctx *ui) {
    bool prev_failed = chrome->settings_save_failed;
    bool ok = app_settings_save(&chrome->settings, chrome->cli.settings_path);
    chrome->settings_save_failed = !ok;
    chrome->settings_dirty = false;
    if (ok) {
        chrome->window_dirty = false;
    } else if (!prev_failed) {
        app_chrome_notify(chrome, ui, "Couldn’t save settings.", CUI_TONE_DANGER);
    }
    return ok;
}

static void observe_window_pos(app_chrome *chrome) {
    int x = 0, y = 0;
    chrome->observed_window_has_pos =
        app_window_get_pos(cui_app_native_window(NULL), cui_app_backend(NULL), &x, &y);
    chrome->observed_window_x = x;
    chrome->observed_window_y = y;
}

static void apply_window_reset_bounds(app_chrome *chrome, cui_ctx *ui) {
    cui_app_set_size(NULL, chrome->settings.window.width, chrome->settings.window.height);
    app_window_center(cui_app_native_window(NULL), cui_app_backend(NULL),
                      chrome->settings.window.width, chrome->settings.window.height);
    observe_window_pos(chrome);
    chrome->window_reset_settling = true;
    chrome->window_reset_settle_since = ui ? cui_time(ui) : 0.0;
    if (ui) cui_request_wakeup(ui, 0.05f);
}

void chrome_reset_window(app_chrome *chrome, cui_ctx *ui) {
    if (cui_app_ui(NULL) != ui) return;
    chrome->window_fullscreen_pending = false;
    chrome->window_reset_pending = false;
    chrome->window_reset_settling = false;
    if (cui_app_is_fullscreen(NULL)) {
        cui_app_set_fullscreen(NULL, false);
        if (cui_app_is_fullscreen(NULL)) {
            chrome->window_reset_pending = true;
            chrome->window_reset_since = cui_time(ui);
            cui_request_wakeup(ui, 0.05f);
            return;
        }
    }
    apply_window_reset_bounds(chrome, ui);
}

static bool capture_live_window_bounds(app_chrome *chrome) {
    if (chrome->window_fullscreen_pending ||
        chrome->window_reset_pending || chrome->window_reset_settling)
        return false;

    bool fs = false;
    bool normal = false;
    int w = 0, h = 0;
    bool has_pos = false;
    int x = 0, y = 0;

    if (cui_app_ui(NULL)) {
        void *native = cui_app_native_window(NULL);
        const char *backend = cui_app_backend(NULL);
        fs = cui_app_is_fullscreen(NULL);
        normal = !fs && app_window_is_normal(native, backend);
        if (normal) {
            cui_vec2 win = cui_app_size(NULL);
            w = (int)lroundf(win.x);
            h = (int)lroundf(win.y);
            has_pos = app_window_get_pos(native, backend, &x, &y);
        }
    } else if (chrome->native_window && chrome->window_backend) {
        if (!app_window_get_bounds(chrome->native_window, chrome->window_backend,
                                   &fs, &normal, &w, &h, &has_pos, &x, &y)) {
            return false;
        }
        normal = !fs && normal;
    } else {
        return false;
    }

    bool changed = false;
    if (fs != chrome->settings.window.fullscreen) {
        chrome->settings.window.fullscreen = fs;
        changed = true;
    }
    if (normal) {
        if (w >= WINDOW_WIDTH_MIN && w <= WINDOW_WIDTH_MAX &&
            h >= WINDOW_HEIGHT_MIN && h <= WINDOW_HEIGHT_MAX &&
            (w != chrome->settings.window.width || h != chrome->settings.window.height)) {
            chrome->settings.window.width = w;
            chrome->settings.window.height = h;
            changed = true;
        }
        if (has_pos) {
            if (!chrome->observed_window_has_pos) {
                chrome->observed_window_has_pos = true;
                chrome->observed_window_x = x;
                chrome->observed_window_y = y;
            } else if (x != chrome->observed_window_x || y != chrome->observed_window_y) {
                chrome->observed_window_x = x;
                chrome->observed_window_y = y;
                if (!chrome->settings.window.has_pos ||
                    x != chrome->settings.window.x || y != chrome->settings.window.y) {
                    chrome->settings.window.has_pos = true;
                    chrome->settings.window.x = x;
                    chrome->settings.window.y = y;
                    changed = true;
                }
            }
        }
    }
    if (changed) chrome->window_dirty = true;
    return changed;
}

static void save_if_dirty(app_chrome *chrome) {
    if (chrome->settings_dirty || chrome->window_dirty) {
        app_settings_save(&chrome->settings, chrome->cli.settings_path);
        chrome->settings_dirty = false;
        chrome->window_dirty = false;
    }
}

void app_chrome_shutdown(app_chrome *chrome) {
    if (!chrome) return;
    save_if_dirty(chrome);
    chrome->native_window = NULL;
    chrome->window_backend = NULL;
    if (chrome->updater) {
        app_updater_destroy(chrome->updater);
        chrome->updater = NULL;
    }
#ifdef __APPLE__
    app_menu_shutdown();
#endif
    app_platform_shutdown();
    app_log_info("Shutting down %s", APP_NAME);
    app_log_uninstall_crash_handler();
    app_log_shutdown();
}

static app_updater *chrome_ensure_updater(app_chrome *chrome) {
    if (!chrome->updater && app_updater_configured()) {
        chrome->updater = app_updater_create(APP_GITHUB_REPO, APP_VERSION);
    }
    return chrome->updater;
}

void chrome_check_for_updates(app_chrome *chrome, bool silent) {
    app_updater *updater = chrome_ensure_updater(chrome);
    if (updater) {
        chrome->update_auto_checked = true;
        if (app_updater_check(updater)) {
            chrome->update_check_pending = true;
            chrome->update_check_silent = silent;
            chrome->update_notified = false;
            chrome->update_relaunch_failed = false;
        }
    }
}

void chrome_start_update(app_chrome *chrome, cui_ctx *ui) {
    app_update_snapshot snap;
    chrome_updater_snapshot(chrome, &snap);
    if (!snap.has_update) return;

    if (ui && chrome->update_toast_handle > 0) {
        cui_toast_dismiss(ui, chrome->update_toast_handle);
        chrome->update_toast_handle = 0;
    }

    chrome->update_check_silent = false;
    if (app_updater_can_self_update()) {
        app_updater *updater = chrome_ensure_updater(chrome);
        if (updater && app_updater_start_install(updater)) {
            if (!(chrome->settings_open && chrome->settings_section == SETTINGS_SECTION_ABOUT)) {
                const char *ver = snap.latest_tag[0] ? snap.latest_tag : snap.latest_version;
                char msg[96];
                snprintf(msg, sizeof msg, "Updating to %s…", ver);
                app_chrome_notify(chrome, ui, msg, CUI_TONE_INFO);
            }
        }
        return;
    }

    if (snap.release_url[0] && cui_app_open_url(snap.release_url)) {
        app_chrome_notify(chrome, ui, "Opened release page in browser.", CUI_TONE_INFO);
    } else {
        app_chrome_notify(chrome, ui, "Couldn’t open release page.", CUI_TONE_DANGER);
    }
}

void chrome_updater_snapshot(app_chrome *chrome, app_update_snapshot *out) {
    app_updater_snapshot(chrome ? chrome->updater : NULL, out);
}

static void chrome_poll_updater(app_chrome *chrome, cui_ctx *ui) {
    if (chrome->update_toast_handle > 0 &&
        cui_toast_action_clicked(ui, chrome->update_toast_handle)) {
        cui_toast_dismiss(ui, chrome->update_toast_handle);
        chrome->update_toast_handle = 0;
        chrome_start_update(chrome, ui);
    }

    if (!chrome->updater) return;
    app_update_snapshot snap;
    app_updater_snapshot(chrome->updater, &snap);

    if (snap.state == APP_UPDATE_CHECKING || snap.state == APP_UPDATE_UPDATING) {
        cui_request_wakeup(ui, 0.1f);
    }

    if (chrome->update_check_pending) {
        if (snap.state == APP_UPDATE_UP_TO_DATE || snap.state == APP_UPDATE_AVAILABLE) {
            chrome->update_check_pending = false;
            int64_t now = (int64_t)time(NULL);
            if (now > 0) {
                chrome->settings.updates.last_checked = now;
                chrome_save_settings(chrome, ui);
            }
        } else if (snap.state == APP_UPDATE_ERROR) {
            chrome->update_check_pending = false;
        }
    }

    bool about_visible = chrome->settings_open && chrome->settings_section == SETTINGS_SECTION_ABOUT;
    if (snap.state == APP_UPDATE_AVAILABLE && snap.has_update && !chrome->update_notified) {
        chrome->update_notified = true;
        if (!about_visible) {
            const char *ver = snap.latest_tag[0] ? snap.latest_tag : snap.latest_version;
            char title[96];
            snprintf(title, sizeof title, "Update available: %s", ver);
            const char *desc = app_updater_can_self_update()
                ? "Click Update to download and relaunch."
                : "Click Update to open the release page.";
            chrome->update_toast_handle = cui_toast(ui, title,
                .description = desc,
                .tone = CUI_TONE_INFO,
                .duration = 8.0f,
                .action = "Update");
        }
    } else if (snap.state == APP_UPDATE_READY_TO_RELAUNCH && !chrome->update_relaunching) {
        chrome->update_relaunching = true;
        capture_live_window_bounds(chrome);
        save_if_dirty(chrome);
        if (app_updater_relaunch(snap.relaunch_path)) {
            cui_app_quit(NULL);
        } else {
            chrome->update_relaunch_failed = true;
            app_chrome_notify(chrome, ui, "Update installed. Please restart the app.", CUI_TONE_SUCCESS);
        }
    } else if (snap.state == APP_UPDATE_ERROR &&
               chrome->last_update_state != APP_UPDATE_ERROR &&
               !chrome->update_check_silent &&
               !about_visible) {
        app_chrome_notify(chrome, ui,
                          snap.error[0] ? snap.error : "Update failed.",
                          CUI_TONE_DANGER);
    }
    chrome->last_update_state = (int)snap.state;
}

#ifdef __APPLE__
static void on_menu_action(int action_index, void *user) {
    app_chrome *chrome = user;
    chrome->pending_action = action_index;
}

static void on_menu_edit_key(cui_key key, void *user) {
    app_chrome *chrome = user;
    chrome->pending_edit_key = key;
}

static void on_menu_toggle_fullscreen(void *user) {
    (void)user;
    chrome_toggle_fullscreen();
}

static void on_menu_will_terminate(void *user) {
    app_chrome *chrome = user;
    capture_live_window_bounds(chrome);
    save_if_dirty(chrome);
}
#else
static bool has_menu_actions(const app_chrome *chrome, app_menu_group group) {
    for (int i = 0; i < chrome->action_count; ++i) {
        if (chrome->actions[i].menu == group) return true;
    }
    return false;
}

static void draw_menu_group_actions(app_chrome *chrome, cui_ctx *ui, app_menu_group group) {
    for (int i = 0; i < chrome->action_count; ++i) {
        const app_registered_action *action = &chrome->actions[i];
        if (action->menu != group) continue;
        cui_key_combo combo = chrome_action_combo(chrome, action);
        const char *shortcut = combo.key != CUI_KEY_NONE ? cui_shortcut_label(ui, combo) : NULL;
        cui_push_id_int(ui, i);
        if (cui_menu_item(ui, action->title,
                          .icon = action->icon[0] ? action->icon : NULL,
                          .shortcut = shortcut,
                          .disabled = !action->enabled,
                          .checked = action->checked)) {
            chrome_trigger_action_index(chrome, ui, i);
        }
        cui_pop_id(ui);
    }
}
#endif

static void on_window_moved(void *user) {
    (void)user;
    cui_ctx *ui = cui_app_ui(NULL);
    if (ui) cui_request_redraw(ui);
}

static void on_window_closing(void *user) {
    app_chrome *chrome = user;
    capture_live_window_bounds(chrome);
    save_if_dirty(chrome);
}

static void on_window_theme_changed(void *user) {
    app_chrome *chrome = user;
    if (chrome) chrome->has_cached_system_dark = false;
    cui_ctx *ui = cui_app_ui(NULL);
    if (ui) cui_request_redraw(ui);
}

static void sync_window_state(app_chrome *chrome, cui_ctx *ui) {
    if (cui_app_ui(NULL) != ui) return;
    if (chrome->window_state_ui != ui) {
        chrome->window_state_ui = ui;
        chrome->window_fullscreen_pending = false;
        chrome->window_reset_pending = false;
        chrome->window_reset_settling = false;
        const char *backend = cui_app_backend(NULL);
        void *native = cui_app_native_window(NULL);
        chrome->native_window = native;
        chrome->window_backend = backend;
        app_log_info("Window ready (backend=%s, renderer=%s)",
                     backend ? backend : "unknown",
                     cui_app_is_software(NULL) ? "Software" : "OpenGL");
        app_window_apply_metadata(native, backend);
        app_window_set_hooks(native, backend, &(app_window_hooks){
            .user = chrome,
            .on_moved = on_window_moved,
            .on_closing = on_window_closing,
            .on_theme_changed = on_window_theme_changed,
        });
        if (chrome->settings.window.has_pos) {
            app_window_set_pos(native, backend,
                               chrome->settings.window.x, chrome->settings.window.y);
        }
        observe_window_pos(chrome);
        if (chrome->settings.window.fullscreen) {
            cui_app_set_fullscreen(NULL, true);
            chrome->window_fullscreen_pending = true;
            chrome->window_fullscreen_since = cui_time(ui);
            cui_request_wakeup(ui, 0.05f);
        }
        if (!chrome->update_startup_checked &&
            !chrome->env_screenshot_mode &&
            chrome->settings.updates.check_on_startup &&
            app_updater_configured()) {
            chrome->update_startup_checked = true;
            if (app_updater_check_due(chrome->settings.updates.last_checked, (int64_t)time(NULL))) {
                chrome_check_for_updates(chrome, true);
            }
        }
    }
    if (chrome->window_reset_pending) {
        if (!cui_app_is_fullscreen(NULL)) {
            chrome->window_reset_pending = false;
            apply_window_reset_bounds(chrome, ui);
        } else if (cui_time(ui) - chrome->window_reset_since >= 2.0) {
            chrome->window_reset_pending = false;
        } else {
            cui_request_wakeup(ui, 0.05f);
        }
    }
    if (chrome->window_reset_settling) {
        cui_vec2 win = cui_app_size(NULL);
        int w = (int)lroundf(win.x);
        int h = (int)lroundf(win.y);
        bool size_matched = (w == chrome->settings.window.width && h == chrome->settings.window.height);
        if (size_matched || (cui_time(ui) - chrome->window_reset_settle_since >= 0.5)) {
            chrome->window_reset_settling = false;
            observe_window_pos(chrome);
        } else {
            cui_request_wakeup(ui, 0.05f);
        }
    }
    if (chrome->window_fullscreen_pending) {
        if (cui_app_is_fullscreen(NULL) || (cui_time(ui) - chrome->window_fullscreen_since >= 1.5)) {
            chrome->window_fullscreen_pending = false;
        } else {
            cui_request_wakeup(ui, 0.05f);
        }
    }
    if (!chrome->window_fullscreen_pending && !chrome->window_reset_pending &&
        !chrome->window_reset_settling && capture_live_window_bounds(chrome)) {
        chrome->window_dirty_since = cui_time(ui);
    }
    if (chrome->window_dirty) {
        double elapsed = cui_time(ui) - chrome->window_dirty_since;
        if (elapsed >= 0.5) {
            chrome->settings_save_failed = !app_settings_save(&chrome->settings, chrome->cli.settings_path);
            chrome->window_dirty = false;
        } else {
            cui_request_wakeup(ui, (float)(0.5 - elapsed) + 0.02f);
        }
    }
}

void app_chrome_begin_frame(app_chrome *chrome, cui_ctx *ui) {
    chrome_apply_appearance(chrome, ui);
    sync_window_state(chrome, ui);

#ifdef __APPLE__
    chrome->injected_edit_key = CUI_KEY_NONE;
    if (chrome->pending_edit_key != CUI_KEY_NONE) {
        chrome->injected_edit_key = chrome->pending_edit_key;
        chrome->pending_edit_key = CUI_KEY_NONE;
        cui_input_key(ui, chrome->injected_edit_key, true, CUI_MOD_PRIMARY);
    }

    app_menu_install(&(app_menu_hooks){
        .settings_open = &chrome->settings_open,
        .settings_section = &chrome->settings_section,
        .about_section = SETTINGS_SECTION_ABOUT,
        .palette_open = &chrome->palette_open,
        .user = chrome,
        .on_action = on_menu_action,
        .on_edit_key = on_menu_edit_key,
        .on_toggle_fullscreen = on_menu_toggle_fullscreen,
        .on_will_terminate = on_menu_will_terminate,
    });
    app_menu_action menu_actions[APP_MAX_ACTIONS];
    int menu_action_count = 0;
    for (int i = 0; i < chrome->action_count; ++i) {
        const app_registered_action *action = &chrome->actions[i];
        if (action->menu == APP_MENU_NONE) continue;
        menu_actions[menu_action_count++] = (app_menu_action){
            .action_index = i,
            .title = action->title,
            .menu = action->menu,
            .combo = chrome_action_combo(chrome, action),
            .enabled = action->enabled,
            .checked = action->checked,
        };
    }
    app_menu_sync(&(app_menu_state){
        .palette_combo = chrome_palette_combo(chrome),
        .fullscreen = cui_app_ui(NULL) ? cui_app_is_fullscreen(NULL) : chrome->settings.window.fullscreen,
        .actions = menu_actions,
        .action_count = menu_action_count,
    });
    if (chrome->pending_action >= 0) {
        int queued = chrome->pending_action;
        chrome->pending_action = -1;
        chrome_trigger_action_index(chrome, ui, queued);
    }
#else
    cui_menu_bar(ui, "app-menu") {
        cui_menu_bar_menu(ui, "File") {
            if (has_menu_actions(chrome, APP_MENU_FILE)) {
                draw_menu_group_actions(chrome, ui, APP_MENU_FILE);
                cui_menu_separator(ui);
            }
            if (cui_menu_item(ui, "Settings…", .icon = CUI_ICON_SETTINGS, .shortcut = "Ctrl ,"))
                chrome->settings_open = true;
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Quit", .icon = CUI_ICON_LOG_OUT, .shortcut = "Ctrl Q"))
                cui_app_quit(NULL);
        }
        if (has_menu_actions(chrome, APP_MENU_EDIT)) {
            cui_menu_bar_menu(ui, "Edit") {
                draw_menu_group_actions(chrome, ui, APP_MENU_EDIT);
            }
        }
        cui_menu_bar_menu(ui, "View") {
            cui_key_combo palette_combo = chrome_palette_combo(chrome);
            if (cui_menu_item(ui, "Command Palette…", .icon = CUI_ICON_COMMAND,
                              .shortcut = palette_combo.key != CUI_KEY_NONE
                                  ? cui_shortcut_label(ui, palette_combo) : NULL))
                chrome->palette_open = true;
            if (cui_menu_item(ui, "Fullscreen", .icon = CUI_ICON_MAXIMIZE_2,
                              .shortcut = "F11", .checked = cui_app_is_fullscreen(NULL)))
                chrome_toggle_fullscreen();
            if (has_menu_actions(chrome, APP_MENU_VIEW)) {
                cui_menu_separator(ui);
                draw_menu_group_actions(chrome, ui, APP_MENU_VIEW);
            }
        }
        cui_menu_bar_menu(ui, "Help") {
            if (cui_menu_item(ui, "About " APP_NAME, .icon = CUI_ICON_INFO)) {
                chrome->settings_section = SETTINGS_SECTION_ABOUT;
                chrome->settings_open = true;
            }
        }
    }
#endif
}

static void handle_chrome_shortcuts(app_chrome *chrome, cui_ctx *ui) {
#ifdef __APPLE__
    if (chrome->injected_edit_key != CUI_KEY_NONE) return;
    if (!chrome->settings_open &&
        cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_CTRL, CUI_KEY_F)) {
        chrome_toggle_fullscreen();
        return;
    }
#else
    if (!chrome->settings_open) {
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_COMMA)) {
            chrome->palette_open = false;
            chrome->settings_open = true;
            return;
        }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Q)) {
            cui_app_quit(NULL);
            return;
        }
        if (cui_shortcut(ui, 0, CUI_KEY_F11)) {
            chrome_toggle_fullscreen();
            return;
        }
    }
#endif
    if (chrome->settings_open || chrome->palette_open || cui_wants_keyboard(ui)) return;
    if (app_keybinding_pressed(ui, &chrome->settings, chrome->command_palette_kb)) {
        chrome->palette_open = true;
        return;
    }
    for (int i = 0; i < chrome->action_count; ++i) {
        const app_registered_action *action = &chrome->actions[i];
        if (!action->enabled) continue;
        cui_key_combo combo = chrome_action_combo(chrome, action);
        if (combo.key != CUI_KEY_NONE && cui_shortcut(ui, combo.mods, combo.key)) {
            chrome_trigger_action_index(chrome, ui, i);
            break;
        }
    }
}

void app_chrome_end_frame(app_chrome *chrome, cui_ctx *ui) {
    bool palette_was_open = chrome->palette_open;
    handle_chrome_shortcuts(chrome, ui);
    /* Defer drawing a newly shortcut-opened palette to the next frame so the
     * triggering key/text event cannot leak into it (e.g. '/', Enter, Esc). */
    bool defer_palette = !palette_was_open && chrome->palette_open;
    if (defer_palette) {
        cui_request_redraw(ui);
    } else {
        chrome_draw_command_palette(chrome, ui);
    }
    chrome_draw_settings_dialog(chrome, ui);
    chrome_poll_updater(chrome, ui);
#ifdef __APPLE__
    if (chrome->injected_edit_key != CUI_KEY_NONE) {
        cui_input_key(ui, chrome->injected_edit_key, false, 0);
        cui_input_key(ui, CUI_KEY_NONE, false, 0);
        chrome->injected_edit_key = CUI_KEY_NONE;
    }
#endif
}

static bool parse_cli(int argc, char **argv, app_cli_opts *opts, int *exit_code) {
    memset(opts, 0, sizeof *opts);
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strncmp(arg, "-psn_", 5) == 0) {
            /* macOS LaunchServices passes -psn_* when launching a bundle. */
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            fprintf(stdout,
                    "Usage: %s [options]\n\n"
                    "Options:\n"
                    "  -h, --help             Show this help message and exit\n"
                    "  -v, --version          Print version information and exit\n"
                    "  --settings <path>      Use a custom settings.json file\n"
                    "  --software             Force the software renderer\n",
                    argc > 0 && argv[0] ? argv[0] : APP_NAME);
            if (exit_code) *exit_code = 0;
            return false;
        }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            fprintf(stdout, "%s %s\n", APP_NAME, APP_VERSION);
            if (exit_code) *exit_code = 0;
            return false;
        }
        if (strcmp(arg, "--software") == 0) {
            opts->software_renderer = true;
            continue;
        }
        const char *settings_arg = NULL;
        if (strcmp(arg, "--settings") == 0) {
            settings_arg = (i + 1 < argc) ? argv[++i] : "";
        } else if (strncmp(arg, "--settings=", 11) == 0) {
            settings_arg = arg + 11;
        }
        if (settings_arg) {
            if (!settings_arg[0]) {
                fprintf(stderr, "Missing path for --settings\n");
                if (exit_code) *exit_code = 2;
                return false;
            }
            opts->settings_path = settings_arg;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\nTry --help for usage.\n", arg);
        if (exit_code) *exit_code = 2;
        return false;
    }
    return true;
}

static bool is_valid_cui_theme_env(const char *val) {
    return val && (strcmp(val, "dark") == 0 || strcmp(val, "light") == 0);
}

static bool is_valid_cui_accent_env(const char *val) {
    if (!val || !val[0]) return false;
    for (int i = 0; i < CUI_ACCENT_COUNT; ++i) {
        if (strcmp(val, cui_accent_name((cui_accent)i)) == 0) return true;
    }
    return false;
}

bool app_chrome_init(app_chrome *chrome, int argc, char **argv, int *exit_code) {
    if (!chrome) {
        if (exit_code) *exit_code = 1;
        return false;
    }
    app_cli_opts cli;
    if (!parse_cli(argc, argv, &cli, exit_code)) return false;

    memset(chrome, 0, sizeof *chrome);
    chrome->settings = chrome_default_settings();
    chrome->command_palette_kb =
        app_settings_find_keybinding(&chrome->settings, APP_KEYBINDING_COMMAND_PALETTE);
    chrome->cli = cli;
    if (!chrome->cli.settings_path) {
        const char *env_settings = getenv("APP_SETTINGS_PATH");
        if (env_settings && env_settings[0]) {
            int en = snprintf(chrome->env_settings_path, sizeof chrome->env_settings_path,
                              "%s", env_settings);
            if (en > 0 && (size_t)en < sizeof chrome->env_settings_path) {
                chrome->cli.settings_path = chrome->env_settings_path;
            }
        }
    }
#ifdef __APPLE__
    chrome->pending_action = -1;
#endif
    char log_dir[4096];
    if (app_settings_dir(log_dir, sizeof log_dir, chrome->cli.settings_path, true)) {
        char log_path[4096];
        size_t dlen = strlen(log_dir);
#ifdef _WIN32
        const char *sep = (dlen > 0 && (log_dir[dlen - 1] == '/' || log_dir[dlen - 1] == '\\')) ? "" : "\\";
#else
        const char *sep = (dlen > 0 && log_dir[dlen - 1] == '/') ? "" : "/";
#endif
        int n = snprintf(log_path, sizeof log_path, "%s%sapp.log", log_dir, sep);
        if (n > 0 && (size_t)n < sizeof log_path) {
            app_log_init(log_path, 0);
        }
    }
    app_log_install_crash_handler();
    app_log_info("Starting %s %s", APP_NAME, APP_VERSION);
    chrome->settings.defer_unknown_keybindings = true;
    app_settings_load(&chrome->settings, chrome->cli.settings_path);
    chrome->settings.defer_unknown_keybindings = false;
    const char *env_theme = getenv("CUI_THEME");
    const char *env_accent = getenv("CUI_ACCENT");
    const char *env_screenshot = getenv("CUI_SCREENSHOT");
    chrome->env_theme_override = is_valid_cui_theme_env(env_theme);
    chrome->env_accent_override = is_valid_cui_accent_env(env_accent);
    chrome->env_screenshot_mode = env_screenshot && env_screenshot[0] != '\0';
    return true;
}

cui_app_desc app_chrome_app_desc(const app_chrome *chrome) {
    return (cui_app_desc){
        .title = APP_NAME,
        .width = chrome->settings.window.width,
        .height = chrome->settings.window.height,
        .renderer = chrome->cli.software_renderer ? CUI_RENDERER_SOFTWARE : CUI_RENDERER_AUTO,
        .theme = resolve_theme_mode(NULL, NULL, chrome->settings.appearance.theme_mode, CUI_THEME_LIGHT),
        .accent = chrome->settings.appearance.accent,
        .follow_system_theme = false,
    };
}
