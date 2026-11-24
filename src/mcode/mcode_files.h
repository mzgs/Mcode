#ifndef MCODE_FILES_H
#define MCODE_FILES_H

#include "mcode_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[256];
    char rel_path[512];
    int parent_idx;
    int first_child;
    int next_sibling;
    int depth;
    bool is_dir;
    bool expanded;
    bool children_loaded;
    bool git_ignored;
    bool has_changed_descendant;
    char git_status; /* 0, 'M', 'A', 'D', 'R', 'U' */
} mcode_fs_node;

typedef struct {
    char root_path[MCODE_PATH_MAX];
    bool is_git_repo;

    mcode_fs_node *nodes;
    int count;
    int cap;
    int first_root;

    /* Git-ignored relative paths (trailing '/' stripped) from `git status --ignored=matching`. */
    char (*ignored_paths)[512];
    int ignored_count;
    int ignored_cap;

    /* Cached latest git diff files snapshot for re-applying status when lazy-expanding folders. */
    mcode_diff_file *git_files;
    int git_file_count;
} mcode_file_tree;

void mcode_file_tree_init(mcode_file_tree *tree);
void mcode_file_tree_free(mcode_file_tree *tree);
void mcode_file_tree_clear(mcode_file_tree *tree);

/* Load or refresh the tree rooted at `root_path`. Preserves expanded directories
 * when refreshing the same root_path. */
void mcode_file_tree_refresh(mcode_file_tree *tree, const char *root_path, bool is_git_repo,
                             const mcode_diff_file *git_files, int git_file_count);

/* Re-apply git status decorations onto loaded nodes (and remember snapshot for future expands). */
void mcode_file_tree_apply_git_status(mcode_file_tree *tree,
                                      const mcode_diff_file *git_files, int git_file_count);

/* Toggle expand/collapse on a directory node (lazy-loads children on first expand). */
bool mcode_file_tree_toggle(mcode_file_tree *tree, int node_idx);

/* Collapse all expanded directories in the tree. */
void mcode_file_tree_collapse_all(mcode_file_tree *tree);

/* Read a project file for preview/viewing (capped at max_bytes, default 512KB).
 * Caller owns *out_buf (freed with free()). *out_cap receives allocated buffer size (>= len + 1). */
bool mcode_file_read_preview(const char *root_path, const char *rel_path,
                             char **out_buf, size_t *out_cap,
                             bool *out_is_binary, bool *out_truncated,
                             int64_t *out_file_size);

#endif
