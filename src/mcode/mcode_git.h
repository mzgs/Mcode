#ifndef MCODE_GIT_H
#define MCODE_GIT_H

#include "mcode_types.h"
#include <stdbool.h>
#include <stddef.h>

/* Inspect a directory: checks whether it is inside a Git repo, finds its current branch and origin URL. */
bool mcode_git_inspect_repo(const char *dir, bool *is_repo,
                            char *branch_out, size_t branch_cap,
                            char *remote_out, size_t remote_cap);

/* Initialize a new Git repository in `dir` (`git init`). */
bool mcode_git_init_repo(const char *dir, char *err, size_t err_cap);

/* Resolve a sensible default parent folder for new/cloned projects (e.g. ~/Developer). */
void mcode_git_default_projects_dir(char *out, size_t cap);

/* Create a new project folder `<parent_dir>/<folder_name>` and optionally run `git init`. */
bool mcode_git_create_project_dir(const char *parent_dir, const char *folder_name,
                                  bool init_git, char *out_path, size_t out_cap,
                                  char *err, size_t err_cap);

/* Extract a default folder name from a Git remote URL (https or ssh). */
void mcode_git_repo_name_from_url(const char *url, char *out, size_t out_cap);

/* Format a path with a leading `~` when inside the user's home directory. */
void mcode_git_shorten_home_path(const char *path, char *out, size_t out_cap);

typedef struct {
    char name[MCODE_NAME_MAX];
    char path[MCODE_PATH_MAX];
    int64_t mtime;
} mcode_discovered_repo;

/* Discover local Git repositories (1 level deep) in common workspace folders + optional `extra_dir`. */
int mcode_git_discover_repos(const char *extra_dir, mcode_discovered_repo *out, int max_count);

/* Refresh changed files (`git status --porcelain` + `git diff --numstat`) for a working directory.
 * Caller owns *out_files (freed with free()). */
bool mcode_git_list_changes(const char *workdir,
                            mcode_diff_file **out_files, int *out_count,
                            int *out_total_added, int *out_total_removed);

/* Load before (HEAD) and after (working tree) UTF-8 contents of a file for cui_diff_view.
 * Caller frees *out_old and *out_new. */
bool mcode_git_load_file_diff(const char *workdir, const mcode_diff_file *file,
                              char **out_old, char **out_new);

/* Detect a cui_diff_view / cui_code_block language ID from a file path extension. */
const char *mcode_lang_from_path(const char *path);

/* Stage all changes and commit with `message`. */
bool mcode_git_commit_all(const char *workdir, const char *message,
                          char *err, size_t err_cap);

/* Commit currently staged changes only (`git commit -m <message>`). */
bool mcode_git_commit_staged(const char *workdir, const char *message,
                             char *err, size_t err_cap);

/* Collect a bounded summary of pending changes (recent commits, changed files,
 * diff stats, patch hunks, and untracked file snippets) for commit message
 * generation. If `staged_only` is true, inspects staged changes only.
 * Caller owns *out_context (freed with free()). */
bool mcode_git_collect_commit_context(const char *workdir, bool staged_only,
                                      size_t max_bytes, char **out_context);

/* Stage / unstage individual files or all files. */
bool mcode_git_stage_file(const char *workdir, const char *rel_path,
                          char *err, size_t err_cap);
bool mcode_git_unstage_file(const char *workdir, const char *rel_path,
                            char *err, size_t err_cap);
bool mcode_git_stage_all(const char *workdir, char *err, size_t err_cap);
bool mcode_git_unstage_all(const char *workdir, char *err, size_t err_cap);

/* Discard all unstaged working tree changes and remove untracked (non-ignored) files.
 * Preserves staged index changes and gitignored files. */
bool mcode_git_discard_all(const char *workdir, char *err, size_t err_cap);

/* Push current branch to upstream (or `git push -u <remote> HEAD` if no upstream). */
bool mcode_git_push(const char *workdir, char *err, size_t err_cap);

typedef struct {
    bool valid;
    bool is_unborn;      /* true when repository has no commits yet */
    bool is_detached;    /* true when HEAD is detached */
    bool has_remote;     /* true when at least one remote is configured */
    bool has_upstream;   /* true when current branch tracks a live upstream */
    bool upstream_gone;  /* true when configured upstream ref is gone */
    char branch[MCODE_BRANCH_MAX];
    char upstream[MCODE_BRANCH_MAX];
    char remote[64];     /* preferred remote name ("origin" or first configured remote) */
    int ahead;
    int behind;
} mcode_git_sync_status;

/* Inspect local branch / upstream / ahead / behind / remote status (fast, no network). */
bool mcode_git_get_sync_status(const char *workdir, mcode_git_sync_status *out);

typedef enum {
    MCODE_GIT_OP_NONE = 0,
    MCODE_GIT_OP_SYNC,      /* pull then push (if ahead) */
    MCODE_GIT_OP_PUBLISH,   /* git push -u <remote> HEAD */
    MCODE_GIT_OP_PUSH,      /* git push (or -u <remote> HEAD without upstream) */
    MCODE_GIT_OP_PULL,      /* git pull --no-edit */
    MCODE_GIT_OP_FETCH,     /* git fetch */
} mcode_git_op_kind;

typedef struct mcode_git_op mcode_git_op;

/* Start an asynchronous Git remote operation (sync/publish/push/pull/fetch). */
mcode_git_op *mcode_git_op_start(mcode_git_op_kind kind, const char *workdir,
                                 const char *remote, bool has_upstream);

/* Poll completion. Returns true once finished and writes result + status/error text. */
bool mcode_git_op_poll(mcode_git_op *op, bool *ok_out, char *msg_out, size_t msg_cap);

/* Request cancellation / immediate termination (safe during shutdown). */
void mcode_git_op_stop(mcode_git_op *op);
void mcode_git_op_kill(mcode_git_op *op);

/* Join worker thread and free resources. */
void mcode_git_op_destroy(mcode_git_op *op);
void mcode_git_op_destroy_timeout(mcode_git_op *op, int grace_ms);

mcode_git_op_kind mcode_git_op_kind_of(const mcode_git_op *op);
const char *mcode_git_op_workdir(const mcode_git_op *op);

/* Create a PR using `gh pr create` if available, else build a GitHub/GitLab compare URL.
 * If opened via gh, writes PR URL to url_out; if fallback, writes compare URL to url_out and sets *used_gh = false. */
bool mcode_git_create_pr(const char *workdir, const char *title, const char *body,
                         char *url_out, size_t url_cap, bool *used_gh,
                         char *err, size_t err_cap);

/* Convert a git remote URL (ssh or https) + branch into a web compare URL (GitHub/GitLab). */
bool mcode_git_build_pr_url(const char *remote_url, const char *base_branch,
                            const char *head_branch, const char *title,
                            char *url_out, size_t url_cap);

#endif
