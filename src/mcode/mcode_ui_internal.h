#ifndef MCODE_UI_INTERNAL_H
#define MCODE_UI_INTERNAL_H

#include "mcode_ui.h"

void mcode_ui_mark_dirty(mcode_app *app);
void mcode_ui_select_thread(mcode_app *app, const char *thread_id);
void mcode_ui_refresh_thread_diff(mcode_thread *thread);
void mcode_ui_load_selected_diff(mcode_thread *thread);

mcode_thread *mcode_ui_create_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool use_worktree, bool auto_approve);

void mcode_ui_send_prompt(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt);
void mcode_ui_stop_thread(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_delete_thread(mcode_app *app, cui_ctx *ui, const char *thread_id);

void mcode_ui_run_terminal_cmd(mcode_app *app, cui_ctx *ui, const char *cmd, const char *cwd);
void mcode_ui_stop_terminal(mcode_app *app);

void mcode_ui_format_elapsed(float seconds, char *out, size_t cap);
void mcode_ui_draw_provider_icon(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                 float size, cui_color tint);

/* Panel renderers */
void mcode_ui_draw_sidebar(mcode_app *app, cui_ctx *ui);
void mcode_ui_draw_chat(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_diff(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_terminal(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_dialogs(mcode_app *app, cui_ctx *ui);

#endif
