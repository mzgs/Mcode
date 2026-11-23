#ifndef MCODE_UI_H
#define MCODE_UI_H

#include "app_chrome.h"
#include "mcode_agent.h"
#include "mcode_git.h"
#include "mcode_proc.h"
#include "mcode_store.h"
#include <pthread.h>
#include <stdbool.h>

enum {
    MCODE_MAX_DISCOVERED_REPOS = 32,
};

typedef enum {
    MCODE_PROJ_DIALOG_OPEN = 0,
    MCODE_PROJ_DIALOG_CREATE,
    MCODE_PROJ_DIALOG_CLONE,
} mcode_project_dialog_mode;

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

    /* Add / Create / Clone Project dialog state */
    bool project_dialog_open;
    int project_dialog_mode;
    char new_proj_name[MCODE_NAME_MAX];
    char new_proj_parent[MCODE_PATH_MAX];
    bool new_proj_init_git;
    char clone_url[512];
    char clone_folder[MCODE_NAME_MAX];
    bool clone_folder_edited;
    char project_dialog_err[256];

    /* Async git clone state */
    mcode_proc *clone_proc;
    pthread_mutex_t clone_mu;
    char clone_dest[MCODE_PATH_MAX];
    char clone_status[256];
    char clone_last_error[256];
    bool clone_finished;
    bool clone_killed;
    int clone_exit_code;

    /* Discovered local Git repositories */
    mcode_discovered_repo discovered_repos[MCODE_MAX_DISCOVERED_REPOS];
    int discovered_repo_count;
    char discovered_filter[96];

    /* Rename Project dialog state */
    bool rename_project_open;
    char rename_project_id[MCODE_ID_MAX];
    char rename_project_buf[MCODE_NAME_MAX];

    /* New Thread (Custom Options) dialog state */
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
    char actions_project_id[MCODE_ID_MAX];
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
