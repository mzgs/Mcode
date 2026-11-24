#ifndef MCODE_TYPES_H
#define MCODE_TYPES_H

#include <cui/cui.h>
#include "settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MCODE_ID_MAX = 64,
    MCODE_NAME_MAX = 160,
    MCODE_PATH_MAX = 1024,
    MCODE_BRANCH_MAX = 128,
    MCODE_CMD_MAX = 512,
    MCODE_SESSION_MAX = 192,
    MCODE_MAX_QUICK_ACTIONS = 16,
    MCODE_COMPOSER_MAX = 8192,
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} mcode_str;

void mcode_str_init(mcode_str *s);
void mcode_str_free(mcode_str *s);
void mcode_str_clear(mcode_str *s);
bool mcode_str_append(mcode_str *s, const char *data, size_t n);
bool mcode_str_append_cstr(mcode_str *s, const char *cstr);
bool mcode_str_set(mcode_str *s, const char *cstr);
const char *mcode_str_cstr(const mcode_str *s);

typedef struct {
    char name[64];
    char command[MCODE_CMD_MAX];
} mcode_quick_action;

typedef struct {
    char id[MCODE_ID_MAX];
    char name[MCODE_NAME_MAX];
    char path[MCODE_PATH_MAX];
    char default_branch[MCODE_BRANCH_MAX];
    char remote_url[512];
    bool is_git_repo;
    bool collapsed;
    mcode_quick_action actions[MCODE_MAX_QUICK_ACTIONS];
    int action_count;
} mcode_project;

typedef enum {
    MCODE_BLOCK_TEXT = 0,
    MCODE_BLOCK_REASONING,
    MCODE_BLOCK_TOOL,
    MCODE_BLOCK_ERROR,
} mcode_block_kind;

typedef struct {
    mcode_block_kind kind;
    mcode_str text;

    /* Reasoning fields */
    bool thinking;

    /* Tool call fields */
    char tool_id[128];
    char tool_name[96];
    char tool_detail[256];
    mcode_str input;
    mcode_str output;
    cui_task_status status;

    double started_mono;
    float duration_sec;
} mcode_block;

typedef struct {
    mcode_str prompt;
    int64_t created_at;
    double started_mono;
    float duration_sec;
    bool running;
    bool failed;
    int vote;
    int input_tokens;
    int output_tokens;
    double cost_usd;

    mcode_block *blocks;
    int block_count;
    int block_cap;
} mcode_turn;

typedef struct {
    char path[512];
    char status[4];
    int added;
    int removed;
    bool untracked;
} mcode_diff_file;

typedef enum {
    MCODE_THREAD_ACTIVE = 0,
    MCODE_THREAD_SNOOZED,
    MCODE_THREAD_SETTLED,
} mcode_thread_state;

typedef enum {
    MCODE_SPEED_DEFAULT = 0,
    MCODE_SPEED_STANDARD,
    MCODE_SPEED_FAST,
} mcode_speed_mode;

typedef enum {
    MCODE_PERM_SUPERVISED = 0,
    MCODE_PERM_ACCEPT_EDITS,
    MCODE_PERM_FULL_ACCESS,
} mcode_permission_mode;

typedef enum {
    MCODE_APPROVAL_NONE = 0,
    MCODE_APPROVAL_ALLOW_ONCE,
    MCODE_APPROVAL_ALLOW_SESSION,
    MCODE_APPROVAL_DENY,
} mcode_approval_decision;

typedef struct {
    bool active;
    char title[160];
    char tool_name[96];
    char command[512];
    char reason[512];
    char details[2048];
    bool can_allow_session;
} mcode_pending_approval;

struct mcode_agent_run;
struct mcode_title_run;

typedef struct {
    char id[MCODE_ID_MAX];
    char project_id[MCODE_ID_MAX];
    char title[MCODE_NAME_MAX];
    app_agent_kind agent;
    char model[APP_MODEL_NAME_MAX];
    char reasoning_effort[32];
    mcode_speed_mode speed_mode;
    mcode_permission_mode perm_mode;
    bool auto_approve;
    char workdir[MCODE_PATH_MAX];
    char branch[MCODE_BRANCH_MAX];
    char session_id[MCODE_SESSION_MAX];
    mcode_thread_state state;
    int64_t created_at;
    int64_t updated_at;

    mcode_turn *turns;
    int turn_count;
    int turn_cap;

    /* Live runtime state (not persisted) */
    bool is_draft;
    struct mcode_agent_run *active_run;
    struct mcode_title_run *title_run;
    bool title_pending;
    char title_auto_fallback[MCODE_NAME_MAX];
    char status_text[160];
    double run_started_mono;
    mcode_pending_approval pending_approval;

    mcode_diff_file *diff_files;
    int diff_file_count;
    int total_added;
    int total_removed;
    int selected_diff_file;
    char *diff_old_text;
    char *diff_new_text;
    char diff_loaded_path[512];
    int diff_change_idx;

    char composer_draft[MCODE_COMPOSER_MAX];
    int scroll_to_bottom_frames;
} mcode_thread;

/* Monotonic clock in seconds. */
double mcode_now_mono(void);
/* Current Unix epoch timestamp in seconds. */
int64_t mcode_now_epoch(void);
/* Generate a short unique hex ID. */
void mcode_gen_id(char *out, size_t cap, const char *prefix);

/* Helper constructors / destructors for turns and blocks. */
mcode_turn *mcode_thread_add_turn(mcode_thread *thread, const char *prompt);
mcode_block *mcode_turn_add_block(mcode_turn *turn, mcode_block_kind kind);
mcode_block *mcode_turn_find_tool(mcode_turn *turn, const char *tool_id);
void mcode_turn_free(mcode_turn *turn);
void mcode_thread_free_runtime(mcode_thread *thread);
void mcode_thread_free(mcode_thread *thread);

#endif
