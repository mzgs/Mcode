#include "mcode_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cui_vterm_desc make_term_desc(const char *cwd) {
    return (cui_vterm_desc){
        .cwd = (cwd && cwd[0]) ? cwd : NULL,
        .scrollback_lines = 5000,
    };
}

bool mcode_ui_ensure_terminal(mcode_app *app, const char *cwd) {
    if (!app) return false;
    if (app->term) return true;
    cui_vterm_desc desc = make_term_desc(cwd);
    app->term = cui_vterm_create(&desc);
    if (!app->term) return false;
    snprintf(app->term_cwd, sizeof app->term_cwd, "%s", (cwd && cwd[0]) ? cwd : "");
    return true;
}

static void restart_terminal(mcode_app *app, const char *cwd) {
    if (!app) return;
    app->term_search_open = false;
    app->term_search_buf[0] = '\0';
    app->term_search_applied[0] = '\0';
    cui_vterm_desc desc = make_term_desc(cwd);
    if (app->term) {
        cui_vterm_restart(app->term, &desc);
    } else {
        app->term = cui_vterm_create(&desc);
    }
    snprintf(app->term_cwd, sizeof app->term_cwd, "%s", (cwd && cwd[0]) ? cwd : "");
    app->term_want_focus = true;
}

static void close_term_search(mcode_app *app) {
    app->term_search_open = false;
    app->term_search_applied[0] = '\0';
    if (app->term) cui_vterm_search_clear(app->term);
    app->term_want_focus = true;
}

static bool on_term_key(void *user, cui_key key, int mods) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return false;
    if ((mods & CUI_MOD_PRIMARY) && !(mods & (CUI_MOD_ALT | CUI_MOD_SHIFT)) &&
        key == CUI_KEY_F) {
        app->term_search_open = true;
        app->term_search_want_focus = true;
        return true;
    }
    if (app->term_search_open && key == CUI_KEY_ESCAPE) {
        close_term_search(app);
        return true;
    }
    return false;
}

void mcode_ui_draw_terminal(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
    if (!proj && app->store.project_count > 0) proj = &app->store.projects[0];
    const char *cwd = (thread && thread->workdir[0]) ? thread->workdir
                    : (proj ? proj->path : NULL);

    mcode_ui_ensure_terminal(app, cwd);

    bool running = app->term && cui_vterm_is_running(app->term);
    bool exited = app->term && cui_vterm_has_exited(app->term);
    int exit_code = app->term ? cui_vterm_exit_code(app->term) : 0;
    const char *osc_title = app->term ? cui_vterm_title(app->term) : NULL;

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        /* Terminal toolbar */
        cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 5, .gap = 6,
                .align = CUI_ALIGN_CENTER, .wrap = true) {
            cui_icon(ui, CUI_ICON_SQUARE_TERMINAL, 14, CUI_COLOR_TEXT_SECONDARY);

            char header[96];
            if (osc_title && osc_title[0]) {
                snprintf(header, sizeof header, "%.60s", osc_title);
            } else {
                snprintf(header, sizeof header, "Terminal");
            }
            cui_text(ui, header, .font = CUI_FONT_MEDIUM, .size = 12.5f);

            if (running) {
                cui_badge(ui, "shell", .tone = CUI_TONE_NEUTRAL);
            } else if (exited) {
                char code_lbl[32];
                snprintf(code_lbl, sizeof code_lbl, "exit %d", exit_code);
                cui_badge(ui, code_lbl,
                          .tone = exit_code == 0 ? CUI_TONE_SUCCESS : CUI_TONE_DANGER);
            }

            if (proj && proj->action_count > 0) {
                cui_separator(ui);
                for (int i = 0; i < proj->action_count; ++i) {
                    cui_push_id_int(ui, i);
                    if (cui_button(ui, proj->actions[i].name, .variant = CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_PLAY,
                                   .tooltip = proj->actions[i].command)) {
                        mcode_ui_run_terminal_cmd(app, ui, proj->actions[i].command, cwd);
                    }
                    cui_pop_id(ui);
                }
            }

            cui_spacer(ui);

            if (proj) {
                if (cui_icon_button(ui, CUI_ICON_PLUS, .tooltip = "Edit Quick Actions")) {
                    mcode_app_action_project_actions(ui, app);
                }
            }
            if (cui_icon_button(ui, CUI_ICON_SEARCH, .tooltip = "Find in Terminal")) {
                if (app->term_search_open) {
                    close_term_search(app);
                } else {
                    app->term_search_open = true;
                    app->term_search_want_focus = true;
                }
            }
            if (running) {
                if (cui_icon_button(ui, CUI_ICON_SQUARE, .tooltip = "Interrupt (Ctrl+C)")) {
                    mcode_ui_stop_terminal(app);
                }
            }
            if (exited) {
                if (cui_button(ui, "Restart", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_ROTATE_CCW)) {
                    restart_terminal(app, cwd);
                }
            } else {
                if (cui_icon_button(ui, CUI_ICON_ROTATE_CCW, .tooltip = "Restart Shell in Active Directory")) {
                    restart_terminal(app, cwd);
                }
            }
            if (cui_icon_button(ui, CUI_ICON_TRASH_2, .tooltip = "Clear Terminal")) {
                if (app->term) cui_vterm_clear(app->term, true);
                app->term_want_focus = true;
            }
            if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Close Terminal Drawer")) {
                app->store.show_terminal_panel = false;
                app->term_search_open = false;
                mcode_ui_mark_dirty(app);
            }
        }

        /* Optional inline find bar */
        if (app->term_search_open && app->term) {
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 4, .gap = 6,
                    .align = CUI_ALIGN_CENTER) {
                int sflags = cui_search(ui, "mcode-term-find",
                                        app->term_search_buf, sizeof app->term_search_buf,
                                        .placeholder = "Find in terminal...",
                                        .width = CUI_PX(240), .size = CUI_SIZE_SM);
                cui_id find_id = cui_last_id(ui);
                if (app->term_search_want_focus) {
                    cui_set_focus(ui, find_id);
                    app->term_search_want_focus = false;
                }
                if (strcmp(app->term_search_buf, app->term_search_applied) != 0) {
                    snprintf(app->term_search_applied, sizeof app->term_search_applied,
                             "%s", app->term_search_buf);
                    cui_vterm_search(app->term, app->term_search_applied, false);
                }
                if (sflags & CUI_SUBMITTED) {
                    if (cui_key_mods(ui) & CUI_MOD_SHIFT) {
                        cui_vterm_search_prev(app->term);
                    } else {
                        cui_vterm_search_next(app->term);
                    }
                }
                if (cui_is_focused(ui, find_id) && cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
                    close_term_search(app);
                }

                int matches = cui_vterm_search_count(app->term);
                int current = cui_vterm_search_current(app->term);
                if (app->term_search_buf[0]) {
                    char mlabel[32];
                    if (matches > 0 && current >= 0) {
                        snprintf(mlabel, sizeof mlabel, "%d / %d", current + 1, matches);
                    } else {
                        snprintf(mlabel, sizeof mlabel, "0 matches");
                    }
                    cui_text(ui, mlabel, .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                }
                if (cui_icon_button(ui, CUI_ICON_CHEVRON_UP, .tooltip = "Previous Match")) {
                    cui_vterm_search_prev(app->term);
                }
                if (cui_icon_button(ui, CUI_ICON_CHEVRON_DOWN, .tooltip = "Next Match")) {
                    cui_vterm_search_next(app->term);
                }
                cui_spacer(ui);
                if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Close Find")) {
                    close_term_search(app);
                }
            }
        }

        cui_separator(ui);

        /* Better-C-UI cui::vterm interactive terminal widget */
        if (app->term) {
            cui_vterm_result tr = cui_vterm(ui, "mcode-term", app->term,
                                            .width = CUI_FILL,
                                            .height = CUI_FILL,
                                            .font_size = 12.5f,
                                            .border = -1,
                                            .radius = -1,
                                            .cursor_style = CUI_VTERM_CURSOR_BAR,
                                            .cursor_blink = true,
                                            .visual_bell = true,
                                            .allow_osc52 = true,
                                            .on_key = on_term_key,
                                            .user = app);
            if (app->term_want_focus) {
                cui_set_focus(ui, cui_last_id(ui));
                app->term_want_focus = false;
            }
            if (tr.clicked_link) {
                cui_app_open_url(tr.clicked_link);
            }
            cui_context_menu(ui, "mcode-term-ctx") {
                if (cui_menu_item(ui, "Copy", .disabled = !tr.has_selection)) {
                    char *sel = cui_vterm_copy_selection(app->term);
                    if (sel) {
                        cui_clipboard_set(ui, sel);
                        free(sel);
                        cui_vterm_clear_selection(app->term);
                    }
                }
                if (cui_menu_item(ui, "Paste")) {
                    const char *clip = cui_clipboard_get(ui);
                    if (clip && clip[0]) cui_vterm_paste(app->term, clip);
                }
                if (cui_menu_item(ui, "Select All")) {
                    cui_vterm_select_all(app->term);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "Find...")) {
                    app->term_search_open = true;
                    app->term_search_want_focus = true;
                }
                if (cui_menu_item(ui, "Clear")) {
                    cui_vterm_clear(app->term, true);
                }
                if (cui_menu_item(ui, "Restart Shell")) {
                    restart_terminal(app, cwd);
                }
            }
        } else {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                cui_text(ui, "Integrated terminal failed to start.", .color = CUI_COLOR_TEXT_SECONDARY);
                if (cui_button(ui, "Retry", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) {
                    restart_terminal(app, cwd);
                }
            }
        }
    }
}
