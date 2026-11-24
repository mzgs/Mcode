#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_store.h"
#include "mcode_agent.h"
#include "mcode_git.h"
#include "app_log.h"
#include <yyjson.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

void mcode_store_init(mcode_store *store) {
    if (!store) return;
    memset(store, 0, sizeof *store);
    store->sidebar_ratio = 0.21f;
    store->diff_ratio = 0.74f;
    store->right_split_ratio = 0.44f;
    store->terminal_ratio = 0.72f;
    store->show_sidebar = true;
    store->show_diff_panel = true;
    store->show_terminal_panel = false;
    store->diff_inline_mode = false;
    store->settled_expanded = false;
    store->changes_expanded = true;
    store->files_expanded = true;
}

void mcode_store_free(mcode_store *store) {
    if (!store) return;
    for (int i = 0; i < store->thread_count; ++i) {
        mcode_thread_free(&store->threads[i]);
    }
    free(store->threads);
    free(store->projects);
    memset(store, 0, sizeof *store);
}

mcode_project *mcode_store_find_project(mcode_store *store, const char *id) {
    if (!store || !id || !id[0]) return NULL;
    for (int i = 0; i < store->project_count; ++i) {
        if (strcmp(store->projects[i].id, id) == 0) return &store->projects[i];
    }
    return NULL;
}

mcode_project *mcode_store_find_project_by_path(mcode_store *store, const char *path) {
    if (!store || !path || !path[0]) return NULL;
    for (int i = 0; i < store->project_count; ++i) {
        if (strcmp(store->projects[i].path, path) == 0) return &store->projects[i];
    }
    return NULL;
}

static void derive_folder_name(const char *path, char *out, size_t cap) {
    if (!path || !path[0]) {
        snprintf(out, cap, "Project");
        return;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') --len;
    const char *slash = NULL;
    for (size_t i = len; i > 0; --i) {
        if (path[i - 1] == '/') { slash = path + i; break; }
    }
    if (!slash) slash = path;
    size_t n = (size_t)((path + len) - slash);
    if (n == 0) {
        snprintf(out, cap, "Project");
    } else {
        snprintf(out, cap, "%.*s", (int)n, slash);
    }
}

static void add_default_quick_actions(mcode_project *p) {
    if (!p || p->action_count > 0) return;
    char probe[MCODE_PATH_MAX + 64];
    FILE *f = NULL;

    snprintf(probe, sizeof probe, "%s/CMakeLists.txt", p->path);
    if ((f = fopen(probe, "rb")) != NULL) {
        fclose(f);
        snprintf(p->actions[p->action_count].name, 64, "Build");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "cmake --build build --parallel");
        p->action_count++;
        snprintf(p->actions[p->action_count].name, 64, "Test");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "ctest --test-dir build --output-on-failure");
        p->action_count++;
        return;
    }

    snprintf(probe, sizeof probe, "%s/package.json", p->path);
    if ((f = fopen(probe, "rb")) != NULL) {
        fclose(f);
        snprintf(p->actions[p->action_count].name, 64, "Dev");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "npm run dev");
        p->action_count++;
        snprintf(p->actions[p->action_count].name, 64, "Test");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "npm test");
        p->action_count++;
        snprintf(p->actions[p->action_count].name, 64, "Build");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "npm run build");
        p->action_count++;
        return;
    }

    snprintf(probe, sizeof probe, "%s/Cargo.toml", p->path);
    if ((f = fopen(probe, "rb")) != NULL) {
        fclose(f);
        snprintf(p->actions[p->action_count].name, 64, "Check");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "cargo check");
        p->action_count++;
        snprintf(p->actions[p->action_count].name, 64, "Test");
        snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "cargo test");
        p->action_count++;
        return;
    }

    snprintf(p->actions[p->action_count].name, 64, "Status");
    snprintf(p->actions[p->action_count].command, MCODE_CMD_MAX, "git status -sb");
    p->action_count++;
}

mcode_project *mcode_store_move_project_to_top(mcode_store *store, const char *id) {
    if (!store || !id || !id[0]) return NULL;
    int idx = -1;
    for (int i = 0; i < store->project_count; ++i) {
        if (strcmp(store->projects[i].id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return NULL;
    if (idx > 0) {
        mcode_project tmp = store->projects[idx];
        memmove(&store->projects[1], &store->projects[0],
                (size_t)idx * sizeof store->projects[0]);
        store->projects[0] = tmp;
    }
    return &store->projects[0];
}

mcode_project *mcode_store_add_project(mcode_store *store, const char *path, const char *name) {
    if (!store || !path || !path[0]) return NULL;
    mcode_project *existing = mcode_store_find_project_by_path(store, path);
    if (existing) {
        char existing_id[MCODE_ID_MAX];
        snprintf(existing_id, sizeof existing_id, "%s", existing->id);
        return mcode_store_move_project_to_top(store, existing_id);
    }

    if (store->project_count >= store->project_cap) {
        int new_cap = store->project_cap ? store->project_cap * 2 : 8;
        mcode_project *grown = realloc(store->projects, (size_t)new_cap * sizeof *grown);
        if (!grown) return NULL;
        store->projects = grown;
        store->project_cap = new_cap;
    }

    if (store->project_count > 0) {
        memmove(&store->projects[1], &store->projects[0],
                (size_t)store->project_count * sizeof store->projects[0]);
    }
    store->project_count++;

    mcode_project *p = &store->projects[0];
    memset(p, 0, sizeof *p);
    mcode_gen_id(p->id, sizeof p->id, "proj");
    snprintf(p->path, sizeof p->path, "%s", path);
    if (name && name[0]) {
        snprintf(p->name, sizeof p->name, "%s", name);
    } else {
        derive_folder_name(path, p->name, sizeof p->name);
    }
    mcode_git_inspect_repo(p->path, &p->is_git_repo,
                           p->default_branch, sizeof p->default_branch,
                           p->remote_url, sizeof p->remote_url);
    add_default_quick_actions(p);
    return p;
}

bool mcode_store_remove_project(mcode_store *store, const char *id) {
    if (!store || !id || !id[0]) return false;
    int idx = -1;
    for (int i = 0; i < store->project_count; ++i) {
        if (strcmp(store->projects[i].id, id) == 0) { idx = i; break; }
    }
    if (idx < 0) return false;
    if (idx + 1 < store->project_count) {
        memmove(&store->projects[idx], &store->projects[idx + 1],
                (size_t)(store->project_count - idx - 1) * sizeof store->projects[0]);
    }
    store->project_count--;
    if (strcmp(store->selected_project_id, id) == 0) store->selected_project_id[0] = '\0';
    return true;
}

mcode_thread *mcode_store_find_thread(mcode_store *store, const char *id) {
    if (!store || !id || !id[0]) return NULL;
    for (int i = 0; i < store->thread_count; ++i) {
        if (strcmp(store->threads[i].id, id) == 0) return &store->threads[i];
    }
    return NULL;
}

mcode_thread *mcode_store_selected_thread(mcode_store *store) {
    if (!store) return NULL;
    return mcode_store_find_thread(store, store->selected_thread_id);
}

mcode_thread *mcode_store_add_thread(mcode_store *store, const mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool auto_approve,
                                     const char *workdir, const char *branch) {
    if (!store) return NULL;
    if (store->thread_count >= store->thread_cap) {
        int new_cap = store->thread_cap ? store->thread_cap * 2 : 16;
        mcode_thread *grown = realloc(store->threads, (size_t)new_cap * sizeof *grown);
        if (!grown) return NULL;
        store->threads = grown;
        store->thread_cap = new_cap;
    }

    mcode_thread *t = &store->threads[store->thread_count++];
    memset(t, 0, sizeof *t);
    mcode_gen_id(t->id, sizeof t->id, "thr");
    if (proj) snprintf(t->project_id, sizeof t->project_id, "%s", proj->id);
    snprintf(t->title, sizeof t->title, "%s", (title && title[0]) ? title : "New thread");
    t->agent = agent;
    if (model && model[0]) snprintf(t->model, sizeof t->model, "%s", model);
    t->auto_approve = auto_approve;
    t->perm_mode = auto_approve ? MCODE_PERM_FULL_ACCESS : MCODE_PERM_ACCEPT_EDITS;
    if (workdir && workdir[0]) snprintf(t->workdir, sizeof t->workdir, "%s", workdir);
    else if (proj) snprintf(t->workdir, sizeof t->workdir, "%s", proj->path);
    if (branch && branch[0]) snprintf(t->branch, sizeof t->branch, "%s", branch);
    else if (proj) snprintf(t->branch, sizeof t->branch, "%s", proj->default_branch);
    t->state = MCODE_THREAD_ACTIVE;
    t->created_at = mcode_now_epoch();
    t->updated_at = t->created_at;
    t->selected_diff_file = -1;
    snprintf(store->selected_thread_id, sizeof store->selected_thread_id, "%s", t->id);
    return t;
}

bool mcode_store_remove_thread(mcode_store *store, const char *id) {
    if (!store || !id || !id[0]) return false;
    int idx = -1;
    for (int i = 0; i < store->thread_count; ++i) {
        if (strcmp(store->threads[i].id, id) == 0) { idx = i; break; }
    }
    if (idx < 0) return false;
    mcode_thread_free(&store->threads[idx]);
    if (idx + 1 < store->thread_count) {
        memmove(&store->threads[idx], &store->threads[idx + 1],
                (size_t)(store->thread_count - idx - 1) * sizeof store->threads[0]);
    }
    store->thread_count--;
    if (strcmp(store->selected_thread_id, id) == 0) {
        if (store->thread_count > 0) {
            int next = idx < store->thread_count ? idx : store->thread_count - 1;
            snprintf(store->selected_thread_id, sizeof store->selected_thread_id, "%s",
                     store->threads[next].id);
        } else {
            store->selected_thread_id[0] = '\0';
        }
    }
    return true;
}

static const char *jstr_or(yyjson_val *obj, const char *key, const char *fallback) {
    yyjson_val *v = yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_str(v) ? yyjson_get_str(v) : fallback;
}

static bool jbool_or(yyjson_val *obj, const char *key, bool fallback) {
    yyjson_val *v = yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : fallback;
}

static int jint_or(yyjson_val *obj, const char *key, int fallback) {
    yyjson_val *v = yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_num(v) ? (int)yyjson_get_num(v) : fallback;
}

static double jnum_or(yyjson_val *obj, const char *key, double fallback) {
    yyjson_val *v = yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_num(v) ? yyjson_get_num(v) : fallback;
}

bool mcode_store_load(mcode_store *store, const char *path) {
    if (!store || !path || !path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    yyjson_doc *doc = yyjson_read_fp(f, YYJSON_READ_ALLOW_BOM, NULL, NULL);
    fclose(f);
    if (!doc) return false;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }

    mcode_store_free(store);
    mcode_store_init(store);

    snprintf(store->selected_project_id, sizeof store->selected_project_id, "%s",
             jstr_or(root, "selected_project_id", ""));
    snprintf(store->selected_thread_id, sizeof store->selected_thread_id, "%s",
             jstr_or(root, "selected_thread_id", ""));
    snprintf(store->default_project_dir, sizeof store->default_project_dir, "%s",
             jstr_or(root, "default_project_dir", ""));

    yyjson_val *layout = yyjson_obj_get(root, "layout");
    if (yyjson_is_obj(layout)) {
        float sr = (float)jnum_or(layout, "sidebar_ratio", store->sidebar_ratio);
        float dr = (float)jnum_or(layout, "diff_ratio", store->diff_ratio);
        float rsr = (float)jnum_or(layout, "right_split_ratio", store->right_split_ratio);
        float tr = (float)jnum_or(layout, "terminal_ratio", store->terminal_ratio);
        if (sr >= 0.10f && sr <= 0.45f) store->sidebar_ratio = sr;
        if (dr >= 0.30f && dr <= 0.88f) store->diff_ratio = dr;
        if (rsr >= 0.12f && rsr <= 0.88f) store->right_split_ratio = rsr;
        if (tr >= 0.35f && tr <= 0.88f) store->terminal_ratio = tr;
        store->show_sidebar = jbool_or(layout, "show_sidebar", store->show_sidebar);
        store->show_diff_panel = jbool_or(layout, "show_diff_panel", store->show_diff_panel);
        store->show_terminal_panel = jbool_or(layout, "show_terminal_panel", store->show_terminal_panel);
        store->diff_inline_mode = jbool_or(layout, "diff_inline_mode", store->diff_inline_mode);
        store->settled_expanded = jbool_or(layout, "settled_expanded", store->settled_expanded);
        store->changes_expanded = jbool_or(layout, "changes_expanded", store->changes_expanded);
        store->files_expanded = jbool_or(layout, "files_expanded", store->files_expanded);
    }

    yyjson_val *projects = yyjson_obj_get(root, "projects");
    if (yyjson_is_arr(projects)) {
        size_t i, max;
        yyjson_val *pv;
        yyjson_arr_foreach(projects, i, max, pv) {
            if (!yyjson_is_obj(pv)) continue;
            const char *ppath = jstr_or(pv, "path", "");
            if (!ppath[0]) continue;
            if (store->project_count >= store->project_cap) {
                int nc = store->project_cap ? store->project_cap * 2 : 8;
                mcode_project *grown = realloc(store->projects, (size_t)nc * sizeof *grown);
                if (!grown) break;
                store->projects = grown;
                store->project_cap = nc;
            }
            mcode_project *p = &store->projects[store->project_count++];
            memset(p, 0, sizeof *p);
            snprintf(p->id, sizeof p->id, "%s", jstr_or(pv, "id", ""));
            if (!p->id[0]) mcode_gen_id(p->id, sizeof p->id, "proj");
            snprintf(p->name, sizeof p->name, "%s", jstr_or(pv, "name", "Project"));
            snprintf(p->path, sizeof p->path, "%s", ppath);
            snprintf(p->default_branch, sizeof p->default_branch, "%s", jstr_or(pv, "default_branch", ""));
            snprintf(p->remote_url, sizeof p->remote_url, "%s", jstr_or(pv, "remote_url", ""));
            p->is_git_repo = jbool_or(pv, "is_git_repo", false);
            p->collapsed = jbool_or(pv, "collapsed", false);

            yyjson_val *acts = yyjson_obj_get(pv, "actions");
            if (yyjson_is_arr(acts)) {
                size_t j, jmax;
                yyjson_val *av;
                yyjson_arr_foreach(acts, j, jmax, av) {
                    if (p->action_count >= MCODE_MAX_QUICK_ACTIONS || !yyjson_is_obj(av)) break;
                    mcode_quick_action *a = &p->actions[p->action_count++];
                    snprintf(a->name, sizeof a->name, "%s", jstr_or(av, "name", "Action"));
                    snprintf(a->command, sizeof a->command, "%s", jstr_or(av, "command", ""));
                }
            }
        }
    }

    yyjson_val *threads = yyjson_obj_get(root, "threads");
    if (yyjson_is_arr(threads)) {
        size_t i, max;
        yyjson_val *tv;
        yyjson_arr_foreach(threads, i, max, tv) {
            if (!yyjson_is_obj(tv)) continue;
            if (store->thread_count >= store->thread_cap) {
                int nc = store->thread_cap ? store->thread_cap * 2 : 16;
                mcode_thread *grown = realloc(store->threads, (size_t)nc * sizeof *grown);
                if (!grown) break;
                store->threads = grown;
                store->thread_cap = nc;
            }
            mcode_thread *t = &store->threads[store->thread_count++];
            memset(t, 0, sizeof *t);
            snprintf(t->id, sizeof t->id, "%s", jstr_or(tv, "id", ""));
            if (!t->id[0]) mcode_gen_id(t->id, sizeof t->id, "thr");
            snprintf(t->project_id, sizeof t->project_id, "%s", jstr_or(tv, "project_id", ""));
            snprintf(t->title, sizeof t->title, "%s", jstr_or(tv, "title", "Thread"));
            int ag = jint_or(tv, "agent", APP_AGENT_CLAUDE);
            if (ag < 0 || ag >= APP_AGENT_COUNT) ag = APP_AGENT_CLAUDE;
            t->agent = (app_agent_kind)ag;
            snprintf(t->model, sizeof t->model, "%s", jstr_or(tv, "model", ""));
            snprintf(t->reasoning_effort, sizeof t->reasoning_effort, "%s",
                     jstr_or(tv, "reasoning_effort", ""));
            const char *spd = jstr_or(tv, "speed_mode", "");
            if (strcmp(spd, "fast") == 0) t->speed_mode = MCODE_SPEED_FAST;
            else if (strcmp(spd, "standard") == 0) t->speed_mode = MCODE_SPEED_STANDARD;
            else t->speed_mode = MCODE_SPEED_DEFAULT;
            t->auto_approve = jbool_or(tv, "auto_approve", false);
            const char *pm = jstr_or(tv, "permission_mode", "");
            if (strcmp(pm, "supervised") == 0) {
                t->perm_mode = MCODE_PERM_SUPERVISED;
                t->auto_approve = false;
            } else if (strcmp(pm, "accept_edits") == 0) {
                t->perm_mode = MCODE_PERM_ACCEPT_EDITS;
                t->auto_approve = false;
            } else if (strcmp(pm, "full_access") == 0) {
                t->perm_mode = MCODE_PERM_FULL_ACCESS;
                t->auto_approve = true;
            } else {
                t->perm_mode = t->auto_approve ? MCODE_PERM_FULL_ACCESS : MCODE_PERM_ACCEPT_EDITS;
            }
            snprintf(t->workdir, sizeof t->workdir, "%s", jstr_or(tv, "workdir", ""));
            snprintf(t->branch, sizeof t->branch, "%s", jstr_or(tv, "branch", ""));
            snprintf(t->session_id, sizeof t->session_id, "%s", jstr_or(tv, "session_id", ""));
            int st = jint_or(tv, "state", MCODE_THREAD_ACTIVE);
            if (st < MCODE_THREAD_ACTIVE || st > MCODE_THREAD_SETTLED) st = MCODE_THREAD_ACTIVE;
            t->state = (mcode_thread_state)st;
            t->created_at = (int64_t)jnum_or(tv, "created_at", 0);
            t->updated_at = (int64_t)jnum_or(tv, "updated_at", 0);
            t->selected_diff_file = -1;

            yyjson_val *turns = yyjson_obj_get(tv, "turns");
            if (yyjson_is_arr(turns)) {
                size_t ti, tmax;
                yyjson_val *trv;
                yyjson_arr_foreach(turns, ti, tmax, trv) {
                    if (!yyjson_is_obj(trv)) continue;
                    mcode_turn *turn = mcode_thread_add_turn(t, jstr_or(trv, "prompt", ""));
                    if (!turn) break;
                    turn->created_at = (int64_t)jnum_or(trv, "created_at", 0);
                    turn->duration_sec = (float)jnum_or(trv, "duration_sec", 0.0);
                    turn->failed = jbool_or(trv, "failed", false);
                    turn->input_tokens = jint_or(trv, "input_tokens", 0);
                    turn->output_tokens = jint_or(trv, "output_tokens", 0);
                    turn->cost_usd = jnum_or(trv, "cost_usd", 0.0);

                    yyjson_val *blocks = yyjson_obj_get(trv, "blocks");
                    if (yyjson_is_arr(blocks)) {
                        size_t bi, bmax;
                        yyjson_val *bv;
                        yyjson_arr_foreach(blocks, bi, bmax, bv) {
                            if (!yyjson_is_obj(bv)) continue;
                            int bk = jint_or(bv, "kind", MCODE_BLOCK_TEXT);
                            if (bk < MCODE_BLOCK_TEXT || bk > MCODE_BLOCK_ERROR) bk = MCODE_BLOCK_TEXT;
                            mcode_block *b = mcode_turn_add_block(turn, (mcode_block_kind)bk);
                            if (!b) break;
                            mcode_str_set(&b->text, jstr_or(bv, "text", ""));
                            snprintf(b->tool_id, sizeof b->tool_id, "%s", jstr_or(bv, "tool_id", ""));
                            snprintf(b->tool_name, sizeof b->tool_name, "%s", jstr_or(bv, "tool_name", ""));
                            snprintf(b->tool_detail, sizeof b->tool_detail, "%s", jstr_or(bv, "tool_detail", ""));
                            mcode_str_set(&b->input, jstr_or(bv, "input", ""));
                            mcode_str_set(&b->output, jstr_or(bv, "output", ""));
                            int bs = jint_or(bv, "status", CUI_TASK_DONE);
                            if (bs < CUI_TASK_PENDING || bs > CUI_TASK_SKIPPED) bs = CUI_TASK_DONE;
                            b->status = (cui_task_status)bs;
                            if (b->kind == MCODE_BLOCK_TOOL && b->status == CUI_TASK_FAILED &&
                                mcode_tool_is_benign_probe_failure(b->tool_name,
                                                                   mcode_str_cstr(&b->input),
                                                                   mcode_str_cstr(&b->output),
                                                                   -1)) {
                                b->status = CUI_TASK_DONE;
                            }
                            b->duration_sec = (float)jnum_or(bv, "duration_sec", 0.0);
                        }
                    }
                }
            }
        }
    }

    yyjson_doc_free(doc);
    return true;
}

bool mcode_store_save(const mcode_store *store, const char *path) {
    if (!store || !path || !path[0]) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    const char *saved_sel_id = store->selected_thread_id;
    for (int i = 0; i < store->thread_count; ++i) {
        if (strcmp(store->threads[i].id, store->selected_thread_id) == 0) {
            if (store->threads[i].is_draft) {
                saved_sel_id = "";
                for (int j = store->thread_count - 1; j >= 0; --j) {
                    if (!store->threads[j].is_draft &&
                        strcmp(store->threads[j].project_id, store->threads[i].project_id) == 0) {
                        saved_sel_id = store->threads[j].id;
                        break;
                    }
                }
            }
            break;
        }
    }

    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_str(doc, root, "selected_project_id", store->selected_project_id);
    yyjson_mut_obj_add_str(doc, root, "selected_thread_id", saved_sel_id);
    if (store->default_project_dir[0]) {
        yyjson_mut_obj_add_str(doc, root, "default_project_dir", store->default_project_dir);
    }

    yyjson_mut_val *layout = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, layout, "sidebar_ratio", store->sidebar_ratio);
    yyjson_mut_obj_add_real(doc, layout, "diff_ratio", store->diff_ratio);
    yyjson_mut_obj_add_real(doc, layout, "right_split_ratio", store->right_split_ratio);
    yyjson_mut_obj_add_real(doc, layout, "terminal_ratio", store->terminal_ratio);
    yyjson_mut_obj_add_bool(doc, layout, "show_sidebar", store->show_sidebar);
    yyjson_mut_obj_add_bool(doc, layout, "show_diff_panel", store->show_diff_panel);
    yyjson_mut_obj_add_bool(doc, layout, "show_terminal_panel", store->show_terminal_panel);
    yyjson_mut_obj_add_bool(doc, layout, "diff_inline_mode", store->diff_inline_mode);
    yyjson_mut_obj_add_bool(doc, layout, "settled_expanded", store->settled_expanded);
    yyjson_mut_obj_add_bool(doc, layout, "changes_expanded", store->changes_expanded);
    yyjson_mut_obj_add_bool(doc, layout, "files_expanded", store->files_expanded);
    yyjson_mut_obj_add_val(doc, root, "layout", layout);

    yyjson_mut_val *projects = yyjson_mut_arr(doc);
    for (int i = 0; i < store->project_count; ++i) {
        const mcode_project *p = &store->projects[i];
        yyjson_mut_val *po = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, po, "id", p->id);
        yyjson_mut_obj_add_str(doc, po, "name", p->name);
        yyjson_mut_obj_add_str(doc, po, "path", p->path);
        yyjson_mut_obj_add_str(doc, po, "default_branch", p->default_branch);
        yyjson_mut_obj_add_str(doc, po, "remote_url", p->remote_url);
        yyjson_mut_obj_add_bool(doc, po, "is_git_repo", p->is_git_repo);
        yyjson_mut_obj_add_bool(doc, po, "collapsed", p->collapsed);

        yyjson_mut_val *acts = yyjson_mut_arr(doc);
        for (int j = 0; j < p->action_count; ++j) {
            yyjson_mut_val *ao = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, ao, "name", p->actions[j].name);
            yyjson_mut_obj_add_str(doc, ao, "command", p->actions[j].command);
            yyjson_mut_arr_append(acts, ao);
        }
        yyjson_mut_obj_add_val(doc, po, "actions", acts);
        yyjson_mut_arr_append(projects, po);
    }
    yyjson_mut_obj_add_val(doc, root, "projects", projects);

    yyjson_mut_val *threads = yyjson_mut_arr(doc);
    for (int i = 0; i < store->thread_count; ++i) {
        const mcode_thread *t = &store->threads[i];
        if (t->is_draft) continue;
        yyjson_mut_val *to = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, to, "id", t->id);
        yyjson_mut_obj_add_str(doc, to, "project_id", t->project_id);
        yyjson_mut_obj_add_str(doc, to, "title", t->title);
        yyjson_mut_obj_add_int(doc, to, "agent", (int)t->agent);
        yyjson_mut_obj_add_str(doc, to, "model", t->model);
        if (t->reasoning_effort[0]) {
            yyjson_mut_obj_add_str(doc, to, "reasoning_effort", t->reasoning_effort);
        }
        if (t->speed_mode == MCODE_SPEED_FAST) {
            yyjson_mut_obj_add_str(doc, to, "speed_mode", "fast");
        } else if (t->speed_mode == MCODE_SPEED_STANDARD) {
            yyjson_mut_obj_add_str(doc, to, "speed_mode", "standard");
        }
        yyjson_mut_obj_add_bool(doc, to, "auto_approve", t->perm_mode == MCODE_PERM_FULL_ACCESS);
        const char *pm_str = (t->perm_mode == MCODE_PERM_SUPERVISED) ? "supervised"
                           : (t->perm_mode == MCODE_PERM_FULL_ACCESS) ? "full_access"
                           : "accept_edits";
        yyjson_mut_obj_add_str(doc, to, "permission_mode", pm_str);
        yyjson_mut_obj_add_str(doc, to, "workdir", t->workdir);
        yyjson_mut_obj_add_str(doc, to, "branch", t->branch);
        yyjson_mut_obj_add_str(doc, to, "session_id", t->session_id);
        yyjson_mut_obj_add_int(doc, to, "state", (int)t->state);
        yyjson_mut_obj_add_sint(doc, to, "created_at", t->created_at);
        yyjson_mut_obj_add_sint(doc, to, "updated_at", t->updated_at);

        yyjson_mut_val *turns = yyjson_mut_arr(doc);
        for (int ti = 0; ti < t->turn_count; ++ti) {
            const mcode_turn *tr = &t->turns[ti];
            yyjson_mut_val *tro = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, tro, "prompt", mcode_str_cstr(&tr->prompt));
            yyjson_mut_obj_add_sint(doc, tro, "created_at", tr->created_at);
            yyjson_mut_obj_add_real(doc, tro, "duration_sec", tr->duration_sec);
            yyjson_mut_obj_add_bool(doc, tro, "failed", tr->failed);
            yyjson_mut_obj_add_int(doc, tro, "input_tokens", tr->input_tokens);
            yyjson_mut_obj_add_int(doc, tro, "output_tokens", tr->output_tokens);
            yyjson_mut_obj_add_real(doc, tro, "cost_usd", tr->cost_usd);

            yyjson_mut_val *blocks = yyjson_mut_arr(doc);
            for (int bi = 0; bi < tr->block_count; ++bi) {
                const mcode_block *b = &tr->blocks[bi];
                yyjson_mut_val *bo = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_int(doc, bo, "kind", (int)b->kind);
                yyjson_mut_obj_add_str(doc, bo, "text", mcode_str_cstr(&b->text));
                if (b->kind == MCODE_BLOCK_TOOL) {
                    yyjson_mut_obj_add_str(doc, bo, "tool_id", b->tool_id);
                    yyjson_mut_obj_add_str(doc, bo, "tool_name", b->tool_name);
                    yyjson_mut_obj_add_str(doc, bo, "tool_detail", b->tool_detail);
                    yyjson_mut_obj_add_str(doc, bo, "input", mcode_str_cstr(&b->input));
                    yyjson_mut_obj_add_str(doc, bo, "output", mcode_str_cstr(&b->output));
                    yyjson_mut_obj_add_int(doc, bo, "status", (int)b->status);
                }
                yyjson_mut_obj_add_real(doc, bo, "duration_sec", b->duration_sec);
                yyjson_mut_arr_append(blocks, bo);
            }
            yyjson_mut_obj_add_val(doc, tro, "blocks", blocks);
            yyjson_mut_arr_append(turns, tro);
        }
        yyjson_mut_obj_add_val(doc, to, "turns", turns);
        yyjson_mut_arr_append(threads, to);
    }
    yyjson_mut_obj_add_val(doc, root, "threads", threads);

    bool ok = false;
#ifdef _WIN32
    FILE *f = fopen(path, "wb");
    if (f) {
        ok = yyjson_mut_write_fp(f, doc, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL, NULL);
        if (fclose(f) != 0) ok = false;
    }
#else
    char tmp[MCODE_PATH_MAX + 32];
    int n = snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
    if (n > 0 && (size_t)n < sizeof tmp) {
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "wb");
            if (!f) {
                close(fd);
                unlink(tmp);
            } else {
                ok = yyjson_mut_write_fp(f, doc, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL, NULL);
                if (fclose(f) != 0) ok = false;
                if (ok && rename(tmp, path) != 0) {
                    ok = false;
                    unlink(tmp);
                } else if (!ok) {
                    unlink(tmp);
                }
            }
        }
    }
#endif
    yyjson_mut_doc_free(doc);
    if (!ok) app_log_error("Could not save Mcode state to %s", path);
    return ok;
}
