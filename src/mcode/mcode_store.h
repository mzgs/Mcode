#ifndef MCODE_STORE_H
#define MCODE_STORE_H

#include "mcode_types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    mcode_project *projects;
    int project_count;
    int project_cap;

    mcode_thread *threads;
    int thread_count;
    int thread_cap;

    char selected_project_id[MCODE_ID_MAX]; /* "" = All Projects */
    char selected_thread_id[MCODE_ID_MAX];
    char default_project_dir[MCODE_PATH_MAX];

    /* Persisted UI layout state */
    float sidebar_ratio;
    float diff_ratio;
    float terminal_ratio;
    bool show_sidebar;
    bool show_diff_panel;
    bool show_terminal_panel;
    bool diff_inline_mode;
    bool settled_expanded;
} mcode_store;

void mcode_store_init(mcode_store *store);
void mcode_store_free(mcode_store *store);

/* Project & thread lookup / mutations */
mcode_project *mcode_store_find_project(mcode_store *store, const char *id);
mcode_project *mcode_store_find_project_by_path(mcode_store *store, const char *path);
mcode_project *mcode_store_add_project(mcode_store *store, const char *path, const char *name);
bool mcode_store_remove_project(mcode_store *store, const char *id);

mcode_thread *mcode_store_find_thread(mcode_store *store, const char *id);
mcode_thread *mcode_store_selected_thread(mcode_store *store);
mcode_thread *mcode_store_add_thread(mcode_store *store, const mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool use_worktree, bool auto_approve,
                                     const char *workdir, const char *branch);
bool mcode_store_remove_thread(mcode_store *store, const char *id);

/* Atomic JSON persistence to <settings_dir>/mcode-state.json (or explicit path). */
bool mcode_store_load(mcode_store *store, const char *path);
bool mcode_store_save(const mcode_store *store, const char *path);

#endif
