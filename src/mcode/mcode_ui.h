#ifndef MCODE_UI_H
#define MCODE_UI_H

#include "app_chrome.h"
#include "mcode_agent.h"
#include "mcode_cli_updates.h"
#include "mcode_files.h"
#include "mcode_git.h"
#include "mcode_proc.h"
#include "mcode_store.h"
#include <cui/cui_vterm.h>
#include <pthread.h>
#include <stdbool.h>

enum {
    MCODE_MAX_DISCOVERED_REPOS = 32,
    MCODE_NAV_HISTORY_MAX = 64,
};

typedef enum {
    MCODE_PROJ_DIALOG_OPEN = 0,
    MCODE_PROJ_DIALOG_CREATE,
    MCODE_PROJ_DIALOG_CLONE,
} mcode_project_dialog_mode;

typedef enum {
    MCODE_INSPECTOR_CHAT = 0,
    MCODE_INSPECTOR_DIFF,
    MCODE_INSPECTOR_FILE,
} mcode_inspector_mode;

typedef enum {
    MCODE_FILE_CTX_NONE = 0,
    MCODE_FILE_CTX_TREE,
    MCODE_FILE_CTX_CHANGE,
    MCODE_FILE_CTX_FILES_HDR,
} mcode_file_ctx_kind;

typedef struct {
    app_chrome *chrome;
    mcode_store store;
    char state_path[MCODE_PATH_MAX];
    bool state_dirty;
    double state_dirty_since;

    mcode_cli_status cli_status;
    double cli_checked_at;
    mcode_cli_updates cli_updates;
    double cli_updates_checked_at;
    double cli_updates_local_recheck_at;
    bool cli_updates_open_requested;
    bool cli_updates_manual_check_pending;
    mcode_model_catalog models;
    mcode_usage_tracker usage;
    double usage_checked_at;
    int usage_tab;   /* 0 = Limits, 1 = Activity */
    int usage_scope; /* 0 = Today, 1 = 7d, 2 = All */
    bool usage_resets_expanded;
    bool usage_open_requested;

    /* Model picker popover state */
    char model_search[128];

    /* Cached real provider icon textures (Claude, Codex, Antigravity) */
    cui_texture provider_icons[APP_AGENT_COUNT];
    bool provider_icons_loaded;

    /* Sidebar search & filter */
    char thread_search[128];

    /* Thread navigation history (top bar Back / Forward) */
    char nav_history[MCODE_NAV_HISTORY_MAX][MCODE_ID_MAX];
    int nav_count;
    int nav_index;

    /* Integrated titlebar double-click tracking */
    double titlebar_last_click_at;

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
    bool new_thread_auto_approve;

    /* Commit / PR dialog state */
    bool git_dialog_open;
    bool git_dialog_is_pr;
    char commit_msg[512];
    char pr_title[160];
    char pr_body[2048];

    /* Right sidebar (Source Control + Project Files Tree) & Center Inspector */
    mcode_file_tree file_tree;
    char git_workdir[MCODE_PATH_MAX];
    mcode_diff_file *git_files;
    int git_file_count;
    int git_total_added;
    int git_total_removed;
    int git_selected_idx;
    char *git_diff_old_text;
    char *git_diff_new_text;
    char git_diff_loaded_path[512];
    bool git_diff_loaded_staged;
    int git_diff_change_idx;
    int git_diff_change_count;
    bool staged_expanded;
    uint64_t last_focus_generation;
    double last_focus_refresh_at;
    char sidebar_commit_msg[512];
    bool inspector_open;
    mcode_inspector_mode inspector_mode;
    bool inspector_showing_chat;
    char preview_rel_path[512];
    char preview_workdir[MCODE_PATH_MAX];
    char *preview_text;
    size_t preview_buf_cap;
    bool preview_is_binary;
    bool preview_truncated;
    int64_t preview_file_size;

    /* Compact mouse-positioned context menu for file browser & changes list */
    bool file_ctx_open;
    bool file_ctx_opened_this_frame;
    mcode_file_ctx_kind file_ctx_kind;
    float file_ctx_x;
    float file_ctx_y;
    int file_ctx_index;
    bool file_ctx_is_dir;
    bool file_ctx_is_expanded;
    bool file_ctx_is_staged;
    bool file_ctx_is_deleted;
    char file_ctx_workdir[MCODE_PATH_MAX];
    char file_ctx_rel_path[512];

    /* Project Quick Actions dialog */
    bool actions_dialog_open;
    char actions_project_id[MCODE_ID_MAX];
    char qa_new_name[64];
    char qa_new_cmd[MCODE_CMD_MAX];

    /* Integrated terminal (Better-C-UI cui::vterm) */
    cui_vterm_session *term;
    char term_cwd[MCODE_PATH_MAX];
    bool composer_want_focus;
    bool term_want_focus;
    bool term_search_open;
    bool term_search_want_focus;
    char term_search_buf[128];
    char term_search_applied[128];
} mcode_app;

void mcode_app_init(mcode_app *app, app_chrome *chrome);
void mcode_app_shutdown(mcode_app *app);
void mcode_app_frame(mcode_app *app, cui_ctx *ui);

/* Dynamic Command Palette provider */
void mcode_app_palette_provider(app_palette_builder *builder, void *user);

/* Top-level menu / shortcut actions */
void mcode_app_action_add_project(cui_ctx *ui, void *user);
void mcode_app_action_new_thread(cui_ctx *ui, void *user);
void mcode_app_action_toggle_sidebar(cui_ctx *ui, void *user);
void mcode_app_action_toggle_diff(cui_ctx *ui, void *user);
void mcode_app_action_toggle_terminal(cui_ctx *ui, void *user);
void mcode_app_action_commit(cui_ctx *ui, void *user);
void mcode_app_action_create_pr(cui_ctx *ui, void *user);
void mcode_app_action_refresh_diff(cui_ctx *ui, void *user);
void mcode_app_action_project_actions(cui_ctx *ui, void *user);
void mcode_app_action_open_folder(cui_ctx *ui, void *user);

#endif
