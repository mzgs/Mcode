#ifndef MCODE_UI_INTERNAL_H
#define MCODE_UI_INTERNAL_H

#include "mcode_ui.h"

/* Shared recessed chrome background for sidebars, status bar, and toolbars. */
static inline cui_color mcode_ui_chrome_bg(cui_ctx *ui) {
    return app_theme_current(ui)->chrome_bg;
}

/* Shared input/control surface fill sitting on recessed chrome rails. */
static inline cui_color mcode_ui_chrome_input_bg(cui_ctx *ui) {
    return app_theme_current(ui)->chrome_input_bg;
}

void mcode_ui_mark_dirty(mcode_app *app);
void mcode_ui_focus_composer(mcode_app *app, cui_ctx *ui);
void mcode_ui_select_thread(mcode_app *app, const char *thread_id);
void mcode_ui_refresh_thread_diff(mcode_thread *thread);
void mcode_ui_load_selected_diff(mcode_thread *thread);
void mcode_ui_refresh_workspace_git(mcode_app *app);
void mcode_ui_load_workspace_diff(mcode_app *app);
bool mcode_ui_start_git_op(mcode_app *app, cui_ctx *ui, mcode_git_op_kind kind,
                           const char *workdir, bool commit_followup);
bool mcode_ui_is_git_op_running(const mcode_app *app, const char *workdir);
void mcode_ui_open_discard_all_dialog(mcode_app *app, const char *workdir);
void mcode_ui_perform_discard_all(mcode_app *app, cui_ctx *ui);
void mcode_ui_refresh_file_tree(mcode_app *app);
void mcode_ui_open_diff_file(mcode_app *app, int file_idx);
void mcode_ui_open_file_preview(mcode_app *app, cui_ctx *ui, const char *root_path,
                                const char *rel_path);
void mcode_ui_close_inspector(mcode_app *app);

/* Commit message auto-generation & shared input with trailing sparkle button */
void mcode_ui_start_commit_msg_gen(mcode_app *app, cui_ctx *ui, const char *workdir,
                                   bool staged_only, mcode_commit_gen_target target);
void mcode_ui_cancel_commit_msg_gen(mcode_app *app);
bool mcode_ui_is_generating_commit_msg(const mcode_app *app, mcode_commit_gen_target target);
int mcode_ui_draw_commit_msg_input(mcode_app *app, cui_ctx *ui, const char *id,
                                   char *buf, size_t cap, const char *placeholder,
                                   cui_control_size size, float font_size,
                                   cui_color bg, cui_color border_color, float radius,
                                   const char *workdir, bool staged_only, bool has_changes,
                                   mcode_commit_gen_target target);

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
void mcode_ui_queue_message(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt);
void mcode_ui_steer_message(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt);
void mcode_ui_steer_queued_item(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int index);
void mcode_ui_send_next_queued(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_cancel_pending_steer(mcode_app *app, mcode_thread *thread);
void mcode_ui_stop_thread(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_delete_thread(mcode_app *app, cui_ctx *ui, const char *thread_id);

bool mcode_ui_ensure_terminal(mcode_app *app, const char *cwd);
void mcode_ui_run_terminal_cmd(mcode_app *app, cui_ctx *ui, const char *cmd, const char *cwd);
void mcode_ui_stop_terminal(mcode_app *app);

void mcode_ui_format_elapsed(float seconds, char *out, size_t cap);
void mcode_ui_draw_project_badge(cui_ctx *ui, const char *name, float size);
void mcode_ui_draw_provider_icon(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                 float size, cui_color tint);

/* Shared local & remote image resolution, LRU texture cache, and inline preview cards */
void mcode_ui_images_shutdown(mcode_app *app);
bool mcode_ui_images_poll(mcode_app *app, bool *out_loading);
bool mcode_image_is_http_url(const char *s);
bool mcode_image_has_supported_ext(const char *path);
bool mcode_ui_resolve_local_path(const char *raw_link, const char *workdir,
                                 bool strip_line_suffix,
                                 char *out_path, size_t out_cap);
bool mcode_ui_resolve_remote_image(mcode_app *app, const char *url,
                                   char *out_path, size_t out_cap, bool *out_loading);
const mcode_image_cache_entry *mcode_ui_image_get(mcode_app *app, cui_ctx *ui,
                                                  const char *abs_path);
void mcode_ui_open_image_lightbox(mcode_app *app, const char *abs_path, const char *title);
bool mcode_ui_draw_image_preview_card(mcode_app *app, cui_ctx *ui, const char *id,
                                      const char *abs_path, const char *caption,
                                      float max_w, float max_h, bool compact);
void mcode_ui_draw_image_loading_card(cui_ctx *ui, const char *caption, float max_w);

/* Panel renderers */
void mcode_ui_draw_sidebar(mcode_app *app, cui_ctx *ui);
void mcode_ui_keep_thread_scrolls_alive(mcode_app *app, cui_ctx *ui);
void mcode_ui_draw_chat(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_workspace_main(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_diff(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_file_context_menu(mcode_app *app, cui_ctx *ui);
void mcode_ui_draw_terminal(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_status_usage(mcode_app *app, cui_ctx *ui, mcode_thread *thread);
void mcode_ui_draw_status_cli_updates(mcode_app *app, cui_ctx *ui);
void mcode_ui_draw_composer_usage_ring(mcode_app *app, cui_ctx *ui, const mcode_thread *thread,
                                       cui_placement hover_placement);
void mcode_ui_draw_dialogs(mcode_app *app, cui_ctx *ui);

#endif
