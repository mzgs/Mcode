#ifndef MCODE_UI_INTERNAL_H
#define MCODE_UI_INTERNAL_H

#include "mcode_ui.h"

void mcode_ui_mark_dirty(mcode_app *app);
void mcode_ui_select_thread(mcode_app *app, const char *thread_id);
void mcode_ui_refresh_thread_diff(mcode_thread *thread);
void mcode_ui_load_selected_diff(mcode_thread *thread);
void mcode_ui_refresh_workspace_git(mcode_app *app);
void mcode_ui_load_workspace_diff(mcode_app *app);
void mcode_ui_refresh_file_tree(mcode_app *app);
void mcode_ui_open_diff_file(mcode_app *app, int file_idx);
void mcode_ui_open_file_preview(mcode_app *app, cui_ctx *ui, const char *root_path,
                                const char *rel_path);
void mcode_ui_close_inspector(mcode_app *app);

/* Project lifecycle & dialogs */
void mcode_ui_promote_project(mcode_app *app, const char *project_id);
void mcode_ui_open_project_dialog(mcode_app *app, mcode_project_dialog_mode mode);
void mcode_ui_refresh_discovered_repos(mcode_app *app);
mcode_project *mcode_ui_add_project_and_open(mcode_app *app, cui_ctx *ui,
                                             const char *path, const char *name);
void mcode_ui_remove_project(mcode_app *app, cui_ctx *ui, const char *project_id);
void mcode_ui_init_project_git(mcode_app *app, cui_ctx *ui, mcode_project *proj);
void mcode_ui_open_project_actions_for(mcode_app *app, cui_ctx *ui, const char *project_id);
void mcode_ui_open_rename_project(mcode_app *app, const mcode_project *proj);
bool mcode_ui_start_clone(mcode_app *app, cui_ctx *ui, const char *url,
                          const char *parent_dir, const char *folder_name);
void mcode_ui_cancel_clone(mcode_app *app);

/* Thread lifecycle */
mcode_thread *mcode_ui_create_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool auto_approve);
mcode_thread *mcode_ui_quick_new_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj);
void mcode_ui_open_new_thread_dialog(mcode_app *app, const mcode_project *proj);

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
void mcode_ui_draw_workspace_main(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_diff(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_terminal(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_dialogs(mcode_app *app, cui_ctx *ui);

#endif
