#ifndef MCODE_UI_H
#define MCODE_UI_H

#include "app_chrome.h"
#include "mcode_agent.h"
#include "mcode_proc.h"
#include "mcode_store.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    app_chrome *chrome;
    mcode_store store;
    char state_path[MCODE_PATH_MAX];
    bool state_dirty;
    double state_dirty_since;

    mcode_cli_status cli_status;
    double cli_checked_at;
    mcode_model_catalog models;

    /* Model picker popover state */
    char model_search[128];

    /* Cached real provider icon textures (Claude, Codex, Antigravity) */
    cui_texture provider_icons[APP_AGENT_COUNT];
    bool provider_icons_loaded;

    /* Sidebar search & filter */
    char thread_search[128];

    /* New Thread dialog state */
    bool new_thread_open;
    int new_thread_project_idx;
    int new_thread_agent;
    char new_thread_title[MCODE_NAME_MAX];
    char new_thread_model[APP_MODEL_NAME_MAX];
    bool new_thread_custom_model;
    bool new_thread_worktree;
    bool new_thread_auto_approve;

    /* Commit / PR dialog state */
    bool git_dialog_open;
    bool git_dialog_is_pr;
    char commit_msg[512];
    char pr_title[160];
    char pr_body[2048];

    /* Project Quick Actions & Worktree Setup dialog */
    bool actions_dialog_open;
    char qa_new_name[64];
    char qa_new_cmd[MCODE_CMD_MAX];

    /* Integrated terminal */
    char *term_text;
    cui_terminal_line *term_lines;
    cui_terminal_buffer term_buf;
    mcode_proc *term_proc;
    pthread_mutex_t term_mu;
    mcode_str term_pending;
    bool term_exited;
    int term_exit_code;
} mcode_app;

void mcode_app_init(mcode_app *app, app_chrome *chrome);
void mcode_app_shutdown(mcode_app *app);
void mcode_app_frame(mcode_app *app, cui_ctx *ui);

/* Dynamic Command Palette provider */
void mcode_app_palette_provider(app_palette_builder *builder, void *user);

/* Top-level menu / shortcut actions */
void mcode_app_action_add_project(cui_ctx *ui, void *user);
void mcode_app_action_new_thread(cui_ctx *ui, void *user);
void mcode_app_action_toggle_diff(cui_ctx *ui, void *user);
void mcode_app_action_toggle_terminal(cui_ctx *ui, void *user);
void mcode_app_action_commit(cui_ctx *ui, void *user);
void mcode_app_action_create_pr(cui_ctx *ui, void *user);
void mcode_app_action_refresh_diff(cui_ctx *ui, void *user);
void mcode_app_action_project_actions(cui_ctx *ui, void *user);
void mcode_app_action_open_folder(cui_ctx *ui, void *user);

#endif
