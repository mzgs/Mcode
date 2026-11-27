#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_ui_internal.h"
#include "mcode_git.h"
#include "mcode_provider_icons.h"
#include "app_log.h"
#include "platform.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

void mcode_ui_format_elapsed(float seconds, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (seconds < 0.0f) seconds = 0.0f;
    int total = (int)seconds;
    if (total < 60) {
        snprintf(out, cap, "%ds", total);
    } else if (total < 3600) {
        snprintf(out, cap, "%dm %ds", total / 60, total % 60);
    } else {
        snprintf(out, cap, "%dh %dm", total / 3600, (total % 3600) / 60);
    }
}

void mcode_ui_mark_dirty(mcode_app *app) {
    if (!app) return;
    app->state_dirty = true;
    app->state_dirty_since = mcode_now_mono();
}

void mcode_ui_focus_composer(mcode_app *app, cui_ctx *ui) {
    if (!app) return;
    app->composer_want_focus = true;
    app->term_want_focus = false;
    app->term_search_want_focus = false;
    if (ui) cui_request_redraw(ui);
}

void mcode_ui_promote_project(mcode_app *app, const char *project_id) {
    if (!app || !project_id || !project_id[0]) return;
    bool already_top = (app->store.project_count > 0 &&
                        strcmp(app->store.projects[0].id, project_id) == 0);
    if (mcode_store_move_project_to_top(&app->store, project_id) && !already_top) {
        mcode_ui_mark_dirty(app);
    }
}

void mcode_ui_draw_provider_icon(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                 float size, cui_color tint) {
    if (!ui) return;
    if (agent < 0 || agent >= APP_AGENT_COUNT) {
        cui_icon(ui, CUI_ICON_CPU, size, tint);
        return;
    }
    if (app && !app->provider_icons_loaded) {
        app->provider_icons_loaded = true;
        uint8_t rgba[MCODE_PROVIDER_ICON_SIZE * MCODE_PROVIDER_ICON_SIZE * 4];
        for (int i = 0; i < APP_AGENT_COUNT; ++i) {
            const uint8_t *alpha = k_mcode_provider_icon_alpha[i];
            for (int p = 0; p < MCODE_PROVIDER_ICON_SIZE * MCODE_PROVIDER_ICON_SIZE; ++p) {
                rgba[p * 4 + 0] = 255;
                rgba[p * 4 + 1] = 255;
                rgba[p * 4 + 2] = 255;
                rgba[p * 4 + 3] = alpha[p];
            }
            app->provider_icons[i] = cui_image_create(
                ui, MCODE_PROVIDER_ICON_SIZE, MCODE_PROVIDER_ICON_SIZE, rgba);
        }
    }
    if (app && app->provider_icons[agent]) {
        cui_image(ui, app->provider_icons[agent],
                  .width = CUI_PX(size), .height = CUI_PX(size), .tint = tint);
    } else {
        cui_icon(ui, mcode_agent_icon(agent), size, tint);
    }
}

void mcode_ui_load_selected_diff(mcode_thread *thread) {
    if (!thread) return;
    if (thread->diff_file_count <= 0) {
        thread->selected_diff_file = -1;
        free(thread->diff_old_text);
        thread->diff_old_text = NULL;
        free(thread->diff_new_text);
        thread->diff_new_text = NULL;
        thread->diff_loaded_path[0] = '\0';
        return;
    }
    if (thread->selected_diff_file < 0 || thread->selected_diff_file >= thread->diff_file_count) {
        thread->selected_diff_file = 0;
    }
    const mcode_diff_file *f = &thread->diff_files[thread->selected_diff_file];
    char *old_t = NULL;
    char *new_t = NULL;
    if (mcode_git_load_file_diff(thread->workdir, f, &old_t, &new_t)) {
        free(thread->diff_old_text);
        free(thread->diff_new_text);
        thread->diff_old_text = old_t;
        thread->diff_new_text = new_t;
        snprintf(thread->diff_loaded_path, sizeof thread->diff_loaded_path, "%s", f->path);
        thread->diff_change_idx = 0;
    } else {
        free(old_t);
        free(new_t);
    }
}

void mcode_ui_refresh_thread_diff(mcode_thread *thread) {
    if (!thread || !thread->workdir[0]) return;
    char prev_path[512] = "";
    if (thread->selected_diff_file >= 0 && thread->selected_diff_file < thread->diff_file_count) {
        snprintf(prev_path, sizeof prev_path, "%s", thread->diff_files[thread->selected_diff_file].path);
    }

    mcode_diff_file *files = NULL;
    int count = 0, add = 0, rem = 0;
    if (!mcode_git_list_changes(thread->workdir, &files, &count, &add, &rem)) {
        return;
    }
    free(thread->diff_files);
    thread->diff_files = files;
    thread->diff_file_count = count;
    thread->total_added = add;
    thread->total_removed = rem;

    int next_sel = (count > 0) ? 0 : -1;
    if (prev_path[0]) {
        for (int i = 0; i < count; ++i) {
            if (strcmp(files[i].path, prev_path) == 0) {
                next_sel = i;
                break;
            }
        }
    }
    thread->selected_diff_file = next_sel;
    mcode_ui_load_selected_diff(thread);
}

static void resolve_active_project_root(mcode_app *app, const char **out_root,
                                        bool *out_is_git) {
    if (out_root) *out_root = NULL;
    if (out_is_git) *out_is_git = false;
    if (!app) return;

    mcode_thread *t = mcode_store_selected_thread(&app->store);
    mcode_project *p = t ? mcode_store_find_project(&app->store, t->project_id) : NULL;
    if (!p && app->store.selected_project_id[0]) {
        p = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }
    if (!p && app->store.project_count > 0) {
        p = &app->store.projects[0];
    }

    const char *root = (t && t->workdir[0]) ? t->workdir
                     : (p && p->path[0]) ? p->path
                     : NULL;
    if (out_root) *out_root = root;
    if (out_is_git) *out_is_git = p ? p->is_git_repo : false;
}

void mcode_ui_load_workspace_diff(mcode_app *app) {
    if (!app) return;
    if (app->git_file_count <= 0 || !app->git_workdir[0]) {
        app->git_selected_idx = -1;
        free(app->git_diff_old_text);
        app->git_diff_old_text = NULL;
        free(app->git_diff_new_text);
        app->git_diff_new_text = NULL;
        app->git_diff_loaded_path[0] = '\0';
        app->git_diff_loaded_staged = false;
        app->git_diff_change_count = 0;
        return;
    }
    if (app->git_selected_idx < 0 || app->git_selected_idx >= app->git_file_count) {
        app->git_selected_idx = 0;
    }
    const mcode_diff_file *f = &app->git_files[app->git_selected_idx];
    char *old_t = NULL;
    char *new_t = NULL;
    if (mcode_git_load_file_diff(app->git_workdir, f, &old_t, &new_t)) {
        free(app->git_diff_old_text);
        free(app->git_diff_new_text);
        app->git_diff_old_text = old_t;
        app->git_diff_new_text = new_t;
        snprintf(app->git_diff_loaded_path, sizeof app->git_diff_loaded_path, "%s", f->path);
        app->git_diff_loaded_staged = f->staged;
        app->git_diff_change_idx = 0;
        app->git_diff_change_count = 0;
    } else {
        free(old_t);
        free(new_t);
    }
}

void mcode_ui_refresh_workspace_git(mcode_app *app) {
    if (!app) return;
    const char *root = NULL;
    bool is_git = false;
    resolve_active_project_root(app, &root, &is_git);

    char prev_path[512] = "";
    bool prev_staged = false;
    if (app->git_selected_idx >= 0 && app->git_selected_idx < app->git_file_count) {
        snprintf(prev_path, sizeof prev_path, "%s", app->git_files[app->git_selected_idx].path);
        prev_staged = app->git_files[app->git_selected_idx].staged;
    }

    if (!root || !root[0]) {
        app->git_workdir[0] = '\0';
        memset(&app->git_sync, 0, sizeof app->git_sync);
        if (app->commit_gen_run && app->commit_gen_target == MCODE_COMMIT_GEN_SIDEBAR) {
            mcode_ui_cancel_commit_msg_gen(app);
        }
        free(app->git_files);
        app->git_files = NULL;
        app->git_file_count = 0;
        app->git_total_added = 0;
        app->git_total_removed = 0;
        app->git_selected_idx = -1;
        mcode_ui_load_workspace_diff(app);
        mcode_file_tree_clear(&app->file_tree);
        return;
    }

    if (app->commit_gen_run && app->commit_gen_target == MCODE_COMMIT_GEN_SIDEBAR &&
        strcmp(app->commit_gen_workdir, root) != 0) {
        mcode_ui_cancel_commit_msg_gen(app);
    }

    snprintf(app->git_workdir, sizeof app->git_workdir, "%s", root);

    mcode_diff_file *files = NULL;
    int count = 0, add = 0, rem = 0;
    if (!mcode_git_list_changes(root, &files, &count, &add, &rem)) {
        free(files);
        files = NULL;
        count = 0;
        add = 0;
        rem = 0;
    }

    if (!mcode_git_get_sync_status(root, &app->git_sync)) {
        memset(&app->git_sync, 0, sizeof app->git_sync);
    }

    free(app->git_files);
    app->git_files = files;
    app->git_file_count = count;
    app->git_total_added = add;
    app->git_total_removed = rem;

    int next_sel = (count > 0) ? 0 : -1;
    if (prev_path[0]) {
        for (int i = 0; i < count; ++i) {
            if (strcmp(files[i].path, prev_path) == 0 && files[i].staged == prev_staged) {
                next_sel = i;
                break;
            }
        }
    }
    app->git_selected_idx = next_sel;
    if (app->inspector_open && app->inspector_mode == MCODE_INSPECTOR_DIFF) {
        mcode_ui_load_workspace_diff(app);
    }

    /* Keep selected thread's diff snapshot & live branch mirrored for topbar & commit dialog */
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t && strcmp(t->workdir, root) == 0) {
        free(t->diff_files);
        t->diff_files = NULL;
        if (count > 0) {
            t->diff_files = malloc((size_t)count * sizeof *t->diff_files);
            if (t->diff_files) {
                memcpy(t->diff_files, files, (size_t)count * sizeof *t->diff_files);
            }
        }
        t->diff_file_count = t->diff_files ? count : 0;
        t->total_added = add;
        t->total_removed = rem;
        t->selected_diff_file = (t->diff_file_count > 0) ? next_sel : -1;
        if (app->git_sync.valid && app->git_sync.branch[0] &&
            strcmp(t->branch, app->git_sync.branch) != 0) {
            snprintf(t->branch, sizeof t->branch, "%s", app->git_sync.branch);
        }
    }
    if (app->git_sync.valid && app->git_sync.branch[0]) {
        mcode_project *p = mcode_store_find_project_by_path(&app->store, root);
        if (p && strcmp(p->default_branch, app->git_sync.branch) != 0) {
            snprintf(p->default_branch, sizeof p->default_branch, "%s", app->git_sync.branch);
        }
    }

    mcode_file_tree_refresh(&app->file_tree, root, is_git, app->git_files, app->git_file_count);
}

bool mcode_ui_is_git_op_running(const mcode_app *app, const char *workdir) {
    if (!app || !app->git_op) return false;
    if (!workdir || !workdir[0]) return true;
    return strcmp(mcode_git_op_workdir(app->git_op), workdir) == 0;
}

bool mcode_ui_start_git_op(mcode_app *app, cui_ctx *ui, mcode_git_op_kind kind,
                           const char *workdir, bool commit_followup) {
    if (!app || kind == MCODE_GIT_OP_NONE || !workdir || !workdir[0]) return false;
    if (app->git_op) {
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              "A Git operation is already in progress.", CUI_TONE_INFO);
        }
        return false;
    }

    mcode_git_sync_status local_st;
    const mcode_git_sync_status *st = NULL;
    if (app->git_sync.valid && strcmp(app->git_workdir, workdir) == 0) {
        st = &app->git_sync;
    } else if (mcode_git_get_sync_status(workdir, &local_st)) {
        st = &local_st;
    }

    const char *remote = (st && st->remote[0]) ? st->remote : "origin";
    bool has_upstream = (st && st->has_upstream);

    mcode_git_op *op = mcode_git_op_start(kind, workdir, remote, has_upstream);
    if (!op) {
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              "Could not start Git operation.", CUI_TONE_DANGER);
        }
        return false;
    }

    app->git_op = op;
    app->git_op_was_commit_followup = commit_followup;
    if (ui) cui_request_redraw(ui);
    return true;
}

void mcode_ui_open_discard_all_dialog(mcode_app *app, const char *workdir) {
    if (!app || !workdir || !workdir[0]) return;
    int tracked = 0;
    int untracked = 0;
    int staged = 0;
    for (int i = 0; i < app->git_file_count; ++i) {
        const mcode_diff_file *f = &app->git_files[i];
        if (f->staged) {
            ++staged;
        } else if (f->untracked) {
            ++untracked;
        } else {
            ++tracked;
        }
    }
    if (tracked + untracked == 0) return;
    snprintf(app->discard_dialog_workdir, sizeof app->discard_dialog_workdir, "%s", workdir);
    app->discard_dialog_tracked_count = tracked;
    app->discard_dialog_untracked_count = untracked;
    app->discard_dialog_staged_count = staged;
    app->discard_dialog_open = true;
}

void mcode_ui_perform_discard_all(mcode_app *app, cui_ctx *ui) {
    if (!app || !app->discard_dialog_workdir[0]) {
        if (app) app->discard_dialog_open = false;
        return;
    }

    char workdir[MCODE_PATH_MAX];
    snprintf(workdir, sizeof workdir, "%s", app->discard_dialog_workdir);
    int staged_before = app->discard_dialog_staged_count;

    char err[256] = "";
    bool ok = mcode_git_discard_all(workdir, err, sizeof err);
    app->discard_dialog_open = false;

    if (ok) {
        mcode_project *p = mcode_store_find_project_by_path(&app->store, workdir);
        if (p) {
            char pid[MCODE_ID_MAX];
            snprintf(pid, sizeof pid, "%s", p->id);
            mcode_ui_promote_project(app, pid);
        }
        mcode_ui_refresh_workspace_git(app);
        if (app->commit_gen_run && app->commit_gen_target == MCODE_COMMIT_GEN_SIDEBAR &&
            app->git_file_count == 0) {
            mcode_ui_cancel_commit_msg_gen(app);
        }
        if (app->inspector_open && app->inspector_mode == MCODE_INSPECTOR_DIFF &&
            app->git_file_count == 0) {
            mcode_ui_close_inspector(app);
        } else if (app->inspector_open && app->inspector_mode == MCODE_INSPECTOR_FILE &&
                   strcmp(app->preview_workdir, workdir) == 0 &&
                   app->preview_rel_path[0]) {
            char *buf = NULL;
            size_t cap = 0;
            bool is_bin = false;
            bool trunc = false;
            int64_t fsz = 0;
            if (mcode_file_read_preview(app->preview_workdir, app->preview_rel_path,
                                        &buf, &cap, &is_bin, &trunc, &fsz)) {
                free(app->preview_text);
                app->preview_text = buf;
                app->preview_buf_cap = cap;
                app->preview_is_binary = is_bin;
                app->preview_truncated = trunc;
                app->preview_file_size = fsz;
            } else {
                mcode_ui_close_inspector(app);
            }
        }
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              staged_before > 0 ? "Discarded all unstaged changes."
                                                : "Discarded all changes.",
                              CUI_TONE_INFO);
        }
    } else {
        mcode_ui_refresh_workspace_git(app);
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              err[0] ? err : "Could not discard changes.",
                              CUI_TONE_DANGER);
        }
    }
}

void mcode_ui_refresh_file_tree(mcode_app *app) {
    if (!app) return;
    const char *root = NULL;
    bool is_git = false;
    resolve_active_project_root(app, &root, &is_git);
    mcode_file_tree_refresh(&app->file_tree, root, is_git, app->git_files, app->git_file_count);
}

void mcode_ui_close_inspector(mcode_app *app) {
    if (!app) return;
    app->inspector_open = false;
    app->inspector_showing_chat = false;
    app->inspector_mode = MCODE_INSPECTOR_CHAT;
}

void mcode_ui_open_diff_file(mcode_app *app, int file_idx) {
    if (!app || file_idx < 0 || file_idx >= app->git_file_count) return;
    app->git_selected_idx = file_idx;
    mcode_ui_load_workspace_diff(app);
    app->inspector_open = true;
    app->inspector_mode = MCODE_INSPECTOR_DIFF;
    app->inspector_showing_chat = false;
}

void mcode_ui_open_file_preview(mcode_app *app, cui_ctx *ui, const char *root_path,
                                const char *rel_path) {
    if (!app || !root_path || !root_path[0] || !rel_path || !rel_path[0]) return;
    char *buf = NULL;
    size_t cap = 0;
    bool is_bin = false;
    bool trunc = false;
    int64_t fsz = 0;
    if (!mcode_file_read_preview(root_path, rel_path, &buf, &cap, &is_bin, &trunc, &fsz)) {
        if (ui) app_chrome_notify(app->chrome, ui, "Could not read file.", CUI_TONE_DANGER);
        return;
    }
    free(app->preview_text);
    app->preview_text = buf;
    app->preview_buf_cap = cap;
    app->preview_is_binary = is_bin;
    app->preview_truncated = trunc;
    app->preview_file_size = fsz;
    snprintf(app->preview_rel_path, sizeof app->preview_rel_path, "%s", rel_path);
    snprintf(app->preview_workdir, sizeof app->preview_workdir, "%s", root_path);
    app->inspector_open = true;
    app->inspector_mode = MCODE_INSPECTOR_FILE;
    app->inspector_showing_chat = false;
}

static void nav_history_push(mcode_app *app, const char *thread_id) {
    if (!app || !thread_id || !thread_id[0]) return;
    if (app->nav_count > 0 && app->nav_index >= 0 && app->nav_index < app->nav_count &&
        strcmp(app->nav_history[app->nav_index], thread_id) == 0) {
        return;
    }
    int next_pos = (app->nav_count > 0 && app->nav_index >= 0) ? (app->nav_index + 1) : 0;
    if (next_pos >= MCODE_NAV_HISTORY_MAX) {
        memmove(app->nav_history[0], app->nav_history[1],
                (size_t)(MCODE_NAV_HISTORY_MAX - 1) * sizeof app->nav_history[0]);
        next_pos = MCODE_NAV_HISTORY_MAX - 1;
    }
    snprintf(app->nav_history[next_pos], sizeof app->nav_history[next_pos], "%s", thread_id);
    app->nav_index = next_pos;
    app->nav_count = next_pos + 1;
}

static void discard_draft_threads(mcode_app *app, const char *keep_id) {
    if (!app) return;
    for (int i = app->store.thread_count - 1; i >= 0; --i) {
        mcode_thread *t = &app->store.threads[i];
        if (!t->is_draft) continue;
        if (keep_id && keep_id[0] && strcmp(t->id, keep_id) == 0) continue;
        if (t->active_run || t->turn_count > 0) {
            t->is_draft = false;
            continue;
        }
        char tid[MCODE_ID_MAX];
        snprintf(tid, sizeof tid, "%s", t->id);
        mcode_store_remove_thread(&app->store, tid);
    }
}

static int nav_history_find_prev(mcode_app *app) {
    if (!app || app->nav_count <= 0 || app->nav_index < 0) return -1;
    mcode_thread *sel = mcode_store_selected_thread(&app->store);
    if (sel && sel->is_draft) {
        for (int i = app->nav_index; i >= 0; --i) {
            mcode_thread *t = mcode_store_find_thread(&app->store, app->nav_history[i]);
            if (t && !t->is_draft) return i;
        }
        return -1;
    }
    if (app->nav_count <= 1 || app->nav_index <= 0) return -1;
    for (int i = app->nav_index - 1; i >= 0; --i) {
        mcode_thread *t = mcode_store_find_thread(&app->store, app->nav_history[i]);
        if (t && !t->is_draft) return i;
    }
    return -1;
}

static int nav_history_find_next(mcode_app *app) {
    if (!app || app->nav_count <= 1 || app->nav_index < 0) return -1;
    for (int i = app->nav_index + 1; i < app->nav_count; ++i) {
        mcode_thread *t = mcode_store_find_thread(&app->store, app->nav_history[i]);
        if (t && !t->is_draft) return i;
    }
    return -1;
}

static void nav_history_jump(mcode_app *app, cui_ctx *ui, int target_idx) {
    if (!app || target_idx < 0 || target_idx >= app->nav_count) return;
    char target_id[MCODE_ID_MAX];
    snprintf(target_id, sizeof target_id, "%s", app->nav_history[target_idx]);
    bool was_selected = (strcmp(app->store.selected_thread_id, target_id) == 0);
    discard_draft_threads(app, target_id);
    mcode_thread *t = mcode_store_find_thread(&app->store, target_id);
    if (!t) return;
    app->nav_index = target_idx;
    t->has_unread = false;
    mcode_ui_focus_composer(app, ui);
    if (!was_selected || strcmp(app->store.selected_thread_id, t->id) != 0) {
        snprintf(app->store.selected_thread_id, sizeof app->store.selected_thread_id, "%s", t->id);
        mcode_ui_close_inspector(app);
        mcode_ui_refresh_workspace_git(app);
        if (!t->is_draft) mcode_ui_mark_dirty(app);
    } else {
        app->inspector_showing_chat = true;
    }
}

void mcode_ui_select_thread(mcode_app *app, const char *thread_id) {
    if (!app || !thread_id || !thread_id[0]) return;
    char target_id[MCODE_ID_MAX];
    snprintf(target_id, sizeof target_id, "%s", thread_id);
    bool was_selected = (strcmp(app->store.selected_thread_id, target_id) == 0);
    discard_draft_threads(app, target_id);
    mcode_thread *t = mcode_store_find_thread(&app->store, target_id);
    if (!t) return;
    t->has_unread = false;
    mcode_ui_focus_composer(app, NULL);
    if (!t->is_draft) nav_history_push(app, t->id);
    if (was_selected && strcmp(app->store.selected_thread_id, t->id) == 0) {
        app->inspector_showing_chat = true;
        return;
    }
    snprintf(app->store.selected_thread_id, sizeof app->store.selected_thread_id, "%s", t->id);
    mcode_ui_close_inspector(app);
    mcode_ui_refresh_workspace_git(app);
    if (!t->is_draft) mcode_ui_mark_dirty(app);
}

static void append_shell_quoted(mcode_str *out, const char *s) {
    if (!out || !s) return;
#ifdef _WIN32
    mcode_str_append(out, "\"", 1);
    for (const char *p = s; *p; ++p) {
        if (*p == '"') mcode_str_append(out, "\"\"", 2);
        else mcode_str_append(out, p, 1);
    }
    mcode_str_append(out, "\"", 1);
#else
    mcode_str_append(out, "'", 1);
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') mcode_str_append(out, "'\\''", 4);
        else mcode_str_append(out, p, 1);
    }
    mcode_str_append(out, "'", 1);
#endif
}

void mcode_ui_run_terminal_cmd(mcode_app *app, cui_ctx *ui, const char *cmd, const char *cwd) {
    if (!app || !cmd || !cmd[0]) return;
    app->store.show_terminal_panel = true;
    app->composer_want_focus = false;
    app->term_want_focus = true;
    mcode_ui_mark_dirty(app);

    if (!mcode_ui_ensure_terminal(app, cwd)) {
        if (ui) app_chrome_notify(app->chrome, ui, "Failed to start integrated terminal.", CUI_TONE_DANGER);
        return;
    }

    mcode_str line;
    mcode_str_init(&line);
    if (cwd && cwd[0] && strcmp(app->term_cwd, cwd) != 0) {
#ifdef _WIN32
        mcode_str_append_cstr(&line, "cd /d ");
#else
        mcode_str_append_cstr(&line, "cd ");
#endif
        append_shell_quoted(&line, cwd);
        mcode_str_append_cstr(&line, " && ");
        snprintf(app->term_cwd, sizeof app->term_cwd, "%s", cwd);
    }
    mcode_str_append_cstr(&line, cmd);
    mcode_str_append(&line, "\r", 1);

    cui_vterm_scroll_to_bottom(app->term);
    cui_vterm_write(app->term, line.data, line.len);
    mcode_str_free(&line);
    if (ui) cui_request_redraw(ui);
}

void mcode_ui_stop_terminal(mcode_app *app) {
    if (!app || !app->term || !cui_vterm_is_running(app->term)) return;
    cui_vterm_write(app->term, "\x03", 1);
}

void mcode_ui_refresh_discovered_repos(mcode_app *app) {
    if (!app) return;
    app->discovered_repo_count = mcode_git_discover_repos(
        app->store.default_project_dir, app->discovered_repos, MCODE_MAX_DISCOVERED_REPOS);
}

static void remember_parent_dir(mcode_app *app, const char *project_path) {
    if (!app || !project_path || !project_path[0]) return;
    size_t len = strlen(project_path);
    while (len > 1 && (project_path[len - 1] == '/' || project_path[len - 1] == '\\')) --len;
    const char *sep = NULL;
    for (size_t i = len; i > 0; --i) {
        if (project_path[i - 1] == '/' || project_path[i - 1] == '\\') {
            sep = project_path + i - 1;
            break;
        }
    }
    if (sep && sep > project_path) {
        size_t plen = (size_t)(sep - project_path);
        if (plen < sizeof app->store.default_project_dir) {
            memcpy(app->store.default_project_dir, project_path, plen);
            app->store.default_project_dir[plen] = '\0';
        }
    }
}

void mcode_ui_open_project_dialog(mcode_app *app, mcode_project_dialog_mode mode) {
    if (!app) return;
    app->project_dialog_mode = (int)mode;
    app->project_dialog_err[0] = '\0';
    app->new_proj_name[0] = '\0';
    app->new_proj_init_git = true;
    app->clone_url[0] = '\0';
    app->clone_folder[0] = '\0';
    app->clone_folder_edited = false;
    app->discovered_filter[0] = '\0';

    if (app->store.default_project_dir[0]) {
        snprintf(app->new_proj_parent, sizeof app->new_proj_parent, "%s",
                 app->store.default_project_dir);
    } else {
        mcode_git_default_projects_dir(app->new_proj_parent, sizeof app->new_proj_parent);
    }
    if (mode == MCODE_PROJ_DIALOG_OPEN) {
        mcode_ui_refresh_discovered_repos(app);
    }
    app->project_dialog_open = true;
}

mcode_project *mcode_ui_add_project_and_open(mcode_app *app, cui_ctx *ui,
                                             const char *path, const char *name) {
    if (!app || !path || !path[0]) return NULL;
    bool existed = (mcode_store_find_project_by_path(&app->store, path) != NULL);
    mcode_project *p = mcode_store_add_project(&app->store, path, name);
    if (!p) return NULL;

    p->collapsed = false;
    snprintf(app->store.selected_project_id, sizeof app->store.selected_project_id, "%s", p->id);
    remember_parent_dir(app, p->path);

    /* Jump straight into an active thread for this project (or open a draft thread) */
    const char *existing_tid = NULL;
    for (int i = app->store.thread_count - 1; i >= 0; --i) {
        if (!app->store.threads[i].is_draft &&
            strcmp(app->store.threads[i].project_id, p->id) == 0 &&
            app->store.threads[i].state != MCODE_THREAD_SETTLED) {
            existing_tid = app->store.threads[i].id;
            break;
        }
    }
    if (existing_tid) {
        mcode_ui_select_thread(app, existing_tid);
    } else {
        mcode_ui_quick_new_thread(app, ui, p);
    }

    mcode_ui_mark_dirty(app);
    char msg[240];
    snprintf(msg, sizeof msg, "%s project: %s%s",
             existed ? "Opened" : "Added",
             p->name,
             p->is_git_repo ? "" : " (not a Git repo)");
    if (ui) app_chrome_notify(app->chrome, ui, msg, CUI_TONE_SUCCESS);
    return p;
}

void mcode_ui_remove_project(mcode_app *app, cui_ctx *ui, const char *project_id) {
    if (!app || !project_id || !project_id[0]) return;
    mcode_project *p = mcode_store_find_project(&app->store, project_id);
    if (!p) return;

    char name[MCODE_NAME_MAX];
    snprintf(name, sizeof name, "%s", p->name);

    /* Remove threads belonging to this project from Mcode state without touching files on disk */
    for (int i = app->store.thread_count - 1; i >= 0; --i) {
        mcode_thread *t = &app->store.threads[i];
        if (strcmp(t->project_id, project_id) != 0) continue;
        if (t->active_run) {
            mcode_agent_destroy(t->active_run);
            t->active_run = NULL;
        }
        if (t->title_run) {
            mcode_agent_title_destroy(t->title_run);
            t->title_run = NULL;
        }
        char tid[MCODE_ID_MAX];
        snprintf(tid, sizeof tid, "%s", t->id);
        mcode_store_remove_thread(&app->store, tid);
    }

    mcode_store_remove_project(&app->store, project_id);
    if (strcmp(app->actions_project_id, project_id) == 0) {
        app->actions_project_id[0] = '\0';
        app->actions_dialog_open = false;
    }
    if (strcmp(app->rename_project_id, project_id) == 0) {
        app->rename_project_id[0] = '\0';
        app->rename_project_open = false;
    }

    mcode_ui_close_inspector(app);
    mcode_ui_refresh_workspace_git(app);
    mcode_ui_refresh_discovered_repos(app);
    mcode_ui_mark_dirty(app);

    char msg[240];
    snprintf(msg, sizeof msg, "Removed %s from Mcode (files on disk untouched).", name);
    if (ui) app_chrome_notify(app->chrome, ui, msg, CUI_TONE_INFO);
}

void mcode_ui_init_project_git(mcode_app *app, cui_ctx *ui, mcode_project *proj) {
    if (!app || !proj) return;
    char proj_id[MCODE_ID_MAX];
    snprintf(proj_id, sizeof proj_id, "%s", proj->id);
    char err[256] = "";
    if (!mcode_git_init_repo(proj->path, err, sizeof err)) {
        if (ui) app_chrome_notify(app->chrome, ui, err[0] ? err : "git init failed.", CUI_TONE_DANGER);
        return;
    }
    mcode_git_inspect_repo(proj->path, &proj->is_git_repo,
                           proj->default_branch, sizeof proj->default_branch,
                           proj->remote_url, sizeof proj->remote_url);
    for (int i = 0; i < app->store.thread_count; ++i) {
        mcode_thread *t = &app->store.threads[i];
        if (strcmp(t->project_id, proj_id) == 0 && !t->branch[0] && proj->default_branch[0]) {
            snprintf(t->branch, sizeof t->branch, "%s", proj->default_branch);
        }
    }
    mcode_ui_promote_project(app, proj_id);
    mcode_ui_refresh_workspace_git(app);
    mcode_ui_mark_dirty(app);
    if (ui) app_chrome_notify(app->chrome, ui, "Initialized Git repository.", CUI_TONE_SUCCESS);
}

void mcode_ui_open_project_actions_for(mcode_app *app, cui_ctx *ui, const char *project_id) {
    if (!app) return;
    if (app->store.project_count == 0) {
        if (ui) app_chrome_notify(app->chrome, ui, "Add a project first.", CUI_TONE_WARNING);
        return;
    }
    if (project_id && project_id[0]) {
        snprintf(app->actions_project_id, sizeof app->actions_project_id, "%s", project_id);
    } else {
        app->actions_project_id[0] = '\0';
    }
    app->qa_new_name[0] = '\0';
    app->qa_new_cmd[0] = '\0';
    app->actions_dialog_open = true;
}

void mcode_ui_open_rename_project(mcode_app *app, const mcode_project *proj) {
    if (!app || !proj) return;
    snprintf(app->rename_project_id, sizeof app->rename_project_id, "%s", proj->id);
    snprintf(app->rename_project_buf, sizeof app->rename_project_buf, "%s", proj->name);
    app->rename_project_open = true;
}

static void on_clone_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    mcode_app *app = (mcode_app *)user;
    if (!app || !data || len == 0) return;

    /* Extract the latest non-empty progress segment (git prints \r-separated progress lines). */
    char latest[256] = "";
    size_t seg_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || data[i] == '\r' || data[i] == '\n') {
            size_t a = seg_start;
            size_t b = i;
            while (a < b && isspace((unsigned char)data[a])) ++a;
            while (b > a && isspace((unsigned char)data[b - 1])) --b;
            if (b > a) {
                size_t n = b - a;
                if (n >= sizeof latest) n = sizeof latest - 1;
                memcpy(latest, data + a, n);
                latest[n] = '\0';
            }
            seg_start = i + 1;
        }
    }
    if (latest[0]) {
        pthread_mutex_lock(&app->clone_mu);
        snprintf(app->clone_status, sizeof app->clone_status, "%s", latest);
        snprintf(app->clone_last_error, sizeof app->clone_last_error, "%s", latest);
        pthread_mutex_unlock(&app->clone_mu);
    }
}

static void on_clone_exit(int exit_code, bool killed, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    pthread_mutex_lock(&app->clone_mu);
    app->clone_finished = true;
    app->clone_killed = killed;
    app->clone_exit_code = exit_code;
    pthread_mutex_unlock(&app->clone_mu);
}

bool mcode_ui_start_clone(mcode_app *app, cui_ctx *ui, const char *url,
                          const char *parent_dir, const char *folder_name) {
    (void)ui;
    if (!app || app->clone_proc) return false;
    app->project_dialog_err[0] = '\0';

    if (!url || !url[0]) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "Enter a Git repository URL.");
        return false;
    }
    if (!parent_dir || !parent_dir[0]) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "Choose a destination folder.");
        return false;
    }
    if (!folder_name || !folder_name[0] || strcmp(folder_name, ".") == 0 ||
        strcmp(folder_name, "..") == 0 || strchr(folder_name, '/') || strchr(folder_name, '\\')) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "Enter a valid folder name.");
        return false;
    }

    /* Expand leading ~/ in parent_dir */
    char base[MCODE_PATH_MAX];
    if (parent_dir[0] == '~' && (parent_dir[1] == '/' || parent_dir[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            snprintf(app->project_dialog_err, sizeof app->project_dialog_err,
                     "Could not resolve home directory.");
            return false;
        }
        snprintf(base, sizeof base, "%s%s", home, parent_dir + 1);
    } else {
        snprintf(base, sizeof base, "%s", parent_dir);
    }
    size_t blen = strlen(base);
    while (blen > 1 && (base[blen - 1] == '/' || base[blen - 1] == '\\')) base[--blen] = '\0';

    char dest[MCODE_PATH_MAX];
    int n = snprintf(dest, sizeof dest, "%s/%s", base, folder_name);
    if (n <= 0 || (size_t)n >= sizeof dest) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "Destination path is too long.");
        return false;
    }

    struct stat st;
    if (stat(dest, &st) == 0) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err,
                 "Folder already exists: %s (use Open Folder instead).", folder_name);
        return false;
    }

    pthread_mutex_lock(&app->clone_mu);
    snprintf(app->clone_dest, sizeof app->clone_dest, "%s", dest);
    snprintf(app->clone_status, sizeof app->clone_status, "Cloning %s...", folder_name);
    app->clone_last_error[0] = '\0';
    app->clone_finished = false;
    app->clone_killed = false;
    app->clone_exit_code = 0;
    pthread_mutex_unlock(&app->clone_mu);

    const char *argv[] = { "git", "clone", "--progress", url, dest, NULL };
    app->clone_proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = true,
        .on_bytes = on_clone_bytes,
        .on_exit = on_clone_exit,
        .user = app,
    });
    if (!app->clone_proc) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err,
                 "Could not start git clone (is git installed?).");
        return false;
    }
    return true;
}

void mcode_ui_cancel_clone(mcode_app *app) {
    if (!app || !app->clone_proc) return;
    mcode_proc_stop(app->clone_proc);
}

mcode_thread *mcode_ui_create_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool auto_approve) {
    if (!app || !proj) return NULL;
    const char *final_title = (title && title[0]) ? title : "New thread";

    /* Discard any prior unsent draft before opening a fresh one. */
    discard_draft_threads(app, NULL);

    /* Keep new threads as unsent drafts until the first message is sent so
     * opening a new thread never saves empty threads. */
    mcode_thread *t = mcode_store_add_thread(&app->store, proj, final_title,
                                             agent, model, auto_approve,
                                             proj->path, proj->default_branch);
    if (t) {
        t->is_draft = true;
        proj->collapsed = false;
        snprintf(app->store.selected_project_id, sizeof app->store.selected_project_id,
                 "%s", proj->id);
        mcode_ui_close_inspector(app);
        mcode_ui_refresh_workspace_git(app);
        mcode_ui_focus_composer(app, ui);
    }
    return t;
}

mcode_thread *mcode_ui_quick_new_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj) {
    if (!app || !proj) return NULL;
    mcode_thread *sel = mcode_store_selected_thread(&app->store);
    if (sel && sel->is_draft && strcmp(sel->project_id, proj->id) == 0) {
        proj->collapsed = false;
        snprintf(app->store.selected_project_id, sizeof app->store.selected_project_id,
                 "%s", proj->id);
        mcode_ui_close_inspector(app);
        mcode_ui_focus_composer(app, ui);
        return sel;
    }
    const app_settings *s = app_chrome_settings(app->chrome);
    bool auto_app = s ? s->mcode.auto_approve_permissions : false;
    app_agent_kind ag = s ? s->mcode.default_agent : APP_AGENT_CLAUDE;
    return mcode_ui_create_thread(app, ui, proj, "", ag, "", auto_app);
}

void mcode_ui_open_new_thread_dialog(mcode_app *app, const mcode_project *proj) {
    if (!app || app->store.project_count == 0) return;
    const app_settings *s = app_chrome_settings(app->chrome);
    app->new_thread_project_idx = 0;
    const char *want_id = proj ? proj->id : app->store.selected_project_id;
    if (want_id && want_id[0]) {
        for (int i = 0; i < app->store.project_count; ++i) {
            if (strcmp(app->store.projects[i].id, want_id) == 0) {
                app->new_thread_project_idx = i;
                break;
            }
        }
    }
    app->new_thread_agent = (int)s->mcode.default_agent;
    app->new_thread_title[0] = '\0';
    app->new_thread_model[0] = '\0';
    app->new_thread_custom_model = false;
    app->new_thread_auto_approve = s->mcode.auto_approve_permissions;
    app->new_thread_open = true;
}

static bool prompt_is_blank(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        ++s;
    }
    return true;
}

static bool start_thread_turn(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                              const char *prompt, bool is_steer) {
    if (!app || !thread || prompt_is_blank(prompt)) return false;
    if (thread->active_run) return false;

    bool should_auto_title = (thread->turn_count == 0 &&
        (strcmp(thread->title, "New thread") == 0 || thread->title[0] == '\0'));
    bool was_draft = thread->is_draft;

    thread->state = MCODE_THREAD_ACTIVE;
    thread->has_unread = false;
    thread->stop_requested = false;
    thread->queue_paused = false;
    char err[256] = "";
    const app_settings *settings = app_chrome_settings(app->chrome);
    if (!mcode_agent_start(thread, settings, prompt, err, sizeof err)) {
        if (ui) app_chrome_notify(app->chrome, ui, err[0] ? err : "Could not start agent.", CUI_TONE_DANGER);
        return false;
    }

    if (is_steer && thread->turn_count > 0) {
        thread->turns[thread->turn_count - 1].steered = true;
    }
    if (was_draft) {
        thread->is_draft = false;
        thread->created_at = mcode_now_epoch();
        thread->updated_at = thread->created_at;
        nav_history_push(app, thread->id);
        mcode_ui_refresh_thread_diff(thread);
    }
    /* Set an immediate fallback title and ask the provider for a concise title in parallel. */
    if (should_auto_title) {
        const char *p = prompt;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && p[n] != '\r' && n < 56) ++n;
        while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80) --n;
        if (n > 0) {
            snprintf(thread->title, sizeof thread->title, "%.*s%s",
                     (int)n, p, (p[n] != '\0') ? "..." : "");
        }
        snprintf(thread->title_auto_fallback, sizeof thread->title_auto_fallback,
                 "%s", thread->title);
        if (thread->title_run) {
            mcode_agent_title_destroy(thread->title_run);
            thread->title_run = NULL;
        }
        thread->title_pending = false;
        if (thread->agent == APP_AGENT_AGY) {
            /* Defer agy title generation until the turn completes so two concurrent
             * `agy -p` processes do not race on agy's local trajectory database. */
            thread->title_pending = true;
        } else {
            thread->title_run = mcode_agent_start_title(thread, settings, prompt);
        }
    }
    mcode_ui_promote_project(app, thread->project_id);
    mcode_ui_mark_dirty(app);
    return true;
}

void mcode_ui_send_prompt(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt) {
    (void)start_thread_turn(app, ui, thread, prompt, false);
}

void mcode_ui_queue_message(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt) {
    if (!app || !thread || prompt_is_blank(prompt)) return;
    if (!thread->active_run && !thread->queue_paused && thread->queued_count == 0) {
        mcode_ui_send_prompt(app, ui, thread, prompt);
        return;
    }
    if (mcode_thread_queue_push(thread, prompt)) {
        if (thread->active_run) {
            thread->queue_paused = false;
        }
        thread->updated_at = mcode_now_epoch();
        mcode_ui_mark_dirty(app);
    }
}

void mcode_ui_steer_message(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt) {
    if (!app || !thread || prompt_is_blank(prompt)) return;
    if (!thread->active_run) {
        mcode_ui_send_prompt(app, ui, thread, prompt);
        return;
    }

    mcode_steer_result res = mcode_agent_steer(thread->active_run, thread, prompt);
    if (res == MCODE_STEER_INJECTED) {
        if (thread->turn_count > 0) {
            mcode_block *b = mcode_turn_add_block(&thread->turns[thread->turn_count - 1],
                                                  MCODE_BLOCK_STEER);
            if (b) mcode_str_set(&b->text, prompt);
        }
        snprintf(thread->status_text, sizeof thread->status_text, "Steering...");
        thread->queue_paused = false;
        thread->stop_requested = false;
        thread->scroll_to_bottom_frames = 3;
        thread->updated_at = mcode_now_epoch();
        mcode_ui_mark_dirty(app);
    } else if (res == MCODE_STEER_INTERRUPTING) {
        if (thread->steer_pending && thread->pending_steer.len > 0) {
            mcode_str_append_cstr(&thread->pending_steer, "\n\n");
            mcode_str_append_cstr(&thread->pending_steer, prompt);
        } else {
            mcode_str_set(&thread->pending_steer, prompt);
            thread->steer_pending = true;
        }
        snprintf(thread->status_text, sizeof thread->status_text, "Steering — redirecting...");
        thread->queue_paused = false;
        thread->stop_requested = false;
        thread->scroll_to_bottom_frames = 2;
        thread->updated_at = mcode_now_epoch();
        mcode_ui_mark_dirty(app);
    } else {
        mcode_thread_queue_push_front(thread, prompt);
        thread->queue_paused = false;
        mcode_ui_mark_dirty(app);
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              "Could not steer active run; queued message next.",
                              CUI_TONE_WARNING);
        }
    }
}

void mcode_ui_steer_queued_item(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int index) {
    if (!app || !thread || index < 0 || index >= thread->queued_count) return;
    char *copy = strdup(mcode_str_cstr(&thread->queued_msgs[index].text));
    if (!copy) return;
    mcode_thread_queue_remove(thread, index);
    mcode_ui_mark_dirty(app);
    if (thread->active_run) {
        mcode_ui_steer_message(app, ui, thread, copy);
    } else {
        mcode_ui_send_prompt(app, ui, thread, copy);
    }
    free(copy);
}

void mcode_ui_send_next_queued(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!app || !thread || thread->active_run || thread->queued_count <= 0) return;
    mcode_str next;
    mcode_str_init(&next);
    if (mcode_thread_queue_pop_front(thread, &next) && !prompt_is_blank(mcode_str_cstr(&next))) {
        if (!start_thread_turn(app, ui, thread, mcode_str_cstr(&next), false)) {
            /* Put it back at the front if the CLI failed to launch */
            mcode_thread_queue_push_front(thread, mcode_str_cstr(&next));
            thread->queue_paused = true;
        }
    }
    mcode_str_free(&next);
    mcode_ui_mark_dirty(app);
}

void mcode_ui_cancel_pending_steer(mcode_app *app, mcode_thread *thread) {
    if (!app || !thread || !thread->steer_pending) return;
    if (thread->pending_steer.len > 0) {
        if (!thread->composer_draft[0]) {
            snprintf(thread->composer_draft, sizeof thread->composer_draft, "%s",
                     mcode_str_cstr(&thread->pending_steer));
        } else {
            mcode_thread_queue_push_front(thread, mcode_str_cstr(&thread->pending_steer));
        }
    }
    thread->steer_pending = false;
    mcode_str_clear(&thread->pending_steer);
    mcode_ui_mark_dirty(app);
}

void mcode_ui_stop_thread(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!thread || !thread->active_run) return;
    thread->stop_requested = true;
    if (thread->steer_pending) {
        if (thread->pending_steer.len > 0) {
            mcode_thread_queue_push_front(thread, mcode_str_cstr(&thread->pending_steer));
        }
        thread->steer_pending = false;
        mcode_str_clear(&thread->pending_steer);
    }
    if (thread->queued_count > 0) {
        thread->queue_paused = true;
        mcode_ui_mark_dirty(app);
    }
    mcode_agent_stop(thread->active_run);
    snprintf(thread->status_text, sizeof thread->status_text, "Stopping...");
    if (ui) app_chrome_notify(app->chrome, ui, "Stopping agent turn...", CUI_TONE_INFO);
}

void mcode_ui_delete_thread(mcode_app *app, cui_ctx *ui, const char *thread_id) {
    if (!app || !thread_id) return;
    mcode_thread *t = mcode_store_find_thread(&app->store, thread_id);
    if (!t) return;
    if (t->active_run) {
        mcode_agent_destroy(t->active_run);
        t->active_run = NULL;
    }
    if (t->title_run) {
        mcode_agent_title_destroy(t->title_run);
        t->title_run = NULL;
    }
    mcode_store_remove_thread(&app->store, thread_id);
    mcode_thread *next = mcode_store_selected_thread(&app->store);
    if (next) mcode_ui_refresh_thread_diff(next);
    mcode_ui_mark_dirty(app);
    if (ui) app_chrome_notify(app->chrome, ui, "Thread deleted.", CUI_TONE_INFO);
}

void mcode_app_init(mcode_app *app, app_chrome *chrome) {
    if (!app) return;
    memset(app, 0, sizeof *app);
    app->chrome = chrome;
    mcode_store_init(&app->store);
    mcode_file_tree_init(&app->file_tree);
    app->git_selected_idx = -1;
    app->staged_expanded = true;
    if (chrome) app->last_focus_generation = chrome->focus_generation;
    app->last_focus_refresh_at = mcode_now_mono();
    pthread_mutex_init(&app->clone_mu, NULL);

    char dir[MCODE_PATH_MAX] = "";
    if (chrome && app_settings_dir(dir, sizeof dir, chrome->cli.settings_path, true)) {
        snprintf(app->state_path, sizeof app->state_path, "%s/mcode-state.json", dir);
        if (mcode_store_load(&app->store, app->state_path)) {
            mcode_thread *sel = mcode_store_selected_thread(&app->store);
            if (sel) {
                nav_history_push(app, sel->id);
            }
            mcode_ui_refresh_workspace_git(app);
        }
    }

    mcode_agent_detect_clis(chrome ? app_chrome_settings(chrome) : NULL, &app->cli_status);
    app->cli_checked_at = mcode_now_mono();
    mcode_cli_updates_init(&app->cli_updates);
    mcode_model_catalog_init(&app->models);
    mcode_model_catalog_refresh(&app->models, &app->cli_status);
    mcode_usage_tracker_init(&app->usage);
    app->usage_tab = 0;
    app->usage_scope = 2; /* All */
    if (!(chrome && chrome->env_screenshot_mode)) {
        mcode_usage_tracker_refresh(&app->usage, &app->cli_status, false);
        app->usage_checked_at = mcode_now_mono();
        mcode_cli_updates_check_all(&app->cli_updates, &app->cli_status, false);
        app->cli_updates_checked_at = mcode_now_mono();
    }
    if (app->store.project_count == 0) {
        mcode_ui_refresh_discovered_repos(app);
    }
}

void mcode_app_shutdown(mcode_app *app) {
    if (!app) return;

    /* Flush the native window close immediately so the window vanishes on screen
     * before background subprocess teardown runs. */
    app_platform_flush_window_close();

    /* Signal all background subprocesses in parallel first so they exit concurrently
     * instead of paying sequential per-process wait timeouts. */
    double shutdown_start = mcode_now_mono();
    mcode_usage_tracker_begin_shutdown(&app->usage);
    mcode_model_catalog_begin_shutdown(&app->models);
    mcode_cli_updates_begin_shutdown(&app->cli_updates);
    if (app->clone_proc) {
        mcode_proc_stop(app->clone_proc);
    }
    if (app->git_op) {
        mcode_git_op_stop(app->git_op);
    }
    if (app->commit_gen_run) {
        mcode_agent_commit_msg_kill(app->commit_gen_run);
    }
    for (int i = 0; i < app->store.thread_count; ++i) {
        if (app->store.threads[i].title_run) {
            mcode_agent_title_kill(app->store.threads[i].title_run);
        }
        if (app->store.threads[i].active_run) {
            mcode_agent_stop(app->store.threads[i].active_run);
        }
    }

    /* Persist workspace state and close the PTY while signaled processes exit. */
    if (app->state_path[0]) {
        mcode_store_save(&app->store, app->state_path);
    }
    if (app->term) {
        cui_vterm_destroy(app->term);
        app->term = NULL;
    }

    /* Reap disposable read-only probes (already SIGKILLed above). */
    if (app->commit_gen_run) {
        mcode_agent_commit_msg_destroy(app->commit_gen_run);
        app->commit_gen_run = NULL;
        app->commit_gen_target = MCODE_COMMIT_GEN_NONE;
        app->commit_gen_workdir[0] = '\0';
    }
    mcode_usage_tracker_free(&app->usage);
    mcode_model_catalog_free(&app->models);
    mcode_cli_updates_free(&app->cli_updates);

    /* Reap active agent turns, git ops, and clones sharing a single short grace budget. */
    enum { SHUTDOWN_GRACE_MS = 80 };
    if (app->clone_proc) {
        int elapsed = (int)((mcode_now_mono() - shutdown_start) * 1000.0);
        int rem = SHUTDOWN_GRACE_MS - elapsed;
        mcode_proc_destroy_timeout(app->clone_proc, rem > 0 ? rem : 0);
        app->clone_proc = NULL;
    }
    if (app->git_op) {
        int elapsed = (int)((mcode_now_mono() - shutdown_start) * 1000.0);
        int rem = SHUTDOWN_GRACE_MS - elapsed;
        mcode_git_op_destroy_timeout(app->git_op, rem > 0 ? rem : 0);
        app->git_op = NULL;
        app->git_op_was_commit_followup = false;
    }
    for (int i = 0; i < app->store.thread_count; ++i) {
        if (app->store.threads[i].title_run) {
            mcode_agent_title_destroy(app->store.threads[i].title_run);
            app->store.threads[i].title_run = NULL;
        }
        if (app->store.threads[i].active_run) {
            int elapsed = (int)((mcode_now_mono() - shutdown_start) * 1000.0);
            int rem = SHUTDOWN_GRACE_MS - elapsed;
            mcode_agent_destroy_timeout(app->store.threads[i].active_run, rem > 0 ? rem : 0);
            app->store.threads[i].active_run = NULL;
        }
    }
    mcode_store_free(&app->store);
    mcode_file_tree_free(&app->file_tree);
    free(app->git_files);
    app->git_files = NULL;
    app->git_file_count = 0;
    free(app->git_diff_old_text);
    app->git_diff_old_text = NULL;
    free(app->git_diff_new_text);
    app->git_diff_new_text = NULL;
    free(app->preview_text);
    app->preview_text = NULL;
    app->preview_buf_cap = 0;
    pthread_mutex_destroy(&app->clone_mu);
}

void mcode_ui_cancel_commit_msg_gen(mcode_app *app) {
    if (!app) return;
    if (app->commit_gen_run) {
        mcode_agent_commit_msg_stop(app->commit_gen_run);
        mcode_agent_commit_msg_destroy(app->commit_gen_run);
        app->commit_gen_run = NULL;
    }
    app->commit_gen_target = MCODE_COMMIT_GEN_NONE;
    app->commit_gen_workdir[0] = '\0';
    app->commit_gen_focus_target = MCODE_COMMIT_GEN_NONE;
}

bool mcode_ui_is_generating_commit_msg(const mcode_app *app, mcode_commit_gen_target target) {
    if (!app || !app->commit_gen_run) return false;
    if (target == MCODE_COMMIT_GEN_NONE) return true;
    return app->commit_gen_target == target;
}

static bool any_agy_run_active(const mcode_app *app) {
    if (!app) return false;
    if (app->commit_gen_run && mcode_agent_commit_msg_agent(app->commit_gen_run) == APP_AGENT_AGY) {
        return true;
    }
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->agent == APP_AGENT_AGY && (t->active_run || t->title_run)) {
            return true;
        }
    }
    return false;
}

void mcode_ui_start_commit_msg_gen(mcode_app *app, cui_ctx *ui, const char *workdir,
                                   bool staged_only, mcode_commit_gen_target target) {
    if (!app || !workdir || !workdir[0]) return;
    if (app->commit_gen_run) {
        mcode_ui_cancel_commit_msg_gen(app);
    }

    char *ctx = NULL;
    if (!mcode_git_collect_commit_context(workdir, staged_only, 8192, &ctx) || !ctx) {
        free(ctx);
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              staged_only ? "No staged changes to summarize."
                                          : "No changes to summarize.",
                              CUI_TONE_INFO);
        }
        return;
    }

    const app_settings *settings = app_chrome_settings(app->chrome);
    const mcode_thread *sel = mcode_store_selected_thread(&app->store);
    bool agy_busy = any_agy_run_active(app);

    app_agent_kind order[APP_AGENT_COUNT];
    int order_count = 0;
    app_agent_kind preferred = sel ? sel->agent
                             : (settings ? settings->mcode.default_agent : APP_AGENT_CLAUDE);
    if (preferred < 0 || preferred >= APP_AGENT_COUNT) preferred = APP_AGENT_CLAUDE;
    order[order_count++] = preferred;
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if ((app_agent_kind)i != preferred) {
            order[order_count++] = (app_agent_kind)i;
        }
    }

    app_agent_kind chosen = APP_AGENT_COUNT;
    for (int pass = 0; pass < 2 && chosen == APP_AGENT_COUNT; ++pass) {
        if (pass == 1) {
            mcode_agent_detect_clis(settings, &app->cli_status);
            app->cli_checked_at = mcode_now_mono();
        }
        for (int i = 0; i < order_count; ++i) {
            app_agent_kind cand = order[i];
            if (!app->cli_status.available[cand]) continue;
            if (cand == APP_AGENT_AGY && agy_busy) continue;
            chosen = cand;
            break;
        }
    }

    if (chosen == APP_AGENT_COUNT) {
        free(ctx);
        if (ui) {
            if (app->cli_status.available[APP_AGENT_AGY] && agy_busy) {
                app_chrome_notify(app->chrome, ui,
                                  "Antigravity CLI is busy — try again when the active turn finishes.",
                                  CUI_TONE_WARNING);
            } else {
                app_chrome_notify(app->chrome, ui,
                                  "No AI CLI found (Claude, Codex, or Antigravity) to generate a commit message.",
                                  CUI_TONE_WARNING);
            }
        }
        return;
    }

    const char *model = (sel && sel->agent == chosen)
        ? mcode_agent_effective_model(sel, settings)
        : mcode_agent_default_model(settings, chosen);

    mcode_commit_msg_run *run = mcode_agent_start_commit_msg(chosen, model, workdir, settings, ctx);
    free(ctx);
    if (!run) {
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              "Could not start commit message generation.",
                              CUI_TONE_DANGER);
        }
        return;
    }

    app->commit_gen_run = run;
    app->commit_gen_target = target;
    snprintf(app->commit_gen_workdir, sizeof app->commit_gen_workdir, "%s", workdir);
    app->commit_gen_focus_target = MCODE_COMMIT_GEN_NONE;
    if (ui) cui_request_redraw(ui);
}

static void poll_background_work(mcode_app *app, cui_ctx *ui, int *out_running_count) {
    int running = 0;
    int title_running = 0;
    bool any_finished = false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        mcode_thread *t = &app->store.threads[i];
        if (t->title_run) {
            char generated[MCODE_NAME_MAX] = "";
            bool ok = false;
            if (mcode_agent_poll_title(t->title_run, generated, sizeof generated, &ok)) {
                mcode_agent_title_destroy(t->title_run);
                t->title_run = NULL;
                if (ok && generated[0] &&
                    t->title_auto_fallback[0] &&
                    strcmp(t->title, t->title_auto_fallback) == 0) {
                    snprintf(t->title, sizeof t->title, "%s", generated);
                    t->updated_at = mcode_now_epoch();
                    mcode_ui_mark_dirty(app);
                    any_finished = true;
                }
                t->title_auto_fallback[0] = '\0';
            } else {
                ++title_running;
            }
        }
        if (!t->active_run) continue;
        bool finished = false;
        (void)mcode_agent_poll(t->active_run, t, &app->usage, &finished);
        if (finished) {
            mcode_agent_destroy(t->active_run);
            t->active_run = NULL;
            bool was_stopped = t->stop_requested;
            t->stop_requested = false;
            bool last_failed = (t->turn_count > 0 && t->turns[t->turn_count - 1].failed);

            if (strcmp(app->store.selected_thread_id, t->id) == 0) {
                mcode_ui_refresh_workspace_git(app);
            } else {
                mcode_ui_refresh_thread_diff(t);
            }
            if (!(app->chrome && app->chrome->env_screenshot_mode)) {
                mcode_usage_tracker_refresh_agent(&app->usage, &app->cli_status, t->agent, false);
                app->usage_checked_at = mcode_now_mono();
            }

            /* Dispatch pending steer redirect immediately (ahead of queued messages). */
            if (!was_stopped && t->steer_pending && !prompt_is_blank(mcode_str_cstr(&t->pending_steer))) {
                char *steer_prompt = strdup(mcode_str_cstr(&t->pending_steer));
                t->steer_pending = false;
                mcode_str_clear(&t->pending_steer);
                if (steer_prompt) {
                    if (!start_thread_turn(app, ui, t, steer_prompt, true)) {
                        mcode_thread_queue_push_front(t, steer_prompt);
                        t->queue_paused = true;
                    }
                    free(steer_prompt);
                }
            } else if (!was_stopped && !last_failed && !t->queue_paused && t->queued_count > 0) {
                /* Auto-drain the next queued follow-up message on clean completion. */
                mcode_str next_msg;
                mcode_str_init(&next_msg);
                if (mcode_thread_queue_pop_front(t, &next_msg) &&
                    !prompt_is_blank(mcode_str_cstr(&next_msg))) {
                    if (!start_thread_turn(app, ui, t, mcode_str_cstr(&next_msg), false)) {
                        mcode_thread_queue_push_front(t, mcode_str_cstr(&next_msg));
                        t->queue_paused = true;
                    }
                }
                mcode_str_free(&next_msg);
            } else if ((was_stopped || last_failed) && t->queued_count > 0) {
                /* Pause remaining queue on Stop or error so user can review before continuing. */
                t->queue_paused = true;
            }

            mcode_ui_mark_dirty(app);
            any_finished = true;
            if (t->active_run) {
                ++running;
            } else {
                if (!was_stopped && !last_failed &&
                    strcmp(app->store.selected_thread_id, t->id) != 0) {
                    t->has_unread = true;
                }
                char msg[220];
                snprintf(msg, sizeof msg, "Finished: %s", t->title);
                app_chrome_notify(app->chrome, ui, msg, CUI_TONE_SUCCESS);
            }
        } else {
            ++running;
        }
    }

    /* Poll async commit message generation */
    bool commit_gen_running = false;
    if (app->commit_gen_run) {
        char generated[512] = "";
        bool ok = false;
        if (mcode_agent_poll_commit_msg(app->commit_gen_run, generated, sizeof generated, &ok)) {
            mcode_commit_gen_target target = app->commit_gen_target;
            char gen_wd[MCODE_PATH_MAX];
            snprintf(gen_wd, sizeof gen_wd, "%s", app->commit_gen_workdir);

            mcode_agent_commit_msg_destroy(app->commit_gen_run);
            app->commit_gen_run = NULL;
            app->commit_gen_target = MCODE_COMMIT_GEN_NONE;
            app->commit_gen_workdir[0] = '\0';
            any_finished = true;

            if (ok && generated[0]) {
                if (target == MCODE_COMMIT_GEN_SIDEBAR &&
                    strcmp(app->git_workdir, gen_wd) == 0) {
                    snprintf(app->sidebar_commit_msg, sizeof app->sidebar_commit_msg,
                             "%s", generated);
                    app->commit_gen_focus_target = MCODE_COMMIT_GEN_SIDEBAR;
                } else if (target == MCODE_COMMIT_GEN_DIALOG && app->git_dialog_open) {
                    mcode_thread *sel = mcode_store_selected_thread(&app->store);
                    if (sel && strcmp(sel->workdir, gen_wd) == 0) {
                        snprintf(app->commit_msg, sizeof app->commit_msg, "%s", generated);
                        app->commit_gen_focus_target = MCODE_COMMIT_GEN_DIALOG;
                    }
                }
            } else if (ui) {
                app_chrome_notify(app->chrome, ui,
                                  "Could not generate commit message.",
                                  CUI_TONE_WARNING);
            }
        } else {
            commit_gen_running = true;
        }
    }

    /* Launch at most one deferred Antigravity title request once no `agy` turn/title is active. */
    bool agy_busy = any_agy_run_active(app);
    if (!agy_busy) {
        const app_settings *settings = app_chrome_settings(app->chrome);
        for (int i = 0; i < app->store.thread_count; ++i) {
            mcode_thread *t = &app->store.threads[i];
            if (!t->title_pending || t->active_run || t->title_run) continue;
            t->title_pending = false;
            if (t->turn_count > 0 && !t->turns[0].failed &&
                t->title_auto_fallback[0] &&
                strcmp(t->title, t->title_auto_fallback) == 0 &&
                t->turns[0].prompt.len > 0) {
                t->title_run = mcode_agent_start_title(t, settings, mcode_str_cstr(&t->turns[0].prompt));
                if (t->title_run) {
                    ++title_running;
                    break;
                }
            }
        }
    }

    /* Drain integrated terminal PTY stream */
    bool term_dirty = app->term ? cui_vterm_poll(app->term) : false;
    bool term_visible_running = app->store.show_terminal_panel &&
                                app->term && cui_vterm_is_running(app->term);

    /* Poll async git remote operation (sync / publish / push / pull / fetch) */
    bool git_op_running = false;
    if (app->git_op) {
        bool ok = false;
        char msg[256] = "";
        if (mcode_git_op_poll(app->git_op, &ok, msg, sizeof msg)) {
            mcode_git_op_kind kind = mcode_git_op_kind_of(app->git_op);
            char op_wd[MCODE_PATH_MAX];
            snprintf(op_wd, sizeof op_wd, "%s", mcode_git_op_workdir(app->git_op));
            bool was_commit_followup = app->git_op_was_commit_followup;

            mcode_git_op_destroy(app->git_op);
            app->git_op = NULL;
            app->git_op_was_commit_followup = false;
            any_finished = true;

            if (ok && op_wd[0]) {
                mcode_project *p = mcode_store_find_project_by_path(&app->store, op_wd);
                if (p) {
                    char pid[MCODE_ID_MAX];
                    snprintf(pid, sizeof pid, "%s", p->id);
                    mcode_ui_promote_project(app, pid);
                }
            }
            if (!op_wd[0] || strcmp(app->git_workdir, op_wd) == 0) {
                mcode_ui_refresh_workspace_git(app);
            }
            if (ui) {
                const char *toast = msg;
                if (ok && was_commit_followup) {
                    if (kind == MCODE_GIT_OP_SYNC) toast = "Committed and synced.";
                    else if (kind == MCODE_GIT_OP_PUBLISH) toast = "Committed and published branch.";
                    else toast = "Committed and pushed.";
                } else if (!toast[0]) {
                    toast = ok ? "Git operation completed." : "Git operation failed.";
                }
                app_chrome_notify(app->chrome, ui, toast,
                                  ok ? CUI_TONE_SUCCESS : CUI_TONE_DANGER);
            }
        } else {
            git_op_running = true;
        }
    }

    /* Poll async git clone */
    bool clone_active = (app->clone_proc != NULL);
    if (clone_active) {
        pthread_mutex_lock(&app->clone_mu);
        bool cfin = app->clone_finished;
        bool ckilled = app->clone_killed;
        int ccode = app->clone_exit_code;
        char cdest[MCODE_PATH_MAX];
        char cerr[256];
        snprintf(cdest, sizeof cdest, "%s", app->clone_dest);
        snprintf(cerr, sizeof cerr, "%s", app->clone_last_error);
        app->clone_finished = false;
        pthread_mutex_unlock(&app->clone_mu);

        if (cfin) {
            mcode_proc_destroy(app->clone_proc);
            app->clone_proc = NULL;
            clone_active = false;
            any_finished = true;
            if (!ckilled && ccode == 0) {
                app->project_dialog_err[0] = '\0';
                mcode_ui_add_project_and_open(app, ui, cdest, NULL);
                app->project_dialog_open = false;
            } else if (!ckilled) {
                snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "%s",
                         cerr[0] ? cerr : "git clone failed.");
            }
        }
    }

    bool catalog_updated = mcode_model_catalog_poll(&app->models);
    bool usage_updated = mcode_usage_tracker_poll(&app->usage, &app->cli_status, agy_busy);
    bool cli_updates_changed = mcode_cli_updates_poll(&app->cli_updates, &app->cli_status);

    mcode_cli_update_result cli_upd_res;
    while (mcode_cli_updates_take_result(&app->cli_updates, &cli_upd_res)) {
        cli_updates_changed = true;
        if (cli_upd_res.ok) {
            mcode_agent_detect_clis(app_chrome_settings(app->chrome), &app->cli_status);
            app->cli_checked_at = mcode_now_mono();
            mcode_model_catalog_refresh(&app->models, &app->cli_status);
        }
        if (ui && cli_upd_res.message[0]) {
            app_chrome_notify(app->chrome, ui, cli_upd_res.message,
                              cli_upd_res.ok ? CUI_TONE_SUCCESS : CUI_TONE_WARNING);
        }
    }

    if (app->cli_updates_manual_check_pending && !mcode_cli_updates_is_busy(&app->cli_updates)) {
        app->cli_updates_manual_check_pending = false;
        int avail = mcode_cli_updates_available_count(&app->cli_updates);
        if (ui) {
            if (avail > 0) {
                char msg[96];
                snprintf(msg, sizeof msg, "%d provider CLI update%s available.",
                         avail, avail == 1 ? "" : "s");
                app_chrome_notify(app->chrome, ui, msg, CUI_TONE_INFO);
                app->cli_updates_open_requested = true;
            } else {
                app_chrome_notify(app->chrome, ui,
                                  "All installed provider CLIs are up to date.",
                                  CUI_TONE_SUCCESS);
            }
        }
        cli_updates_changed = true;
    }

    mcode_reset_credit_outcome reset_outcome = MCODE_RESET_OUTCOME_NONE;
    char reset_err[160] = "";
    if (mcode_usage_tracker_take_codex_reset_result(&app->usage, &reset_outcome,
                                                    reset_err, sizeof reset_err)) {
        usage_updated = true;
        if (ui) {
            switch (reset_outcome) {
            case MCODE_RESET_OUTCOME_RESET:
                app_chrome_notify(app->chrome, ui,
                                  "Codex rate limits reset.", CUI_TONE_SUCCESS);
                break;
            case MCODE_RESET_OUTCOME_NOTHING_TO_RESET:
                app_chrome_notify(app->chrome, ui,
                                  "Codex rate limits are already at 100% — no reset credit used.",
                                  CUI_TONE_INFO);
                break;
            case MCODE_RESET_OUTCOME_NO_CREDIT:
                app_chrome_notify(app->chrome, ui,
                                  "No Codex reset credits available.", CUI_TONE_WARNING);
                break;
            case MCODE_RESET_OUTCOME_ALREADY_REDEEMED:
                app_chrome_notify(app->chrome, ui,
                                  "That Codex reset credit was already redeemed.", CUI_TONE_WARNING);
                break;
            case MCODE_RESET_OUTCOME_ERROR:
            default:
                app_chrome_notify(app->chrome, ui,
                                  reset_err[0] ? reset_err : "Failed to redeem Codex reset credit.",
                                  CUI_TONE_DANGER);
                break;
            }
        }
    }

    if (term_dirty) {
        cui_request_redraw(ui);
    } else if (running > 0 || clone_active || commit_gen_running || git_op_running || term_visible_running) {
        cui_request_wakeup(ui, 0.033f);
    } else if (title_running > 0 || app->models.loading_agy ||
               mcode_usage_tracker_is_loading(&app->usage) ||
               mcode_cli_updates_is_busy(&app->cli_updates)) {
        cui_request_wakeup(ui, 0.1f);
    } else if (any_finished || catalog_updated || usage_updated || cli_updates_changed) {
        cui_request_redraw(ui);
    }

    double now = mcode_now_mono();

    /* Refresh active project's Git changes & files when the window regains focus */
    if (app->chrome && app->chrome->focus_generation != app->last_focus_generation) {
        app->last_focus_generation = app->chrome->focus_generation;
        if (!app->chrome->env_screenshot_mode && (now - app->last_focus_refresh_at) >= 0.4) {
            app->last_focus_refresh_at = now;
            if (app->store.project_count > 0 || mcode_store_selected_thread(&app->store)) {
                mcode_ui_refresh_workspace_git(app);
                if (app->inspector_open && app->inspector_mode == MCODE_INSPECTOR_FILE &&
                    app->preview_workdir[0] && app->preview_rel_path[0]) {
                    mcode_ui_open_file_preview(app, NULL, app->preview_workdir,
                                               app->preview_rel_path);
                }
            }
            if ((now - app->usage_checked_at) >= 60.0) {
                mcode_usage_tracker_refresh(&app->usage, &app->cli_status, agy_busy);
                app->usage_checked_at = now;
            }
            /* Re-verify local CLI versions on focus if updates were pending (e.g. upgraded in external shell). */
            if (mcode_cli_updates_available_count(&app->cli_updates) > 0 &&
                (now - app->cli_updates_local_recheck_at) >= 15.0) {
                mcode_cli_updates_check_all(&app->cli_updates, &app->cli_status, true);
                app->cli_updates_local_recheck_at = now;
            }
        }
    }

    /* Refresh CLI availability periodically when settings dialog closes or every 30s */
    if (now - app->cli_checked_at > 30.0) {
        mcode_cli_status prev_clis = app->cli_status;
        mcode_agent_detect_clis(app_chrome_settings(app->chrome), &app->cli_status);
        app->cli_checked_at = now;
        if (app->cli_status.available[APP_AGENT_AGY] &&
            (!prev_clis.available[APP_AGENT_AGY] || !app->models.loaded_agy)) {
            mcode_model_catalog_refresh(&app->models, &app->cli_status);
        }
        if (!(app->chrome && app->chrome->env_screenshot_mode)) {
            for (int i = 0; i < APP_AGENT_COUNT; ++i) {
                bool avail = app->cli_status.available[i];
                bool had = prev_clis.available[i];
                bool path_changed = (strcmp(app->cli_status.resolved_path[i],
                                            prev_clis.resolved_path[i]) != 0);
                if (avail && !had) {
                    mcode_usage_tracker_refresh_agent(&app->usage, &app->cli_status,
                                                      (app_agent_kind)i, agy_busy);
                }
                if (avail != had || path_changed) {
                    mcode_cli_updates_check_agent(&app->cli_updates, &app->cli_status,
                                                  (app_agent_kind)i, false);
                }
            }
        }
    }

    /* Refresh provider usage quotas periodically (every 5 minutes) */
    if (!(app->chrome && app->chrome->env_screenshot_mode) &&
        (now - app->usage_checked_at) >= 300.0) {
        mcode_usage_tracker_refresh(&app->usage, &app->cli_status, agy_busy);
        app->usage_checked_at = now;
    }

    /* Re-check provider CLI updates periodically (every 4 hours) */
    if (!(app->chrome && app->chrome->env_screenshot_mode) &&
        (now - app->cli_updates_checked_at) >= 14400.0) {
        mcode_cli_updates_check_all(&app->cli_updates, &app->cli_status, false);
        app->cli_updates_checked_at = now;
    }

    /* Debounced atomic save of workspace state */
    if (app->state_dirty && (now - app->state_dirty_since) >= 0.45 && app->state_path[0]) {
        mcode_store_save(&app->store, app->state_path);
        app->state_dirty = false;
    }

    if (out_running_count) *out_running_count = running;
}

static void draw_status_bar(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int running_count) {
    cui_box(ui, .width = CUI_FILL, .bg = mcode_ui_chrome_bg(ui)) {
        cui_status_bar(ui, "mcode-status") {
            mcode_ui_draw_status_usage(app, ui, thread);

            cui_status_bar_spacer(ui);

            if (running_count > 0) {
                char run_buf[48];
                snprintf(run_buf, sizeof run_buf, "%d running###sb-run", running_count);
                cui_status_bar_text(ui, run_buf, .icon = CUI_ICON_ACTIVITY, .tone = CUI_TONE_INFO);
            }

            mcode_ui_draw_status_cli_updates(app, ui);
        }
    }
}

enum {
    TOPBAR_ICON_SIDEBAR_OPEN = 0,
    TOPBAR_ICON_SIDEBAR_CLOSED,
    TOPBAR_ICON_ACTIONS,
    TOPBAR_ICON_BOTTOM_PANEL,
    TOPBAR_ICON_RIGHT_PANEL,
};

typedef struct {
    int kind;
    bool active;
    bool hovered;
    bool pressed;
} topbar_icon_paint;

static void paint_topbar_icon_btn(cui_ctx *ui, cui_rect r, void *user) {
    const topbar_icon_paint *p = (const topbar_icon_paint *)user;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    if (p->active) {
        cui_color fill = cui_alpha(fg, dark ? (p->hovered ? 0.13f : 0.09f)
                                            : (p->hovered ? 0.10f : 0.07f));
        cui_color bdr = cui_alpha(fg, dark ? 0.12f : 0.10f);
        cui_draw_rect_border(ui, r, CUI_CORNERS(7.0f), fill, bdr, 1.0f);
    } else if (p->pressed || p->hovered) {
        cui_color fill = cui_alpha(fg, p->pressed ? 0.10f : 0.06f);
        cui_draw_rect(ui, r, CUI_CORNERS(7.0f), fill);
    }

    cui_color stroke = p->active
        ? fg
        : (p->hovered ? cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY)
                      : cui_color_of(ui, CUI_COLOR_TEXT_MUTED));
    cui_color clear = { 0, 0, 0, 0 };
    float t = 1.35f;

    if (p->kind == TOPBAR_ICON_ACTIONS) {
        float w = 14.0f, h = 14.0f;
        float x = floorf(r.x + (r.w - w) * 0.5f) + 0.5f;
        float y = floorf(r.y + (r.h - h) * 0.5f) + 0.5f;
        cui_draw_rect_border(ui, (cui_rect){ x, y, w, h }, CUI_CORNERS(3.2f), clear, stroke, t);
        cui_draw_line(ui, (cui_vec2){ x + 3.4f, y + 4.2f }, (cui_vec2){ x + w - 3.4f, y + 4.2f }, t, stroke);
        cui_draw_line(ui, (cui_vec2){ x + 3.4f, y + 7.0f }, (cui_vec2){ x + w - 3.4f, y + 7.0f }, t, stroke);
        cui_draw_line(ui, (cui_vec2){ x + 3.4f, y + 9.8f }, (cui_vec2){ x + w - 3.4f, y + 9.8f }, t, stroke);
        return;
    }

    float w = 15.0f, h = 13.0f;
    float x = floorf(r.x + (r.w - w) * 0.5f) + 0.5f;
    float y = floorf(r.y + (r.h - h) * 0.5f) + 0.5f;
    cui_draw_rect_border(ui, (cui_rect){ x, y, w, h }, CUI_CORNERS(3.0f), clear, stroke, t);

    if (p->kind == TOPBAR_ICON_SIDEBAR_OPEN || p->kind == TOPBAR_ICON_SIDEBAR_CLOSED) {
        float sx = x + 4.8f;
        cui_draw_line(ui, (cui_vec2){ sx, y + 1.0f }, (cui_vec2){ sx, y + h - 1.0f }, t, stroke);
        float cy = y + h * 0.5f;
        if (p->kind == TOPBAR_ICON_SIDEBAR_OPEN) {
            cui_vec2 chev[3] = {
                { x + 10.8f, cy - 2.4f },
                { x + 8.3f,  cy },
                { x + 10.8f, cy + 2.4f },
            };
            cui_draw_polyline(ui, chev, 3, 1.25f, stroke, false);
        } else {
            cui_vec2 chev[3] = {
                { x + 8.6f,  cy - 2.4f },
                { x + 11.1f, cy },
                { x + 8.6f,  cy + 2.4f },
            };
            cui_draw_polyline(ui, chev, 3, 1.25f, stroke, false);
        }
    } else if (p->kind == TOPBAR_ICON_BOTTOM_PANEL) {
        float sy = y + h - 4.2f;
        cui_draw_line(ui, (cui_vec2){ x + 1.0f, sy }, (cui_vec2){ x + w - 1.0f, sy }, t, stroke);
    } else if (p->kind == TOPBAR_ICON_RIGHT_PANEL) {
        float sx = x + w - 4.8f;
        cui_draw_line(ui, (cui_vec2){ sx, y + 1.0f }, (cui_vec2){ sx, y + h - 1.0f }, t, stroke);
    }
}

static bool draw_topbar_icon_btn(cui_ctx *ui, const char *id, int kind, bool active,
                                 const char *tooltip, const char *shortcut) {
    cui_id cid = cui_make_id(ui, id);
    cui_rect prev = cui_get_rect(ui, cid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = prev.w > 0.0f &&
               mp.x >= prev.x && mp.x <= prev.x + prev.w &&
               mp.y >= prev.y && mp.y <= prev.y + prev.h;
    bool prs = hov && cui_mouse_down(ui, CUI_MOUSE_LEFT);
    topbar_icon_paint spec = { kind, active, hov, prs };
    cui_canvas_result cr = cui_canvas(ui, id, CUI_PX(28), CUI_PX(28),
                                      paint_topbar_icon_btn, &spec, sizeof spec);
    if (tooltip && tooltip[0]) {
        if (shortcut && shortcut[0]) {
            char tip[160];
            snprintf(tip, sizeof tip, "%s  (%s)", tooltip, shortcut);
            cui_tooltip(ui, tip);
        } else {
            cui_tooltip(ui, tooltip);
        }
    }
    return cr.clicked;
}

static void handle_topbar_drag_zone(mcode_app *app, cui_ctx *ui, const char *id,
                                    cui_size width) {
    cui_canvas_result cr = cui_canvas(ui, id, width, CUI_FILL, NULL, NULL, 0);
    if (cr.hovered && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) {
        void *native = cui_app_ui(NULL) ? cui_app_native_window(NULL) : NULL;
        const char *backend = cui_app_ui(NULL) ? cui_app_backend(NULL) : NULL;
        double now = cui_time(ui);
        if (app->titlebar_last_click_at > 0.0 && (now - app->titlebar_last_click_at) < 0.32) {
            app->titlebar_last_click_at = 0.0;
            app_window_titlebar_double_click(native, backend);
        } else {
            app->titlebar_last_click_at = now;
            app_window_start_drag(native, backend);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        }
    }
}

static cui_color project_badge_color(const char *name) {
    static const uint32_t k_palette[] = {
        0x0099b8ffu, /* cyan / teal */
        0x2f7cf6ffu, /* blue */
        0x7c5cf6ffu, /* violet */
        0x159e6dffu, /* emerald */
        0xd98218ffu, /* amber */
        0xd9467bffu, /* rose */
        0x4f66f0ffu, /* indigo */
    };
    uint32_t h = 2166136261u;
    if (name) {
        for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
            h ^= (uint32_t)(*p);
            h *= 16777619u;
        }
    }
    return CUI_RGBA(k_palette[h % (sizeof k_palette / sizeof k_palette[0])]);
}

void mcode_ui_draw_project_badge(cui_ctx *ui, const char *name, float size) {
    char letter[2] = "P";
    if (name) {
        for (const char *p = name; *p; ++p) {
            if (isalnum((unsigned char)*p)) {
                letter[0] = (char)toupper((unsigned char)*p);
                break;
            }
        }
    }
    cui_box(ui, .width = CUI_PX(size), .height = CUI_PX(size), .radius = size * 0.5f,
            .bg = project_badge_color(name),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_text(ui, letter, .font = CUI_FONT_MEDIUM, .size = size * 0.56f,
                 .color = CUI_RGBA(0xffffffffu), .nowrap = true);
    }
}

static void draw_topbar_left_controls(mcode_app *app, cui_ctx *ui, bool fill_width) {
    void *native = cui_app_ui(NULL) ? cui_app_native_window(NULL) : NULL;
    const char *backend = cui_app_ui(NULL) ? cui_app_backend(NULL) : NULL;
    float left_inset = app_window_titlebar_left_inset(native, backend);
    float pad_left = left_inset > 10.0f ? left_inset : 10.0f;

    int prev_idx = nav_history_find_prev(app);
    int next_idx = nav_history_find_next(app);

    cui_row(ui, .width = fill_width ? CUI_FILL : CUI_FIT, .height = CUI_PX(40),
            .pad_left = pad_left, .pad_right = 8, .gap = 4,
            .align = CUI_ALIGN_CENTER) {
        if (draw_topbar_icon_btn(ui, "tb-sidebar",
                                 app->store.show_sidebar ? TOPBAR_ICON_SIDEBAR_OPEN
                                                         : TOPBAR_ICON_SIDEBAR_CLOSED,
                                 false,
                                 app->store.show_sidebar ? "Hide Sidebar" : "Show Sidebar",
                                 "Cmd B")) {
            mcode_app_action_toggle_sidebar(ui, app);
        }

        cui_row(ui, .pad_left = 4, .pad_right = 4, .gap = 0, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "M", .font = CUI_FONT_MEDIUM, .size = 13.5f,
                     .color = CUI_COLOR_FOREGROUND, .nowrap = true);
            cui_text(ui, "code", .font = CUI_FONT_BODY, .size = 13.5f,
                     .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        }

        if (fill_width) {
            handle_topbar_drag_zone(app, ui, "tb-drag-left", CUI_FILL);
        } else {
            cui_gap(ui, 4);
        }

        cui_row(ui, .gap = 0, .align = CUI_ALIGN_CENTER) {
            if (cui_icon_button(ui, CUI_ICON_ARROW_LEFT, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .width = CUI_PX(26),
                                .disabled = (prev_idx < 0),
                                .color = (prev_idx >= 0) ? CUI_COLOR_TEXT_SECONDARY
                                                         : CUI_COLOR_TEXT_MUTED,
                                .tooltip = "Previous Thread")) {
                nav_history_jump(app, ui, prev_idx);
            }
            if (cui_icon_button(ui, CUI_ICON_ARROW_RIGHT, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .width = CUI_PX(26),
                                .disabled = (next_idx < 0),
                                .color = (next_idx >= 0) ? CUI_COLOR_TEXT_SECONDARY
                                                         : CUI_COLOR_TEXT_MUTED,
                                .tooltip = "Next Thread")) {
                nav_history_jump(app, ui, next_idx);
            }
        }
    }
}

static bool draw_actions_pop_item(cui_ctx *ui, const char *id, const char *icon,
                                  const char *label, const char *shortcut, bool danger) {
    cui_id cid = cui_make_id(ui, id);
    cui_rect r = cui_get_rect(ui, cid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = r.w > 0.0f && mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    cui_color bg = hov
        ? cui_alpha(cui_color_of(ui, danger ? CUI_COLOR_DANGER : CUI_COLOR_FOREGROUND), 0.08f)
        : CUI_RGBA(0x00000001u);
    cui_color fg = danger ? CUI_COLOR_DANGER : CUI_COLOR_FOREGROUND;
    cui_color ic = danger ? CUI_COLOR_DANGER : CUI_COLOR_TEXT_SECONDARY;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = id,
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_FILL,
        .pad_x = 9,
        .pad_y = 6,
        .gap = 8,
        .hoverable = true,
        .border = -1.0f,
        .radius = 6,
        .bg = bg,
    });
    if (icon && icon[0]) {
        cui_icon(ui, icon, 14, ic);
    }
    cui_text(ui, label, .size = 12.5f, .color = fg, .width = CUI_FILL, .nowrap = true);
    if (shortcut && shortcut[0]) {
        cui_text(ui, shortcut, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
    }
    cui_card_end(ui);
    return clicked;
}

static void draw_topbar_right_controls(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                       bool fill_width) {
    mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }

    cui_row(ui, .width = fill_width ? CUI_FILL : CUI_FIT, .height = CUI_PX(40),
            .pad_left = fill_width ? 8 : 0,
            .pad_right = fill_width ? 10 : 0,
            .gap = 4, .align = CUI_ALIGN_CENTER) {
        if (fill_width) {
            handle_topbar_drag_zone(app, ui, "tb-drag-right", CUI_FILL);
        }

        bool actions_open = cui_popover_is_open(ui, "topbar-actions-pop");
        if (draw_topbar_icon_btn(ui, "tb-actions", TOPBAR_ICON_ACTIONS, actions_open,
                                 thread ? "Thread & Git Actions" : "Workspace Actions", NULL)) {
            cui_popover_toggle(ui, "topbar-actions-pop");
        }

        bool close_actions = false;
        cui_popover(ui, "topbar-actions-pop", .placement = CUI_PLACE_BOTTOM_END,
                    .width = 252, .pad = 6, .gap = 2) {
            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                if (thread) {
                    if (thread->branch[0] || thread->diff_file_count > 0) {
                        cui_row(ui, .width = CUI_FILL, .pad_x = 9, .pad_y = 6, .gap = 6,
                                .align = CUI_ALIGN_CENTER, .wrap = true) {
                            if (thread->branch[0]) {
                                cui_badge(ui, thread->branch, .icon = CUI_ICON_GIT_BRANCH,
                                          .tone = CUI_TONE_NEUTRAL,
                                          .size = CUI_SIZE_SM);
                            }
                            if (thread->diff_file_count > 0) {
                                char dstat[48];
                                snprintf(dstat, sizeof dstat, "+%d -%d",
                                         thread->total_added, thread->total_removed);
                                cui_text(ui, dstat, .font = CUI_FONT_MONO, .size = 11.0f,
                                         .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
                            }
                        }
                        cui_separator(ui);
                    }

                    if (proj && proj->is_git_repo) {
                        if (draw_actions_pop_item(ui, "act-commit", CUI_ICON_CHECK,
                                                  "Commit Changes...", "Cmd Shift C", false)) {
                            mcode_app_action_commit(ui, app);
                            close_actions = true;
                        }
                        if (app->git_sync.valid && strcmp(app->git_workdir, thread->workdir) == 0 &&
                            !mcode_ui_is_git_op_running(app, thread->workdir)) {
                            if (!app->git_sync.has_upstream && !app->git_sync.is_unborn &&
                                !app->git_sync.is_detached && app->git_sync.has_remote) {
                                if (draw_actions_pop_item(ui, "act-publish", CUI_ICON_UPLOAD,
                                                          "Publish Branch", NULL, false)) {
                                    mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_PUBLISH,
                                                          thread->workdir, false);
                                    close_actions = true;
                                }
                            } else if (app->git_sync.has_upstream &&
                                       (app->git_sync.ahead > 0 || app->git_sync.behind > 0)) {
                                char sync_lbl[64];
                                if (app->git_sync.behind > 0 && app->git_sync.ahead > 0) {
                                    snprintf(sync_lbl, sizeof sync_lbl,
                                             "Sync Changes %d\xe2\x86\x93 %d\xe2\x86\x91",
                                             app->git_sync.behind, app->git_sync.ahead);
                                } else if (app->git_sync.behind > 0) {
                                    snprintf(sync_lbl, sizeof sync_lbl,
                                             "Sync Changes %d\xe2\x86\x93", app->git_sync.behind);
                                } else {
                                    snprintf(sync_lbl, sizeof sync_lbl,
                                             "Sync Changes %d\xe2\x86\x91", app->git_sync.ahead);
                                }
                                if (draw_actions_pop_item(ui, "act-sync", CUI_ICON_REFRESH_CW,
                                                          sync_lbl, NULL, false)) {
                                    mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_SYNC,
                                                          thread->workdir, false);
                                    close_actions = true;
                                }
                            }
                        }
                        if (draw_actions_pop_item(ui, "act-pr", CUI_ICON_ARROW_UP_RIGHT,
                                                  "Push & Create PR...", "Cmd Shift P", false)) {
                            mcode_app_action_create_pr(ui, app);
                            close_actions = true;
                        }
                        if (draw_actions_pop_item(ui, "act-rdiff", CUI_ICON_REFRESH_CW,
                                                  "Refresh Diff", "Cmd R", false)) {
                            mcode_app_action_refresh_diff(ui, app);
                            close_actions = true;
                        }
                    } else if (proj && !proj->is_git_repo) {
                        if (draw_actions_pop_item(ui, "act-init-git", CUI_ICON_GIT_BRANCH,
                                                  "Initialize Git Repository", NULL, false)) {
                            char pid[MCODE_ID_MAX];
                            snprintf(pid, sizeof pid, "%s", proj->id);
                            mcode_ui_init_project_git(app, ui, proj);
                            proj = mcode_store_find_project(&app->store, pid);
                            close_actions = true;
                        }
                    }

                    if (draw_actions_pop_item(ui, "act-finder", CUI_ICON_FOLDER_OPEN,
                                              "Open Folder in Finder", "Cmd Shift F", false)) {
                        mcode_app_action_open_folder(ui, app);
                        close_actions = true;
                    }
                    if (!thread->is_draft) {
                        if (thread->state != MCODE_THREAD_SETTLED) {
                            if (draw_actions_pop_item(ui, "act-settle", CUI_ICON_CIRCLE_CHECK,
                                                      "Mark Thread Settled", NULL, false)) {
                                thread->state = MCODE_THREAD_SETTLED;
                                mcode_ui_mark_dirty(app);
                                close_actions = true;
                            }
                        } else {
                            if (draw_actions_pop_item(ui, "act-reopen", CUI_ICON_INBOX,
                                                      "Reopen Thread", NULL, false)) {
                                thread->state = MCODE_THREAD_ACTIVE;
                                mcode_ui_mark_dirty(app);
                                close_actions = true;
                            }
                        }
                    }
                    if (proj) {
                        if (draw_actions_pop_item(ui, "act-qa", CUI_ICON_SLIDERS_HORIZONTAL,
                                                  "Quick Actions...", NULL, false)) {
                            mcode_ui_open_project_actions_for(app, ui, proj->id);
                            close_actions = true;
                        }
                    }
                    cui_separator(ui);
                }

                if (draw_actions_pop_item(ui, "act-palette", CUI_ICON_COMMAND,
                                          "Command Palette...", "Cmd K", false)) {
                    app->chrome->palette_open = true;
                    close_actions = true;
                }
                if (draw_actions_pop_item(ui, "act-settings", CUI_ICON_SETTINGS,
                                          "Settings...", "Cmd ,", false)) {
                    app->chrome->settings_open = true;
                    app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
                    close_actions = true;
                }
            }
        }
        if (close_actions) {
            cui_popover_close(ui, "topbar-actions-pop");
        }

        if (draw_topbar_icon_btn(ui, "tb-terminal", TOPBAR_ICON_BOTTOM_PANEL,
                                 app->store.show_terminal_panel,
                                 "Toggle Terminal", "Cmd J")) {
            mcode_app_action_toggle_terminal(ui, app);
        }

        char diff_tip[80];
        if (thread && thread->diff_file_count > 0) {
            snprintf(diff_tip, sizeof diff_tip, "Toggle Changes & Files (%d %s)",
                     thread->diff_file_count,
                     thread->diff_file_count == 1 ? "change" : "changes");
        } else {
            snprintf(diff_tip, sizeof diff_tip, "Toggle Changes & Files");
        }
        bool has_project = (thread != NULL || app->store.project_count > 0);
        if (draw_topbar_icon_btn(ui, "tb-diff", TOPBAR_ICON_RIGHT_PANEL,
                                 app->store.show_diff_panel && has_project,
                                 diff_tip, "Cmd D")) {
            mcode_app_action_toggle_diff(ui, app);
        }
    }
}

static void draw_topbar_main_segment(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                     bool include_left_controls,
                                     bool include_right_controls) {
    mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }

    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(40),
            .bg = CUI_COLOR_BACKGROUND,
            .pad_left = include_left_controls ? 0 : 14,
            .pad_right = include_right_controls ? 10 : 8,
            .gap = 0, .align = CUI_ALIGN_CENTER) {
        if (include_left_controls) {
            draw_topbar_left_controls(app, ui, false);
            thread = mcode_store_selected_thread(&app->store);
            proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
            if (!proj && app->store.selected_project_id[0]) {
                proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
            }
            cui_box(ui, .width = CUI_PX(1), .height = CUI_PX(20),
                    .bg = CUI_COLOR_BORDER) {}
            cui_gap(ui, 12);
        }

        /* Breadcrumb: [Project Badge] project / Thread title */
        if (thread) {
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .max_width = 620, .clip = true) {
                if (proj) {
                    mcode_ui_draw_project_badge(ui, proj->name, 18.0f);
                    cui_text(ui, proj->name, .font = CUI_FONT_BODY, .size = 13.0f,
                             .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                    char tip[MCODE_PATH_MAX + 64];
                    mcode_git_shorten_home_path(proj->path, tip, sizeof tip);
                    cui_tooltip(ui, tip);
                    cui_text(ui, "/", .font = CUI_FONT_BODY, .size = 13.0f,
                             .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                }

                cui_box(ui, .max_width = 420, .clip = true) {
                    if (cui_inline_edit(ui, "thread-title", thread->title, sizeof thread->title,
                                        .font = CUI_FONT_MEDIUM, .size = 13.0f,
                                        .placeholder = "Untitled thread") & CUI_CHANGED) {
                        thread->title_auto_fallback[0] = '\0';
                        thread->title_pending = false;
                        if (thread->title_run) {
                            mcode_agent_title_stop(thread->title_run);
                        }
                        if (!thread->is_draft) {
                            mcode_ui_promote_project(app, thread->project_id);
                            mcode_ui_mark_dirty(app);
                        }
                    }
                }

                if (!thread->active_run && thread->state == MCODE_THREAD_SETTLED) {
                    cui_badge(ui, "Settled", .tone = CUI_TONE_SUCCESS, .icon = CUI_ICON_CHECK,
                              .size = CUI_SIZE_SM);
                }
            }
        } else if (proj) {
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                mcode_ui_draw_project_badge(ui, proj->name, 18.0f);
                cui_text(ui, proj->name, .font = CUI_FONT_BODY, .size = 13.0f,
                         .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
            }
        }

        /* Draggable empty space */
        handle_topbar_drag_zone(app, ui, "tb-drag-main", CUI_FILL);

        /* Right-side 3-icon cluster when right sidebar is hidden */
        if (include_right_controls) {
            draw_topbar_right_controls(app, ui, thread, false);
        }
    }
}

static void draw_center_workspace_body(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                       bool diff_visible) {
    if (app->store.show_terminal_panel) {
        cui_split(ui, diff_visible ? "mcode-term-split" : "mcode-term-split-wide",
                  &app->store.terminal_ratio,
                  .vertical = true, .min_first = 220, .min_second = 150,
                  .width = CUI_FILL, .height = CUI_FILL) {
            mcode_ui_draw_workspace_main(app, ui, thread);
            cui_split_next(ui);
            mcode_ui_draw_terminal(app, ui, thread);
        }
    } else {
        mcode_ui_draw_workspace_main(app, ui, thread);
    }
}

static void draw_workspace_area(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                bool include_left_controls, cui_color sidebar_bg) {
    bool has_project = (thread != NULL || app->store.project_count > 0);
    if (app->store.show_diff_panel && has_project) {
        cui_split(ui, "mcode-diff-split", &app->store.diff_ratio,
                  .min_first = 360, .min_second = 248,
                  .width = CUI_FILL, .height = CUI_FILL) {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
                draw_topbar_main_segment(app, ui, thread, include_left_controls, false);
                thread = mcode_store_selected_thread(&app->store);
                draw_center_workspace_body(app, ui, thread, true);
            }
            cui_split_next(ui);
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .bg = sidebar_bg, .gap = 0) {
                thread = mcode_store_selected_thread(&app->store);
                draw_topbar_right_controls(app, ui, thread, true);
                mcode_ui_draw_diff(app, ui, thread);
            }
        }
    } else {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
            draw_topbar_main_segment(app, ui, thread, include_left_controls, true);
            thread = mcode_store_selected_thread(&app->store);
            draw_center_workspace_body(app, ui, thread, false);
        }
    }
}

void mcode_app_frame(mcode_app *app, cui_ctx *ui) {
    if (!app || !ui) return;
    int running_count = 0;
    poll_background_work(app, ui, &running_count);

    mcode_thread *thread = mcode_store_selected_thread(&app->store);

    /* Keep menu checkmarks in sync */
    app_chrome_set_action_checked(app->chrome, "toggle_sidebar", app->store.show_sidebar);
    app_chrome_set_action_checked(app->chrome, "toggle_diff", app->store.show_diff_panel);
    app_chrome_set_action_checked(app->chrome, "toggle_terminal", app->store.show_terminal_panel);
    app_chrome_set_action_enabled(app->chrome, "commit", thread != NULL);
    app_chrome_set_action_enabled(app->chrome, "create_pr", thread != NULL);
    app_chrome_set_action_enabled(app->chrome, "refresh_diff", thread != NULL);

    cui_box(ui, .id = "mcode-root-stack", .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL) {
                cui_color sidebar_bg = mcode_ui_chrome_bg(ui);
                if (app->store.show_sidebar) {
                    cui_split(ui, "mcode-main-split", &app->store.sidebar_ratio,
                              .min_first = 248, .min_second = 420,
                              .width = CUI_FILL, .height = CUI_FILL) {
                        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                                   .bg = sidebar_bg, .gap = 0) {
                            draw_topbar_left_controls(app, ui, true);
                            mcode_ui_draw_sidebar(app, ui);
                        }
                        cui_split_next(ui);

                        thread = mcode_store_selected_thread(&app->store);
                        draw_workspace_area(app, ui, thread, false, sidebar_bg);
                    }
                } else {
                    draw_workspace_area(app, ui, thread, true, sidebar_bg);
                }
            }
            thread = mcode_store_selected_thread(&app->store);
            draw_status_bar(app, ui, thread, running_count);
        }

        mcode_ui_draw_file_context_menu(app, ui);
    }

    mcode_ui_draw_dialogs(app, ui);
}

/* ---- Actions & Command Palette ---- */

void mcode_app_action_add_project(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
}

void mcode_app_action_new_thread(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    if (app->store.project_count == 0) {
        mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
        return;
    }
    mcode_thread *sel = mcode_store_selected_thread(&app->store);
    mcode_project *proj = sel ? mcode_store_find_project(&app->store, sel->project_id) : NULL;
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }
    if (!proj) proj = &app->store.projects[0];
    mcode_ui_quick_new_thread(app, ui, proj);
}

void mcode_app_action_toggle_sidebar(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->store.show_sidebar = !app->store.show_sidebar;
    mcode_ui_mark_dirty(app);
}

void mcode_app_action_toggle_diff(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->store.show_diff_panel = !app->store.show_diff_panel;
    if (app->store.show_diff_panel) {
        mcode_ui_refresh_workspace_git(app);
    }
    mcode_ui_mark_dirty(app);
}

void mcode_app_action_toggle_terminal(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->store.show_terminal_panel = !app->store.show_terminal_panel;
    if (app->store.show_terminal_panel) {
        app->composer_want_focus = false;
    }
    app->term_want_focus = app->store.show_terminal_panel;
    mcode_ui_mark_dirty(app);
    if (ui) cui_request_redraw(ui);
}

void mcode_app_action_commit(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (!t) {
        if (ui) app_chrome_notify(app->chrome, ui, "Select a thread first.", CUI_TONE_WARNING);
        return;
    }
    mcode_ui_refresh_thread_diff(t);
    app->git_dialog_is_pr = false;
    snprintf(app->commit_msg, sizeof app->commit_msg, "%s", t->title);
    snprintf(app->pr_title, sizeof app->pr_title, "%s", t->title);
    snprintf(app->pr_body, sizeof app->pr_body, "Automated changes from Mcode thread: %s", t->title);
    app->git_dialog_open = true;
}

void mcode_app_action_create_pr(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (!t) {
        if (ui) app_chrome_notify(app->chrome, ui, "Select a thread first.", CUI_TONE_WARNING);
        return;
    }
    mcode_ui_refresh_thread_diff(t);
    app->git_dialog_is_pr = true;
    snprintf(app->commit_msg, sizeof app->commit_msg, "%s", t->title);
    snprintf(app->pr_title, sizeof app->pr_title, "%s", t->title);
    snprintf(app->pr_body, sizeof app->pr_body, "Created with Mcode on branch `%s`.", t->branch);
    app->git_dialog_open = true;
}

void mcode_app_action_refresh_diff(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_ui_refresh_workspace_git(app);
    if (ui && (mcode_store_selected_thread(&app->store) || app->store.project_count > 0)) {
        app_chrome_notify(app->chrome, ui, "Changes and files refreshed.", CUI_TONE_INFO);
    }
}

void mcode_app_action_project_actions(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_ui_open_project_actions_for(app, ui, NULL);
}

void mcode_app_action_open_folder(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    const char *dir = (t && t->workdir[0]) ? t->workdir : NULL;
    if (!dir && app->store.selected_project_id[0]) {
        mcode_project *p = mcode_store_find_project(&app->store, app->store.selected_project_id);
        if (p) dir = p->path;
    }
    if (!dir && app->store.project_count > 0) dir = app->store.projects[0].path;
    if (!dir || !dir[0]) {
        if (ui) app_chrome_notify(app->chrome, ui, "No project folder selected.", CUI_TONE_WARNING);
        return;
    }
    if (!cui_app_open_url(dir) && ui) {
        app_chrome_notify(app->chrome, ui, "Could not open folder.", CUI_TONE_DANGER);
    }
}

typedef struct {
    mcode_app *app;
    int index;
} palette_target;

static palette_target s_project_targets[64];
static palette_target s_thread_targets[64];
static palette_target s_action_targets[MCODE_MAX_QUICK_ACTIONS];

static void on_palette_new_thread_in_project(cui_ctx *ui, void *user) {
    palette_target *pt = (palette_target *)user;
    if (!pt || !pt->app || pt->index < 0 || pt->index >= pt->app->store.project_count) return;
    mcode_ui_quick_new_thread(pt->app, ui, &pt->app->store.projects[pt->index]);
}

static void on_palette_create_project(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (app) mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CREATE);
}

static void on_palette_clone_project(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (app) mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CLONE);
}

static void on_palette_select_thread(cui_ctx *ui, void *user) {
    palette_target *pt = (palette_target *)user;
    if (!pt || !pt->app || pt->index < 0 || pt->index >= pt->app->store.thread_count) return;
    mcode_ui_select_thread(pt->app, pt->app->store.threads[pt->index].id);
    if (ui) cui_request_redraw(ui);
}

static void on_palette_run_quick_action(cui_ctx *ui, void *user) {
    palette_target *pt = (palette_target *)user;
    if (!pt || !pt->app) return;
    mcode_thread *t = mcode_store_selected_thread(&pt->app->store);
    mcode_project *p = t ? mcode_store_find_project(&pt->app->store, t->project_id) : NULL;
    if (!p && pt->app->store.project_count > 0) p = &pt->app->store.projects[0];
    if (!p || pt->index < 0 || pt->index >= p->action_count) return;
    const char *cwd = (t && t->workdir[0]) ? t->workdir : p->path;
    mcode_ui_run_terminal_cmd(pt->app, ui, p->actions[pt->index].command, cwd);
}

static void on_palette_refresh_usage(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    bool agy_busy = false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->agent == APP_AGENT_AGY && (t->active_run || t->title_run)) {
            agy_busy = true;
            break;
        }
    }
    mcode_usage_tracker_refresh(&app->usage, &app->cli_status, agy_busy);
    app->usage_checked_at = mcode_now_mono();
    if (ui) app_chrome_notify(app->chrome, ui, "Refreshing usage limits...", CUI_TONE_INFO);
}

static void on_palette_open_codex_resets(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->usage_tab = 0;
    app->usage_resets_expanded = true;
    app->usage_open_requested = true;
    if (ui) cui_request_redraw(ui);
}

static void on_palette_check_cli_updates(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_agent_detect_clis(app_chrome_settings(app->chrome), &app->cli_status);
    app->cli_checked_at = mcode_now_mono();
    mcode_cli_updates_check_all(&app->cli_updates, &app->cli_status, false);
    app->cli_updates_checked_at = mcode_now_mono();
    app->cli_updates_manual_check_pending = true;
    if (ui) app_chrome_notify(app->chrome, ui, "Checking for provider CLI updates...", CUI_TONE_INFO);
}

static void on_palette_open_cli_updates(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->cli_updates_open_requested = true;
    if (ui) cui_request_redraw(ui);
}

static void on_palette_send_next_queued(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t && !t->active_run && t->queued_count > 0) {
        mcode_ui_send_next_queued(app, ui, t);
    }
}

static void on_palette_clear_queue(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t && t->queued_count > 0) {
        mcode_thread_queue_clear(t);
        t->queue_paused = false;
        mcode_ui_mark_dirty(app);
        if (ui) app_chrome_notify(app->chrome, ui, "Cleared queued messages.", CUI_TONE_INFO);
    }
}

static void on_palette_toggle_busy_submit(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app || !app->chrome) return;
    app->chrome->settings.mcode.busy_submit_mode =
        (app->chrome->settings.mcode.busy_submit_mode == APP_BUSY_SUBMIT_QUEUE)
            ? APP_BUSY_SUBMIT_STEER
            : APP_BUSY_SUBMIT_QUEUE;
    app_settings_save(&app->chrome->settings, app->chrome->cli.settings_path);
    if (ui) {
        const char *msg = (app->chrome->settings.mcode.busy_submit_mode == APP_BUSY_SUBMIT_STEER)
            ? "Enter while busy now steers immediately."
            : "Enter while busy now queues follow-ups.";
        app_chrome_notify(app->chrome, ui, msg, CUI_TONE_INFO);
    }
}

static void on_palette_git_sync(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app || !app->git_workdir[0]) return;
    if (app->git_sync.valid && !app->git_sync.has_upstream &&
        !app->git_sync.is_unborn && !app->git_sync.is_detached && app->git_sync.has_remote) {
        mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_PUBLISH, app->git_workdir, false);
    } else {
        mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_SYNC, app->git_workdir, false);
    }
}

static void on_palette_git_pull(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (app && app->git_workdir[0]) {
        mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_PULL, app->git_workdir, false);
    }
}

static void on_palette_git_push(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (app && app->git_workdir[0]) {
        mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_PUSH, app->git_workdir, false);
    }
}

static void on_palette_git_fetch(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (app && app->git_workdir[0]) {
        mcode_ui_start_git_op(app, ui, MCODE_GIT_OP_FETCH, app->git_workdir, false);
    }
}

void mcode_app_palette_provider(app_palette_builder *builder, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;

    if (app->git_workdir[0] && app->git_sync.valid) {
        bool can_pub = (!app->git_sync.has_upstream && !app->git_sync.is_unborn &&
                        !app->git_sync.is_detached && app->git_sync.has_remote);
        if (can_pub) {
            app_palette_add(builder, &(app_palette_command){
                .title = "Git: Publish Branch",
                .icon = CUI_ICON_UPLOAD,
                .group = "Git",
                .on_trigger = on_palette_git_sync,
                .user = app,
            });
        } else if (app->git_sync.has_upstream) {
            char sync_title[64];
            if (app->git_sync.behind > 0 && app->git_sync.ahead > 0) {
                snprintf(sync_title, sizeof sync_title,
                         "Git: Sync Changes %d\xe2\x86\x93 %d\xe2\x86\x91",
                         app->git_sync.behind, app->git_sync.ahead);
            } else if (app->git_sync.behind > 0) {
                snprintf(sync_title, sizeof sync_title,
                         "Git: Sync Changes %d\xe2\x86\x93", app->git_sync.behind);
            } else if (app->git_sync.ahead > 0) {
                snprintf(sync_title, sizeof sync_title,
                         "Git: Sync Changes %d\xe2\x86\x91", app->git_sync.ahead);
            } else {
                snprintf(sync_title, sizeof sync_title, "Git: Sync Changes");
            }
            app_palette_add(builder, &(app_palette_command){
                .title = sync_title,
                .icon = CUI_ICON_REFRESH_CW,
                .group = "Git",
                .on_trigger = on_palette_git_sync,
                .user = app,
            });
            app_palette_add(builder, &(app_palette_command){
                .title = "Git: Pull",
                .icon = CUI_ICON_DOWNLOAD,
                .group = "Git",
                .on_trigger = on_palette_git_pull,
                .user = app,
            });
        }
        if (app->git_sync.has_remote) {
            if (!app->git_sync.is_unborn) {
                app_palette_add(builder, &(app_palette_command){
                    .title = "Git: Push",
                    .icon = CUI_ICON_ARROW_UP_RIGHT,
                    .group = "Git",
                    .on_trigger = on_palette_git_push,
                    .user = app,
                });
            }
            app_palette_add(builder, &(app_palette_command){
                .title = "Git: Fetch",
                .icon = CUI_ICON_ROTATE_CCW,
                .group = "Git",
                .on_trigger = on_palette_git_fetch,
                .user = app,
            });
        }
    }

    mcode_thread *sel_thr = mcode_store_selected_thread(&app->store);
    if (sel_thr && sel_thr->queued_count > 0) {
        if (!sel_thr->active_run) {
            app_palette_add(builder, &(app_palette_command){
                .title = "Send Next Queued Message",
                .icon = CUI_ICON_PLAY,
                .group = "Queue",
                .on_trigger = on_palette_send_next_queued,
                .user = app,
            });
        }
        app_palette_add(builder, &(app_palette_command){
            .title = "Clear Queued Messages",
            .icon = CUI_ICON_TRASH_2,
            .group = "Queue",
            .on_trigger = on_palette_clear_queue,
            .user = app,
        });
    }
    if (app->chrome) {
        const char *toggle_lbl =
            (app->chrome->settings.mcode.busy_submit_mode == APP_BUSY_SUBMIT_STEER)
                ? "Enter While Busy: Switch to Queue"
                : "Enter While Busy: Switch to Steer";
        app_palette_add(builder, &(app_palette_command){
            .title = toggle_lbl,
            .icon = CUI_ICON_ARROW_RIGHT,
            .group = "Composer",
            .on_trigger = on_palette_toggle_busy_submit,
            .user = app,
        });
    }

    if (app->cli_status.available[APP_AGENT_CLAUDE] ||
        app->cli_status.available[APP_AGENT_CODEX] ||
        app->cli_status.available[APP_AGENT_AGY]) {
        app_palette_add(builder, &(app_palette_command){
            .title = "Refresh Usage & Rate Limits",
            .icon = CUI_ICON_CHART_BAR,
            .group = "Usage",
            .on_trigger = on_palette_refresh_usage,
            .user = app,
        });
        app_palette_add(builder, &(app_palette_command){
            .title = "Check for Provider CLI Updates",
            .icon = CUI_ICON_REFRESH_CW,
            .group = "Updates",
            .on_trigger = on_palette_check_cli_updates,
            .user = app,
        });
        int cli_upd_avail = mcode_cli_updates_available_count(&app->cli_updates);
        if (cli_upd_avail > 0) {
            char title[96];
            snprintf(title, sizeof title, "Provider CLI Updates (%d available)", cli_upd_avail);
            app_palette_add(builder, &(app_palette_command){
                .title = title,
                .icon = CUI_ICON_DOWNLOAD,
                .group = "Updates",
                .on_trigger = on_palette_open_cli_updates,
                .user = app,
            });
        }
    }
    if (app->cli_status.available[APP_AGENT_CODEX] &&
        app->usage.quotas[APP_AGENT_CODEX].reset_credits_available > 0) {
        char title[96];
        long long avail = (long long)app->usage.quotas[APP_AGENT_CODEX].reset_credits_available;
        snprintf(title, sizeof title, "Codex Rate Limit Resets (%lld available)", avail);
        app_palette_add(builder, &(app_palette_command){
            .title = title,
            .icon = CUI_ICON_ROTATE_CCW,
            .group = "Usage",
            .on_trigger = on_palette_open_codex_resets,
            .user = app,
        });
    }

    app_palette_add(builder, &(app_palette_command){
        .title = "Create New Project...",
        .icon = CUI_ICON_PLUS,
        .group = "Projects",
        .on_trigger = on_palette_create_project,
        .user = app,
    });
    app_palette_add(builder, &(app_palette_command){
        .title = "Clone Git Repository...",
        .icon = CUI_ICON_DOWNLOAD,
        .group = "Projects",
        .on_trigger = on_palette_clone_project,
        .user = app,
    });

    int max_projects = app->store.project_count < 64 ? app->store.project_count : 64;
    for (int i = 0; i < max_projects; ++i) {
        const mcode_project *p = &app->store.projects[i];
        s_project_targets[i] = (palette_target){ app, i };
        char title[224];
        snprintf(title, sizeof title, "New Thread in %s", p->name);
        app_palette_add(builder, &(app_palette_command){
            .title = title,
            .icon = CUI_ICON_FOLDER_OPEN,
            .group = "Projects",
            .on_trigger = on_palette_new_thread_in_project,
            .user = &s_project_targets[i],
        });
    }

    int max_threads = app->store.thread_count < 64 ? app->store.thread_count : 64;
    for (int i = 0; i < max_threads; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->is_draft) continue;
        const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
        s_thread_targets[i] = (palette_target){ app, i };
        char title[240];
        snprintf(title, sizeof title, "%s  ·  %s (%s)",
                 t->title, p ? p->name : "Project", mcode_agent_short_label(t->agent));
        app_palette_add(builder, &(app_palette_command){
            .title = title,
            .icon = mcode_agent_icon(t->agent),
            .group = "Threads",
            .on_trigger = on_palette_select_thread,
            .user = &s_thread_targets[i],
        });
    }

    mcode_thread *sel = mcode_store_selected_thread(&app->store);
    mcode_project *proj = sel ? mcode_store_find_project(&app->store, sel->project_id) : NULL;
    if (!proj && app->store.project_count > 0) proj = &app->store.projects[0];
    if (proj) {
        for (int i = 0; i < proj->action_count && i < MCODE_MAX_QUICK_ACTIONS; ++i) {
            s_action_targets[i] = (palette_target){ app, i };
            char title[192];
            snprintf(title, sizeof title, "Run %s: %s", proj->actions[i].name, proj->actions[i].command);
            app_palette_add(builder, &(app_palette_command){
                .title = title,
                .icon = CUI_ICON_PLAY,
                .group = "Quick Actions",
                .on_trigger = on_palette_run_quick_action,
                .user = &s_action_targets[i],
            });
        }
    }
}
