#include "mcode_ui_internal.h"
#include <stdio.h>

void mcode_ui_draw_terminal(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
    if (!proj && app->store.project_count > 0) proj = &app->store.projects[0];
    const char *cwd = (thread && thread->workdir[0]) ? thread->workdir
                    : (proj ? proj->path : NULL);
    bool busy = (app->term_proc != NULL);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        /* Quick actions & terminal toolbar */
        cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 6, .gap = 6,
                .align = CUI_ALIGN_CENTER, .wrap = true) {
            cui_icon(ui, CUI_ICON_SQUARE_TERMINAL, 14, CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, "Terminal", .font = CUI_FONT_MEDIUM, .size = 12.5f);
            if (busy) {
                cui_spinner(ui, 13);
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
                if (cui_icon_button(ui, CUI_ICON_PLUS, .tooltip = "Edit Quick Actions & Setup Script")) {
                    mcode_app_action_project_actions(ui, app);
                }
            }
            if (busy) {
                if (cui_button(ui, "Stop", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_SQUARE)) {
                    mcode_ui_stop_terminal(app);
                }
            }
            if (cui_icon_button(ui, CUI_ICON_TRASH_2, .tooltip = "Clear Terminal")) {
                cui_terminal_clear(&app->term_buf);
            }
            if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Close Terminal Drawer")) {
                app->store.show_terminal_panel = false;
                mcode_ui_mark_dirty(app);
            }
        }
        cui_separator(ui);

        /* ANSI terminal widget */
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 6) {
            cui_terminal_result tr = cui_terminal(ui, "mcode-term", &app->term_buf,
                                                  .height = -1, .size = 12.5f, .prompt = "$ ");
            if (tr.command && tr.command[0]) {
                mcode_ui_run_terminal_cmd(app, ui, tr.command, cwd);
            }
            if (tr.interrupt && busy) {
                mcode_ui_stop_terminal(app);
            }
        }
    }
}
