#include "mcode_ui_internal.h"

#include <stdio.h>
#include <string.h>

static bool is_agent_busy(const mcode_app *app, app_agent_kind agent) {
    if (!app) return false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->agent == agent && (t->active_run != NULL || t->title_run != NULL)) {
            return true;
        }
    }
    return false;
}

static const char *active_terminal_cwd(mcode_app *app) {
    if (!app) return NULL;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t && t->workdir[0]) return t->workdir;
    if (app->store.selected_project_id[0]) {
        mcode_project *p = mcode_store_find_project(&app->store, app->store.selected_project_id);
        if (p && p->path[0]) return p->path;
    }
    if (app->store.project_count > 0 && app->store.projects[0].path[0]) {
        return app->store.projects[0].path;
    }
    return NULL;
}

static void draw_cli_update_card(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                 const mcode_cli_update_info *info, bool *out_close_pop) {
    bool ag_busy = is_agent_busy(app, agent);

    cui_push_id_int(ui, (int)agent);
    cui_card(ui, .width = CUI_FILL, .muted = true, .pad_x = 10, .pad_y = 8,
             .radius = 9, .gap = 5) {
        cui_row(ui, .width = CUI_FILL, .gap = 5, .align = CUI_ALIGN_CENTER) {
            mcode_ui_draw_provider_icon(app, ui, agent, 14.0f, CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, mcode_agent_label(agent), .font = CUI_FONT_MEDIUM, .size = 12.5f,
                     .color = CUI_COLOR_FOREGROUND, .nowrap = true);

            if (info->update_available && info->installed_version[0] && info->latest_version[0]) {
                cui_text(ui, info->installed_version, .size = 11.0f,
                         .color = CUI_COLOR_TEXT_MUTED, .tabular = true, .nowrap = true);
                cui_text(ui, "→", .size = 11.0f,
                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                cui_badge(ui, info->latest_version, .tone = CUI_TONE_INFO, .size = CUI_SIZE_SM);
            } else if (info->installed_version[0]) {
                cui_text(ui, info->installed_version, .size = 11.0f,
                         .color = CUI_COLOR_TEXT_MUTED, .tabular = true, .nowrap = true);
            }

            cui_spacer(ui);

            if (info->updating) {
                cui_button(ui, "Updating", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_SECONDARY, .loading = true);
            } else if (info->update_available) {
                const char *tip = ag_busy
                    ? "Finish active turns for this provider before updating"
                    : (info->update_cmd_display[0] ? info->update_cmd_display : "Update CLI");
                if (cui_button(ui, "Update", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_DOWNLOAD,
                               .disabled = ag_busy,
                               .tooltip = tip)) {
                    if (!mcode_cli_updates_start_install(&app->cli_updates,
                                                         &app->cli_status, agent)) {
                        app_chrome_notify(app->chrome, ui,
                                          "Could not start CLI update.", CUI_TONE_DANGER);
                    }
                }
            } else if (info->checking) {
                cui_spinner(ui, 11.0f);
            } else if (info->state == MCODE_CLI_UPD_UP_TO_DATE) {
                cui_badge(ui, "Up to date", .tone = CUI_TONE_SUCCESS,
                          .icon = CUI_ICON_CHECK, .size = CUI_SIZE_SM);
            } else if (info->state == MCODE_CLI_UPD_ERROR) {
                cui_badge(ui, "Unavailable", .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
            }
        }

        if (info->updating && info->status_line[0]) {
            cui_text(ui, info->status_line, .font = CUI_FONT_MONO, .size = 10.5f,
                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
        }

        if (info->error[0]) {
            cui_text(ui, info->error, .size = 11.0f, .color = CUI_COLOR_DANGER);
        }

        if ((info->update_available || info->error[0]) && !info->updating &&
            (info->update_cmd_display[0] || info->release_url[0])) {
            cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
                if (info->update_cmd_display[0]) {
                    cui_text(ui, info->update_cmd_display, .font = CUI_FONT_MONO, .size = 10.5f,
                             .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL, .nowrap = true);
                } else {
                    cui_spacer(ui);
                }

                if (info->update_cmd_display[0]) {
                    if (cui_icon_button(ui, CUI_ICON_SQUARE_TERMINAL, .size = CUI_SIZE_SM,
                                        .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Run update command in Terminal")) {
                        mcode_ui_run_terminal_cmd(app, ui, info->update_cmd_display,
                                                  active_terminal_cwd(app));
                        if (out_close_pop) *out_close_pop = true;
                    }
                }
                if (info->release_url[0]) {
                    if (cui_icon_button(ui, CUI_ICON_EXTERNAL_LINK, .size = CUI_SIZE_SM,
                                        .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Open release notes")) {
                        if (!cui_app_open_url(info->release_url)) {
                            app_chrome_notify(app->chrome, ui,
                                              "Could not open release notes.", CUI_TONE_DANGER);
                        }
                    }
                }
            }
        }
    }
    cui_pop_id(ui);
}

static void draw_cli_updates_popover_body(mcode_app *app, cui_ctx *ui,
                                          const mcode_cli_updates_snapshot *snap,
                                          bool *out_close_pop) {
    static const app_agent_kind k_order[APP_AGENT_COUNT] = {
        APP_AGENT_CLAUDE, APP_AGENT_CODEX, APP_AGENT_AGY
    };

    bool any_idle_updatable = false;
    for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
        app_agent_kind ag = k_order[oi];
        if (snap->items[ag].installed && snap->items[ag].update_available &&
            !snap->items[ag].updating && !is_agent_busy(app, ag)) {
            any_idle_updatable = true;
            break;
        }
    }

    cui_column(ui, .width = CUI_FILL, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "CLI Updates", .font = CUI_FONT_MEDIUM, .size = 13.0f,
                     .color = CUI_COLOR_FOREGROUND, .nowrap = true);
            cui_spacer(ui);
            if (snap->available_count >= 2 && snap->updating_count == 0) {
                if (cui_button(ui, "Update All", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_DOWNLOAD,
                               .disabled = !any_idle_updatable,
                               .tooltip = any_idle_updatable
                                   ? "Update all idle provider CLIs"
                                   : "Finish active turns before updating")) {
                    for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
                        app_agent_kind ag = k_order[oi];
                        if (snap->items[ag].installed && snap->items[ag].update_available &&
                            !snap->items[ag].updating && !is_agent_busy(app, ag)) {
                            mcode_cli_updates_start_install(&app->cli_updates,
                                                            &app->cli_status, ag);
                        }
                    }
                }
            }
            if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .disabled = (snap->checking_count > 0 || snap->updating_count > 0),
                                .tooltip = "Check for provider CLI updates")) {
                mcode_cli_updates_check_all(&app->cli_updates, &app->cli_status, false);
                app->cli_updates_checked_at = mcode_now_mono();
            }
        }

        cui_column(ui, .id = "cli-updates-scroll", .width = CUI_FILL,
                   .max_height = 340.0f, .scroll_y = true, .gap = 7) {
            int shown = 0;
            /* Show providers with available updates or active installs first. */
            for (int pass = 0; pass < 2; ++pass) {
                for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
                    app_agent_kind ag = k_order[oi];
                    if (!app->cli_status.available[ag]) continue;
                    const mcode_cli_update_info *info = &snap->items[ag];
                    bool urgent = info->update_available || info->updating;
                    if ((pass == 0) != urgent) continue;
                    draw_cli_update_card(app, ui, ag, info, out_close_pop);
                    ++shown;
                }
            }
            if (shown == 0) {
                cui_text(ui, "No provider CLIs detected.", .size = 12.0f,
                         .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
}

void mcode_ui_draw_status_cli_updates(mcode_app *app, cui_ctx *ui) {
    if (!app || !ui) return;

    mcode_cli_updates_snapshot snap;
    mcode_cli_updates_snapshot_get(&app->cli_updates, &snap);

    bool open = cui_popover_is_open(ui, "mcode-cli-updates-pop");
    if (snap.available_count == 0 && snap.updating_count == 0 &&
        !open && !app->cli_updates_open_requested) {
        return;
    }

    static const app_agent_kind k_order[APP_AGENT_COUNT] = {
        APP_AGENT_CLAUDE, APP_AGENT_CODEX, APP_AGENT_AGY
    };

    cui_vec2 vp = cui_viewport_size(ui);
    float vw = vp.x > 0.0f ? vp.x : 1280.0f;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);

    cui_id btn_id = cui_make_id(ui, "sb-cli-updates");
    cui_rect prev_r = cui_get_rect(ui, btn_id);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = prev_r.w > 0.0f &&
               mp.x >= prev_r.x && mp.x <= prev_r.x + prev_r.w &&
               mp.y >= prev_r.y && mp.y <= prev_r.y + prev_r.h;
    bool prs = hov && cui_mouse_down(ui, CUI_MOUSE_LEFT);

    cui_color info_col = cui_color_of(ui, CUI_COLOR_INFO);
    cui_color fg_col = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color sec_col = cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);

    bool has_action = (snap.available_count > 0 || snap.updating_count > 0);
    cui_color bg;
    if (has_action) {
        bg = prs ? cui_alpha(info_col, dark ? 0.28f : 0.20f)
           : (open || hov) ? cui_alpha(info_col, dark ? 0.21f : 0.15f)
                           : cui_alpha(info_col, dark ? 0.13f : 0.09f);
    } else {
        bg = prs ? cui_alpha(fg_col, 0.12f)
           : (open || hov) ? cui_color_of(ui, CUI_COLOR_HOVER)
                           : CUI_RGBA(0x00000001u);
    }
    cui_color text_col = (open || hov) ? fg_col : (has_action ? fg_col : sec_col);
    float text_sz = cui_theme_get(ui)->text_sm;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "sb-cli-updates",
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .height = CUI_PX(22),
        .pad_x = 7,
        .pad_y = 1,
        .gap = 5,
        .hoverable = true,
        .border = -1.0f,
        .radius = 4.0f,
        .bg = bg,
    });

    if (snap.updating_count > 0) {
        cui_spinner(ui, 10.0f);
        char lbl[48];
        if (snap.updating_count == 1) {
            app_agent_kind upd_ag = APP_AGENT_CLAUDE;
            for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
                if (snap.items[k_order[oi]].updating) {
                    upd_ag = k_order[oi];
                    break;
                }
            }
            snprintf(lbl, sizeof lbl, "Updating %s...", mcode_agent_short_label(upd_ag));
        } else {
            snprintf(lbl, sizeof lbl, "Updating CLIs...");
        }
        cui_text(ui, lbl, .font = CUI_FONT_MEDIUM, .size = text_sz,
                 .color = text_col, .nowrap = true);
    } else if (snap.available_count > 0) {
        cui_icon(ui, CUI_ICON_ARROW_UP, 11.5f, info_col);

        app_agent_kind only_ag = APP_AGENT_CLAUDE;
        for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
            app_agent_kind ag = k_order[oi];
            if (!snap.items[ag].installed || !snap.items[ag].update_available) continue;
            only_ag = ag;
            cui_push_id_int(ui, (int)ag);
            mcode_ui_draw_provider_icon(app, ui, ag, 11.5f, text_col);
            cui_pop_id(ui);
        }

        char lbl[48];
        if (vw < 780.0f) {
            snprintf(lbl, sizeof lbl, "Update");
        } else if (snap.available_count == 1) {
            snprintf(lbl, sizeof lbl, "Update %s", mcode_agent_short_label(only_ag));
        } else {
            snprintf(lbl, sizeof lbl, "Update (%d)", snap.available_count);
        }
        cui_text(ui, lbl, .font = CUI_FONT_MEDIUM, .size = text_sz,
                 .color = text_col, .tabular = true, .nowrap = true);
    } else {
        cui_icon(ui, CUI_ICON_CHECK, 11.5f, cui_color_of(ui, CUI_COLOR_SUCCESS));
        cui_text(ui, "Up to date", .size = text_sz, .color = sec_col, .nowrap = true);
    }

    cui_card_end(ui);

    if (!open && snap.available_count > 0) {
        char tip[256] = "";
        int pos = snprintf(tip, sizeof tip, "CLI update%s available: ",
                           snap.available_count == 1 ? "" : "s");
        int added = 0;
        for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
            app_agent_kind ag = k_order[oi];
            const mcode_cli_update_info *info = &snap.items[ag];
            if (!info->installed || !info->update_available) continue;
            if ((size_t)pos < sizeof tip) {
                pos += snprintf(tip + pos, sizeof tip - (size_t)pos, "%s%s %s → %s",
                                added > 0 ? ", " : "",
                                mcode_agent_short_label(ag),
                                info->installed_version, info->latest_version);
            }
            ++added;
        }
        cui_tooltip(ui, tip);
    }

    if (clicked) {
        cui_popover_toggle(ui, "mcode-cli-updates-pop");
    }
    if (app->cli_updates_open_requested) {
        app->cli_updates_open_requested = false;
        cui_popover_open(ui, "mcode-cli-updates-pop");
    }

    bool close_pop = false;
    cui_popover(ui, "mcode-cli-updates-pop", .placement = CUI_PLACE_TOP_END,
                .width = 412, .pad = 12, .gap = 8) {
        draw_cli_updates_popover_body(app, ui, &snap, &close_pop);
    }
    if (close_pop) {
        cui_popover_close(ui, "mcode-cli-updates-pop");
    }
}
