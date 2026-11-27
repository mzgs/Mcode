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

    if (cui_switch(ui, "Auto-approve agent permissions",
                   &chrome->settings.mcode.auto_approve_permissions,
                   .description = "Skip interactive permission prompts for unattended execution")) {
        changed = true;
    }

    static const char *const busy_submit_labels[APP_BUSY_SUBMIT_COUNT] = {
        "Queue", "Steer"
    };
    setting_row_begin(ui, "Enter while busy", compact, true);
    int bmode = (int)chrome->settings.mcode.busy_submit_mode;
    bool bmode_changed = compact
        ? cui_select(ui, "busy-submit-mode", busy_submit_labels, APP_BUSY_SUBMIT_COUNT,
                     &bmode, .width = CUI_FILL)
        : cui_segmented(ui, "busy-submit-mode", busy_submit_labels, APP_BUSY_SUBMIT_COUNT,
                        &bmode);
    if (bmode_changed) {
        chrome->settings.mcode.busy_submit_mode = (app_busy_submit_mode)bmode;
        changed = true;
    }
    cui_box_end(ui);
#ifdef __APPLE__
    cui_muted(ui, "Cmd+Enter or Alt+Enter triggers the alternate action while an agent is running.");
#else
    cui_muted(ui, "Ctrl+Enter or Alt+Enter triggers the alternate action while an agent is running.");
#endif

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

static void draw_theme_preview(cui_ctx *ui, app_theme_mode mode) {
    if (mode == APP_THEME_SYSTEM) {
        const app_theme_spec *light = app_theme_spec_get(APP_THEME_LIGHT);
        const app_theme_spec *dark = app_theme_spec_get(APP_THEME_DARK);
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(52), .radius = 6,
                .pad = 1, .border = 1.0f, .border_color = CUI_COLOR_BORDER,
                .clip = true, .gap = 0, .align = CUI_ALIGN_STRETCH) {
            /* Light half */
            cui_column(ui, .width = CUI_PCT(50), .height = CUI_FILL,
                       .bg = light->background, .radius_tl = 5, .radius_bl = 5, .gap = 0) {
                cui_row(ui, .width = CUI_FILL, .height = CUI_PX(12),
                        .bg = light->chrome_bg, .radius_tl = 5, .pad_x = 5, .gap = 3,
                        .align = CUI_ALIGN_CENTER) {
                    cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(light->text_muted, 0.55f)) {}
                    cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(light->text_muted, 0.40f)) {}
                }
                cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = light->border) {}
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .pad = 5, .gap = 4, .justify = CUI_JUSTIFY_CENTER) {
                    cui_box(ui, .width = CUI_PCT(82), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(light->foreground, 0.78f)) {}
                    cui_box(ui, .width = CUI_PCT(58), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(light->text_secondary, 0.55f)) {}
                    cui_box(ui, .width = CUI_PX(18), .height = CUI_PX(6), .radius = 3.0f,
                            .bg = light->info) {}
                }
            }
            cui_box(ui, .width = CUI_PX(1), .height = CUI_FILL, .bg = CUI_COLOR_BORDER_STRONG) {}
            /* Dark half */
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .bg = dark->background, .radius_tr = 5, .radius_br = 5, .gap = 0) {
                cui_row(ui, .width = CUI_FILL, .height = CUI_PX(12),
                        .bg = dark->chrome_bg, .radius_tr = 5, .pad_x = 5, .gap = 3,
                        .align = CUI_ALIGN_CENTER) {
                    cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(dark->text_muted, 0.55f)) {}
                    cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(dark->text_muted, 0.40f)) {}
                }
                cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = dark->border) {}
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .pad = 5, .gap = 4, .justify = CUI_JUSTIFY_CENTER) {
                    cui_box(ui, .width = CUI_PCT(82), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(dark->foreground, 0.78f)) {}
                    cui_box(ui, .width = CUI_PCT(58), .height = CUI_PX(3.5f), .radius = 2,
                            .bg = cui_alpha(dark->text_secondary, 0.55f)) {}
                    cui_box(ui, .width = CUI_PX(18), .height = CUI_PX(6), .radius = 3.0f,
                            .bg = dark->info) {}
                }
            }
        }
        return;
    }

    const app_theme_spec *s = app_theme_spec_get(mode);
    cui_color pill_col = s->has_accent
        ? s->accent
        : (s->is_dark ? CUI_RGB(0x3a63b8) : CUI_RGB(0x2563eb));
    cui_color bubble_bg = s->has_accent
        ? cui_mix(s->surface, s->accent, s->is_dark ? 0.20f : 0.12f)
        : s->surface_muted;

    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(52), .radius = 6,
            .pad = 1, .bg = s->background, .border = 1.0f, .border_color = s->border,
            .clip = true, .gap = 0, .align = CUI_ALIGN_STRETCH) {
        /* Mini sidebar rail */
        cui_column(ui, .width = CUI_PCT(29), .height = CUI_FILL,
                   .bg = s->chrome_bg, .radius_tl = 5, .radius_bl = 5,
                   .pad = 5, .gap = 3.5f) {
            cui_row(ui, .gap = 2.5f, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                        .bg = cui_alpha(s->danger, 0.85f)) {}
                cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                        .bg = cui_alpha(s->warning, 0.85f)) {}
                cui_box(ui, .width = CUI_PX(3.5f), .height = CUI_PX(3.5f), .radius = 2,
                        .bg = cui_alpha(s->success, 0.85f)) {}
            }
            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(7), .radius = 2.5f,
                    .bg = s->surface, .border = 0.75f, .border_color = s->border) {}
            cui_box(ui, .width = CUI_PCT(80), .height = CUI_PX(3), .radius = 1.5f,
                    .bg = cui_alpha(s->text_muted, 0.55f)) {}
            cui_box(ui, .width = CUI_PCT(62), .height = CUI_PX(3), .radius = 1.5f,
                    .bg = cui_alpha(s->text_muted, 0.35f)) {}
        }
        cui_box(ui, .width = CUI_PX(1), .height = CUI_FILL, .bg = s->border) {}
        /* Mini workspace */
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                   .bg = s->background, .radius_tr = 5, .radius_br = 5,
                   .pad = 5.5f, .gap = 4, .justify = CUI_JUSTIFY_BETWEEN) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END) {
                cui_box(ui, .width = CUI_PCT(54), .height = CUI_PX(8), .radius = 3.5f,
                        .bg = bubble_bg, .border = 0.75f, .border_color = s->border) {}
            }
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_box(ui, .width = CUI_PCT(86), .height = CUI_PX(3.5f), .radius = 2,
                        .bg = cui_alpha(s->foreground, 0.82f)) {}
                cui_box(ui, .width = CUI_PCT(62), .height = CUI_PX(3.5f), .radius = 2,
                        .bg = cui_alpha(s->text_secondary, 0.55f)) {}
            }
            cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 3.5f) {
                cui_box(ui, .width = CUI_PX(4.5f), .height = CUI_PX(4.5f), .radius = 2.25f,
                        .bg = s->info) {}
                cui_box(ui, .width = CUI_PX(4.5f), .height = CUI_PX(4.5f), .radius = 2.25f,
                        .bg = s->success) {}
                cui_box(ui, .width = CUI_PX(4.5f), .height = CUI_PX(4.5f), .radius = 2.25f,
                        .bg = s->tone_violet) {}
                cui_spacer(ui);
                cui_box(ui, .width = CUI_PX(18), .height = CUI_PX(6.5f), .radius = 3.25f,
                        .bg = pill_col) {}
            }
        }
    }
}

static bool draw_appearance_section(app_chrome *chrome, cui_ctx *ui, bool compact) {
    static const char *const accent_labels[CUI_ACCENT_COUNT] = {
        "Theme default", "Violet", "Blue", "Green", "Amber", "Orange", "Coral", "Rose"
    };
    bool changed = false;

    app_theme_mode active_mode = chrome->settings.appearance.theme_mode;
    app_theme_mode resolved_mode = chrome_resolve_theme(chrome, ui);
    const app_theme_spec *resolved_spec = app_theme_spec_get(resolved_mode);

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, "Theme", .font = CUI_FONT_MEDIUM);
        cui_spacer(ui);
        if (active_mode == APP_THEME_SYSTEM) {
            char sub[64];
            snprintf(sub, sizeof sub, "Following system · %s", resolved_spec->label);
            cui_muted(ui, sub);
        } else {
            cui_muted(ui, resolved_spec->is_dark ? "Dark theme" : "Light theme");
        }
    }

    int theme_count = 0;
    const app_theme_mode *order = app_theme_display_order(&theme_count);
    cui_box(ui, .dir = CUI_GRID, .columns = compact ? 2 : 3,
            .min_column_width = 148, .gap = 8, .width = CUI_FILL) {
        for (int i = 0; i < theme_count; ++i) {
            app_theme_mode mode = order[i];
            const app_theme_spec *spec = app_theme_spec_get(mode);
            bool selected = (active_mode == mode);

            cui_push_id(ui, spec->id);
            bool clicked = cui_card_begin(ui, &(cui_card_opts){
                .id = "theme-card",
                .dir = CUI_COLUMN,
                .width = CUI_FILL,
                .pad = 7,
                .gap = 6,
                .hoverable = true,
                .selected = selected,
                .radius = 10,
                .border = selected ? 1.5f : 1.0f,
            });
            draw_theme_preview(ui, mode);
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, spec->label,
                         .font = selected ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                         .size = 12.5f,
                         .color = selected ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_SECONDARY,
                         .width = CUI_FILL,
                         .nowrap = true);
                if (selected) {
                    cui_icon(ui, CUI_ICON_CHECK, 13.0f, CUI_COLOR_ACCENT);
                } else {
                    cui_icon(ui, spec->icon, 11.5f, CUI_COLOR_TEXT_MUTED);
                }
            }
            cui_card_end(ui);
            cui_pop_id(ui);

            if (clicked && !selected) {
                chrome->settings.appearance.theme_mode = mode;
                chrome->env_theme_override = false;
                chrome->has_cached_system_dark = false;
                chrome_apply_appearance(chrome, ui);
                cui_request_redraw(ui);
                changed = true;
            }
        }
    }

    cui_separator(ui);

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
