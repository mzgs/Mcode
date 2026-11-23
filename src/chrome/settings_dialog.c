#include "chrome_internal.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>

static void setting_row_begin(cui_ctx *ui, const char *label, bool compact, bool center) {
    cui_box_begin(ui, &(cui_box){
        .dir = compact ? CUI_COLUMN : CUI_ROW,
        .gap = compact ? 6 : 16,
        .align = center ? (compact ? CUI_ALIGN_STRETCH : CUI_ALIGN_CENTER) : CUI_ALIGN_DEFAULT,
    });
    cui_text(ui, label, .font = CUI_FONT_MEDIUM, .width = compact ? CUI_FILL : CUI_PX(168));
}

static bool draw_general_section(app_chrome *chrome, cui_ctx *ui, bool compact) {
    static const char *const agent_labels[APP_AGENT_COUNT] = {
        "Claude Code", "Codex", "Antigravity CLI"
    };
    bool changed = false;

    setting_row_begin(ui, "Default agent", compact, true);
    int agent = (int)chrome->settings.mcode.default_agent;
    bool agent_changed = compact
        ? cui_select(ui, "default-agent", agent_labels, APP_AGENT_COUNT, &agent, .width = CUI_FILL)
        : cui_segmented(ui, "default-agent", agent_labels, APP_AGENT_COUNT, &agent);
    if (agent_changed) {
        chrome->settings.mcode.default_agent = (app_agent_kind)agent;
        changed = true;
    }
    cui_box_end(ui);

    if (cui_switch(ui, "Isolate new threads in Git worktrees",
                   &chrome->settings.mcode.use_worktrees_by_default,
                   .description = "Create a dedicated branch and worktree under ~/.mcode/worktrees/ for each thread")) {
        changed = true;
    }
    if (cui_switch(ui, "Auto-approve agent permissions",
                   &chrome->settings.mcode.auto_approve_permissions,
                   .description = "Pass unattended execution flags (best used with isolated worktrees)")) {
        changed = true;
    }

    cui_separator(ui);
    cui_text(ui, "Agent CLI Binaries", .font = CUI_FONT_MEDIUM);

    setting_row_begin(ui, "Claude CLI", compact, false);
    int f_claude = cui_input(ui, "claude-bin", chrome->settings.mcode.claude_bin,
                             sizeof chrome->settings.mcode.claude_bin,
                             .placeholder = "claude", .width = CUI_FILL);
    if (f_claude & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_claude & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    setting_row_begin(ui, "Codex CLI", compact, false);
    int f_codex = cui_input(ui, "codex-bin", chrome->settings.mcode.codex_bin,
                            sizeof chrome->settings.mcode.codex_bin,
                            .placeholder = "codex", .width = CUI_FILL);
    if (f_codex & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_codex & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    setting_row_begin(ui, "Antigravity CLI", compact, false);
    int f_agy = cui_input(ui, "agy-bin", chrome->settings.mcode.agy_bin,
                          sizeof chrome->settings.mcode.agy_bin,
                          .placeholder = "agy", .width = CUI_FILL);
    if (f_agy & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_agy & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    cui_separator(ui);
    cui_text(ui, "Default Models (optional)", .font = CUI_FONT_MEDIUM);

    setting_row_begin(ui, "Claude model", compact, false);
    int f_cm = cui_input(ui, "default-claude-model", chrome->settings.mcode.default_claude_model,
                         sizeof chrome->settings.mcode.default_claude_model,
                         .placeholder = "CLI default (e.g. opus, sonnet)", .width = CUI_FILL);
    if (f_cm & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_cm & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    setting_row_begin(ui, "Codex model", compact, false);
    int f_xm = cui_input(ui, "default-codex-model", chrome->settings.mcode.default_codex_model,
                         sizeof chrome->settings.mcode.default_codex_model,
                         .placeholder = "CLI default (e.g. gpt-6.2-sol)", .width = CUI_FILL);
    if (f_xm & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_xm & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    setting_row_begin(ui, "Antigravity model", compact, false);
    int f_am = cui_input(ui, "default-agy-model", chrome->settings.mcode.default_agy_model,
                         sizeof chrome->settings.mcode.default_agy_model,
                         .placeholder = "CLI default (e.g. gemini-4-neon-high)", .width = CUI_FILL);
    if (f_am & CUI_CHANGED) chrome->settings_dirty = true;
    if (chrome->settings_dirty && (f_am & (CUI_SUBMITTED | CUI_BLURRED))) changed = true;
    cui_box_end(ui);

    cui_separator(ui);

    setting_row_begin(ui, "Settings file", compact, false);
    cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true) {
        if (cui_button(ui, "Import…", .icon = CUI_ICON_UPLOAD)) {
            char path[4096];
            if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                    .kind = CUI_FILE_OPEN,
                    .title = "Import Settings",
                    .filter_name = "JSON",
                    .filter = "json",
                }, path, sizeof path)) {
                app_settings imported = chrome->settings;
                if (app_settings_load(&imported, path)) {
                    /* Keep live window geometry and local update timestamp intact. */
                    imported.window = chrome->settings.window;
                    imported.updates.last_checked = chrome->settings.updates.last_checked;
                    chrome->settings = imported;
                    chrome_reload_appearance(chrome, ui);
                    changed = false;
                    if (chrome_save_settings(chrome, ui))
                        app_chrome_notify(chrome, ui, "Settings imported.", CUI_TONE_SUCCESS);
                } else {
                    app_chrome_notify(chrome, ui, "Couldn’t import settings.", CUI_TONE_DANGER);
                }
            }
        }
        if (cui_button(ui, "Export…", .icon = CUI_ICON_DOWNLOAD)) {
            char path[4096];
            if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                    .kind = CUI_FILE_SAVE,
                    .title = "Export Settings",
                    .filter_name = "JSON",
                    .filter = "json",
                    .default_name = "settings.json",
                }, path, sizeof path)) {
                if (app_settings_save(&chrome->settings, path))
                    app_chrome_notify(chrome, ui, "Settings exported.", CUI_TONE_SUCCESS);
                else
                    app_chrome_notify(chrome, ui, "Couldn’t export settings.", CUI_TONE_DANGER);
            }
        }
        if (cui_button(ui, "Open Folder", .icon = CUI_ICON_FOLDER_OPEN)) {
            chrome_open_settings_folder(chrome, ui);
        }
    }
    cui_box_end(ui);

    setting_row_begin(ui, "Reset settings", compact, false);
    if (cui_confirm_button(ui, "Reset", .icon = CUI_ICON_ROTATE_CCW, .confirm_label = "Confirm")) {
        chrome_reset_settings(chrome);
        chrome_reload_appearance(chrome, ui);
        chrome_reset_window(chrome, ui);
        changed = false;
        if (chrome_save_settings(chrome, ui))
            app_chrome_notify(chrome, ui, "Settings reset to defaults.", CUI_TONE_INFO);
    }
    cui_box_end(ui);

    return changed;
}

static bool draw_appearance_section(app_chrome *chrome, cui_ctx *ui, bool compact) {
    static const char *const theme_labels[APP_THEME_COUNT] = { "System", "Light", "Dark" };
    static const char *const accent_labels[CUI_ACCENT_COUNT] = {
        "Neutral", "Violet", "Blue", "Green", "Amber", "Orange", "Coral", "Rose"
    };
    bool changed = false;

    setting_row_begin(ui, "Theme", compact, true);
    int mode = (int)chrome->settings.appearance.theme_mode;
    bool theme_changed = compact
        ? cui_select(ui, "theme-mode", theme_labels, APP_THEME_COUNT, &mode,
                     .icons = chrome_theme_icons, .width = CUI_FILL)
        : cui_segmented(ui, "theme-mode", theme_labels, APP_THEME_COUNT, &mode,
                        .icons = chrome_theme_icons);
    if (theme_changed) {
        chrome->settings.appearance.theme_mode = (app_theme_mode)mode;
        chrome->env_theme_override = false;
        chrome->has_cached_system_dark = false;
        chrome_apply_appearance(chrome, ui);
        changed = true;
    }
    cui_box_end(ui);

    setting_row_begin(ui, "Accent color", compact, true);
    int accent = (int)chrome->settings.appearance.accent;
    if (cui_select(ui, "accent-color", accent_labels, CUI_ACCENT_COUNT, &accent, .width = CUI_FILL)) {
        chrome->settings.appearance.accent = (cui_accent)accent;
        chrome->env_accent_override = false;
        chrome_apply_appearance(chrome, ui);
        changed = true;
    }
    cui_box_end(ui);

    setting_row_begin(ui, "Text scale", compact, true);
    float percent = roundf(chrome->settings.appearance.font_scale * 100.0f);
    if (cui_slider(ui, "##text-scale", &percent,
                   FONT_SCALE_MIN * 100.0f, FONT_SCALE_MAX * 100.0f,
                   .step = 5.0f, .show_value = true, .format = "%.0f%%",
                   .width = CUI_FILL)) {
        chrome->settings.appearance.font_scale = percent / 100.0f;
        chrome_apply_appearance(chrome, ui);
        changed = true;
    }
    cui_box_end(ui);

    cui_separator(ui);
    if (cui_switch(ui, "Reduce motion", &chrome->settings.appearance.reduced_motion,
                   .description = "Disable UI animations")) {
        chrome->appearance_force_motion = true;
        chrome_apply_appearance(chrome, ui);
        changed = true;
    }

    return changed;
}

static bool draw_keybindings_section(app_chrome *chrome, cui_ctx *ui) {
    bool changed = false;
    cui_table_column columns[] = {
        { "Action", CUI_FILL, CUI_TEXT_LEFT, false },
        { "Keys", CUI_PX(240), CUI_TEXT_LEFT, false },
    };
    cui_secondary(ui, "Click a shortcut, then press a key combination. Backspace clears.");
    cui_search(ui, "keybinding-filter", chrome->keybinding_filter,
               sizeof chrome->keybinding_filter,
               .placeholder = "Filter actions or keys...", .width = CUI_FILL);

    bool has_conflict = false;
    int visible = 0;
    for (int i = 0; i < chrome->settings.keybinding_count; ++i) {
        const app_keybinding *kb = &chrome->settings.keybindings[i];
        if (!has_conflict && chrome_keybinding_conflict(chrome, i) != APP_CONFLICT_NONE) {
            has_conflict = true;
        }
        if (app_contains_ignore_case(kb->label, chrome->keybinding_filter) ||
            app_contains_ignore_case(cui_shortcut_label(ui, kb->combo), chrome->keybinding_filter)) {
            ++visible;
        }
    }
    if (has_conflict) {
        cui_alert(ui, "Shortcut conflict",
                  .description = "Duplicate or reserved shortcuts are highlighted below.",
                  .tone = CUI_TONE_WARNING);
    }

    if (visible == 0) {
        cui_secondary(ui, "No shortcuts match your filter.");
        return false;
    }

    cui_table(ui, "keybindings", columns, 2, .striped = true, .compact = true, .height = CUI_FILL) {
        for (int i = 0; i < chrome->settings.keybinding_count; ++i) {
            app_keybinding *kb = &chrome->settings.keybindings[i];
            if (!app_contains_ignore_case(kb->label, chrome->keybinding_filter) &&
                !app_contains_ignore_case(cui_shortcut_label(ui, kb->combo), chrome->keybinding_filter))
                continue;
            int conflict = chrome_keybinding_conflict(chrome, i);
            cui_table_row(ui, false);
            if (conflict == APP_CONFLICT_NONE) {
                cui_table_cell_text(ui, kb->label);
            } else {
                cui_table_cell(ui);
                cui_text(ui, kb->label);
                cui_badge(ui, conflict == APP_CONFLICT_SETTINGS ? "Reserved" : "Conflict",
                          .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
            }
            cui_table_cell(ui);
            cui_theme *theme = cui_theme_get(ui);
            float control_height = theme->control_md;
            theme->control_md = theme->control_sm;
            if (cui_shortcut_recorder(ui, kb->id, &kb->combo, .width = CUI_FILL)) changed = true;
            theme->control_md = control_height;
        }
    }
    return changed;
}

static bool draw_about_section(app_chrome *chrome, cui_ctx *ui) {
    bool changed = false;
    cui_text(ui, APP_NAME, .font = CUI_FONT_MEDIUM);
    cui_secondary(ui, "Version " APP_VERSION);
    if (APP_DESCRIPTION[0]) cui_secondary(ui, APP_DESCRIPTION);

    if (app_updater_configured()) {
        if (chrome->settings_open && !chrome->update_auto_checked) {
            chrome_check_for_updates(chrome, false);
        }
        app_update_snapshot upd;
        chrome_updater_snapshot(chrome, &upd);
        bool can_self_update = app_updater_can_self_update();
        const char *ver_label = upd.latest_tag[0] ? upd.latest_tag : upd.latest_version;

        cui_separator(ui);
        switch (upd.state) {
        case APP_UPDATE_CHECKING:
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_spinner(ui, 16);
                cui_secondary(ui, "Checking for updates…");
            }
            break;
        case APP_UPDATE_UP_TO_DATE:
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .wrap = true) {
                cui_badge(ui, "Up to date", .tone = CUI_TONE_SUCCESS, .icon = CUI_ICON_CHECK);
                cui_secondary(ui, "You have the latest version.");
                if (cui_button(ui, "Check Again", .variant = CUI_BUTTON_GHOST,
                               .size = CUI_SIZE_SM, .icon = CUI_ICON_REFRESH_CW)) {
                    chrome_check_for_updates(chrome, false);
                }
            }
            break;
        case APP_UPDATE_AVAILABLE: {
            char title[96];
            snprintf(title, sizeof title, "Update available: %s", ver_label);
            const char *desc = can_self_update
                ? "Download, install, and relaunch to update."
                : "A newer release is available on GitHub.";
            cui_alert(ui, title, .description = desc, .tone = CUI_TONE_INFO);
            break;
        }
        case APP_UPDATE_UPDATING: {
            char label[96];
            snprintf(label, sizeof label, "Updating to %s…", ver_label);
            cui_secondary(ui, label);
            cui_progress(ui, 0.0f, .indeterminate = true, .width = CUI_FILL);
            break;
        }
        case APP_UPDATE_READY_TO_RELAUNCH:
            cui_alert(ui, "Update installed",
                      .description = chrome->update_relaunch_failed
                          ? "Please restart the app to use the updated version."
                          : "Relaunching the updated version…",
                      .tone = CUI_TONE_SUCCESS);
            break;
        case APP_UPDATE_ERROR:
            cui_alert(ui, upd.has_update ? "Update failed" : "Couldn’t check for updates",
                      .description = upd.error[0] ? upd.error : "Please try again later.",
                      .tone = CUI_TONE_DANGER);
            if (!upd.has_update) {
                if (cui_button(ui, "Try Again", .variant = CUI_BUTTON_SECONDARY,
                               .size = CUI_SIZE_SM, .icon = CUI_ICON_REFRESH_CW)) {
                    chrome_check_for_updates(chrome, false);
                }
            }
            break;
        default:
            break;
        }

        if (upd.has_update && upd.state != APP_UPDATE_READY_TO_RELAUNCH) {
            cui_row(ui, .gap = 8, .wrap = true) {
                if (cui_button(ui, "Update",
                               .variant = CUI_BUTTON_PRIMARY,
                               .icon = can_self_update ? CUI_ICON_DOWNLOAD : CUI_ICON_EXTERNAL_LINK,
                               .loading = (upd.state == APP_UPDATE_UPDATING),
                               .tooltip = can_self_update ? "Download, install, and relaunch"
                                                          : "Open release page in browser")) {
                    chrome_start_update(chrome, ui);
                }
                if (can_self_update && upd.release_url[0]) {
                    if (cui_button(ui, "Release Notes",
                                   .variant = CUI_BUTTON_GHOST,
                                   .icon = CUI_ICON_EXTERNAL_LINK,
                                   .disabled = (upd.state == APP_UPDATE_UPDATING))) {
                        if (!cui_app_open_url(upd.release_url))
                            app_chrome_notify(chrome, ui, "Couldn’t open release page.", CUI_TONE_DANGER);
                    }
                }
            }
        }

        if (cui_switch(ui, "Check for updates on startup",
                       &chrome->settings.updates.check_on_startup,
                       .description = "Silently check GitHub Releases at most once a day on launch")) {
            changed = true;
        }
    }

    cui_separator(ui);
    char debug_info[4096];
    chrome_format_debug_info(chrome, ui, debug_info, sizeof debug_info);
    char log_path[4096] = "";
    bool has_log = app_log_copy_path(log_path, sizeof log_path);

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, "Diagnostics", .font = CUI_FONT_MEDIUM);
        cui_spacer(ui);
        if (has_log) {
            if (cui_button(ui, "Open Log", .icon = CUI_ICON_FILE_TEXT,
                           .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) {
                chrome_open_log_file(chrome, ui);
            }
        }
        if (cui_copy_button(ui, "copy-debug-info", debug_info)) {
            app_chrome_notify(chrome, ui, "Debug info copied to clipboard.", CUI_TONE_INFO);
        }
    }
    cui_column(ui, .width = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED,
               .radius = 10, .pad_x = 12, .pad_y = 10) {
        cui_text(ui, debug_info, .font = CUI_FONT_MONO, .size = 12, .selectable = true);
    }
    return changed;
}

void chrome_draw_settings_dialog(app_chrome *chrome, cui_ctx *ui) {
    cui_vec2 size = cui_viewport_size(ui);
    bool compact = size.x < 600.0f;
    app_update_snapshot nav_upd;
    chrome_updater_snapshot(chrome, &nav_upd);

    cui_dialog(ui, "Settings", &chrome->settings_open, .width = size.x * 0.9f) {
        /* shortcut: pinned CUI adds 94pt for the header, padding and gap; revisit if its layout changes. */
        cui_column(ui, .height = CUI_PX(fmaxf(0.0f, fminf(size.y * 0.9f, size.y - 48) - 94))) {
            cui_row(ui, .height = CUI_FILL, .gap = 16, .align = CUI_ALIGN_STRETCH) {
                cui_column(ui, .width = CUI_PCT(30), .min_width = 128, .max_width = 180,
                           .height = CUI_FILL) {
                    cui_scroll(ui, .id = "settings-sidebar", .gap = 4) {
                        for (int i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
                            if (i == SETTINGS_SECTION_ABOUT) cui_separator(ui);
                            const char *badge = (i == SETTINGS_SECTION_ABOUT && nav_upd.has_update) ? "New" : NULL;
                            if (cui_nav_item(ui, chrome_section_labels[i], .icon = chrome_section_icons[i],
                                             .badge = badge,
                                             .selected = chrome->settings_section == i,
                                             .group = "settings-sections")) {
                                chrome->settings_section = i;
                            }
                        }
                    }
                }
                cui_separator(ui);
                cui_scroll(ui, .id = "settings-page", .width = CUI_FILL) {
                    cui_column(ui, .height = chrome->settings_section == SETTINGS_SECTION_KEYBINDINGS ? CUI_FILL : CUI_FIT,
                               .gap = 12) {
                        bool changed = false;
                        cui_heading(ui, chrome_section_labels[chrome->settings_section], 4);
                        switch (chrome->settings_section) {
                        case SETTINGS_SECTION_GENERAL:
                            changed = draw_general_section(chrome, ui, compact);
                            break;
                        case SETTINGS_SECTION_APPEARANCE:
                            changed = draw_appearance_section(chrome, ui, compact);
                            break;
                        case SETTINGS_SECTION_KEYBINDINGS:
                            changed = draw_keybindings_section(chrome, ui);
                            break;
                        default:
                            changed = draw_about_section(chrome, ui);
                            break;
                        }
                        if (changed || (chrome->settings_dirty && chrome->settings_section != SETTINGS_SECTION_GENERAL)) {
                            chrome_save_settings(chrome, ui);
                        }
                        if (chrome->settings_save_failed) cui_secondary(ui, "Couldn’t save settings.");
                    }
                }
            }
        }
    }
    if (!chrome->settings_open && chrome->settings_dirty) {
        chrome_save_settings(chrome, ui);
    }
    if (chrome->update_auto_checked &&
        (!chrome->settings_open || chrome->settings_section != SETTINGS_SECTION_ABOUT)) {
        app_update_snapshot upd;
        chrome_updater_snapshot(chrome, &upd);
        if (upd.state == APP_UPDATE_UP_TO_DATE || (upd.state == APP_UPDATE_ERROR && !upd.has_update)) {
            chrome->update_auto_checked = false;
        }
    }
}
