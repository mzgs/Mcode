#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_ui_internal.h"
#include "mcode_git.h"
#include "mcode_provider_icons.h"
#include "app_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MCODE_TERM_TEXT_CAP = 256 * 1024,
    MCODE_TERM_LINE_CAP = 4096,
};

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

void mcode_ui_select_thread(mcode_app *app, const char *thread_id) {
    if (!app || !thread_id || !thread_id[0]) return;
    if (strcmp(app->store.selected_thread_id, thread_id) == 0) return;
    mcode_thread *t = mcode_store_find_thread(&app->store, thread_id);
    if (!t) return;
    snprintf(app->store.selected_thread_id, sizeof app->store.selected_thread_id, "%s", t->id);
    mcode_ui_refresh_thread_diff(t);
    mcode_ui_mark_dirty(app);
}

static void on_term_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    mcode_app *app = (mcode_app *)user;
    pthread_mutex_lock(&app->term_mu);
    mcode_str_append(&app->term_pending, data, len);
    pthread_mutex_unlock(&app->term_mu);
}

static void on_term_exit(int exit_code, bool killed, void *user) {
    (void)killed;
    mcode_app *app = (mcode_app *)user;
    pthread_mutex_lock(&app->term_mu);
    app->term_exited = true;
    app->term_exit_code = exit_code;
    pthread_mutex_unlock(&app->term_mu);
}

void mcode_ui_run_terminal_cmd(mcode_app *app, cui_ctx *ui, const char *cmd, const char *cwd) {
    if (!app || !cmd || !cmd[0]) return;
    if (app->term_proc) {
        mcode_proc_destroy(app->term_proc);
        app->term_proc = NULL;
    }
    app->store.show_terminal_panel = true;
    mcode_ui_mark_dirty(app);

    if (cwd && cwd[0]) {
        cui_terminal_printf(&app->term_buf, "\x1b[2m[%s]\x1b[0m \x1b[36m$ %s\x1b[0m\n", cwd, cmd);
    } else {
        cui_terminal_printf(&app->term_buf, "\x1b[36m$ %s\x1b[0m\n", cmd);
    }

    pthread_mutex_lock(&app->term_mu);
    app->term_exited = false;
    app->term_exit_code = 0;
    pthread_mutex_unlock(&app->term_mu);

    app->term_proc = mcode_proc_spawn_shell(cmd, cwd, on_term_bytes, on_term_exit, app);
    if (!app->term_proc) {
        cui_terminal_printf(&app->term_buf, "\x1b[31mFailed to spawn command.\x1b[0m\n");
        if (ui) app_chrome_notify(app->chrome, ui, "Failed to run shell command.", CUI_TONE_DANGER);
    }
}

void mcode_ui_stop_terminal(mcode_app *app) {
    if (!app || !app->term_proc) return;
    mcode_proc_stop(app->term_proc);
}

mcode_thread *mcode_ui_create_thread(mcode_app *app, cui_ctx *ui, mcode_project *proj,
                                     const char *title, app_agent_kind agent,
                                     const char *model, bool use_worktree, bool auto_approve) {
    if (!app || !proj) return NULL;
    const char *final_title = (title && title[0]) ? title : "New thread";

    char workdir[MCODE_PATH_MAX];
    char branch[MCODE_BRANCH_MAX];
    snprintf(workdir, sizeof workdir, "%s", proj->path);
    snprintf(branch, sizeof branch, "%s", proj->default_branch);

    bool actual_worktree = false;
    if (use_worktree && proj->is_git_repo) {
        char err[256] = "";
        if (mcode_git_create_worktree(proj->path, proj->name, final_title,
                                      workdir, sizeof workdir,
                                      branch, sizeof branch,
                                      err, sizeof err)) {
            actual_worktree = true;
            if (proj->setup_script[0]) {
                mcode_ui_run_terminal_cmd(app, ui, proj->setup_script, workdir);
            }
        } else {
            char msg[320];
            snprintf(msg, sizeof msg, "Worktree failed (using repo root): %s", err);
            if (ui) app_chrome_notify(app->chrome, ui, msg, CUI_TONE_WARNING);
        }
    }

    mcode_thread *t = mcode_store_add_thread(&app->store, proj, final_title,
                                             agent, model, actual_worktree, auto_approve,
                                             workdir, branch);
    if (t) {
        mcode_ui_refresh_thread_diff(t);
        mcode_ui_mark_dirty(app);
    }
    return t;
}

void mcode_ui_send_prompt(mcode_app *app, cui_ctx *ui, mcode_thread *thread, const char *prompt) {
    if (!app || !thread || !prompt || !prompt[0]) return;
    if (thread->active_run) return;

    bool should_auto_title = (thread->turn_count == 0 &&
        (strcmp(thread->title, "New thread") == 0 || thread->title[0] == '\0'));

    thread->state = MCODE_THREAD_ACTIVE;
    char err[256] = "";
    const app_settings *settings = app_chrome_settings(app->chrome);
    if (!mcode_agent_start(thread, settings, prompt, err, sizeof err)) {
        if (ui) app_chrome_notify(app->chrome, ui, err[0] ? err : "Could not start agent.", CUI_TONE_DANGER);
    } else {
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
            thread->title_run = mcode_agent_start_title(thread, settings, prompt);
        }
        mcode_ui_mark_dirty(app);
    }
}

void mcode_ui_stop_thread(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!thread || !thread->active_run) return;
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
    if (t->use_worktree && t->workdir[0]) {
        mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
        if (p && p->is_git_repo && strcmp(p->path, t->workdir) != 0) {
            char err[256] = "";
            if (!mcode_git_remove_worktree(p->path, t->workdir, err, sizeof err)) {
                app_log_warn("Could not remove worktree %s: %s", t->workdir, err);
            }
        }
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
    pthread_mutex_init(&app->term_mu, NULL);
    mcode_str_init(&app->term_pending);

    app->term_text = calloc(1, MCODE_TERM_TEXT_CAP);
    app->term_lines = calloc(MCODE_TERM_LINE_CAP, sizeof *app->term_lines);
    if (app->term_text && app->term_lines) {
        cui_terminal_init(&app->term_buf, app->term_text, MCODE_TERM_TEXT_CAP,
                          app->term_lines, MCODE_TERM_LINE_CAP);
        cui_terminal_printf(&app->term_buf, "\x1b[2mMcode shell ready. Run a command or click a quick action above.\x1b[0m\n");
    }

    char dir[MCODE_PATH_MAX] = "";
    if (chrome && app_settings_dir(dir, sizeof dir, chrome->cli.settings_path, true)) {
        snprintf(app->state_path, sizeof app->state_path, "%s/mcode-state.json", dir);
        if (mcode_store_load(&app->store, app->state_path)) {
            mcode_thread *sel = mcode_store_selected_thread(&app->store);
            if (sel) mcode_ui_refresh_thread_diff(sel);
        }
    }

    mcode_agent_detect_clis(chrome ? app_chrome_settings(chrome) : NULL, &app->cli_status);
    app->cli_checked_at = mcode_now_mono();
    mcode_model_catalog_init(&app->models);
    mcode_model_catalog_refresh(&app->models, &app->cli_status);
}

void mcode_app_shutdown(mcode_app *app) {
    if (!app) return;
    mcode_model_catalog_free(&app->models);
    if (app->term_proc) {
        mcode_proc_destroy(app->term_proc);
        app->term_proc = NULL;
    }
    for (int i = 0; i < app->store.thread_count; ++i) {
        if (app->store.threads[i].active_run) {
            mcode_agent_destroy(app->store.threads[i].active_run);
            app->store.threads[i].active_run = NULL;
        }
        if (app->store.threads[i].title_run) {
            mcode_agent_title_destroy(app->store.threads[i].title_run);
            app->store.threads[i].title_run = NULL;
        }
    }
    if (app->state_path[0]) {
        mcode_store_save(&app->store, app->state_path);
    }
    mcode_store_free(&app->store);
    mcode_str_free(&app->term_pending);
    pthread_mutex_destroy(&app->term_mu);
    free(app->term_text);
    free(app->term_lines);
    app->term_text = NULL;
    app->term_lines = NULL;
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
        (void)mcode_agent_poll(t->active_run, t, &finished);
        if (finished) {
            mcode_agent_destroy(t->active_run);
            t->active_run = NULL;
            mcode_ui_refresh_thread_diff(t);
            mcode_ui_mark_dirty(app);
            any_finished = true;
            char msg[220];
            snprintf(msg, sizeof msg, "Finished: %s", t->title);
            app_chrome_notify(app->chrome, ui, msg, CUI_TONE_SUCCESS);
        } else {
            ++running;
        }
    }

    /* Drain integrated terminal stream */
    bool term_active = (app->term_proc != NULL);
    pthread_mutex_lock(&app->term_mu);
    if (app->term_pending.len > 0) {
        cui_terminal_write(&app->term_buf, app->term_pending.data, (int)app->term_pending.len);
        mcode_str_clear(&app->term_pending);
    }
    bool texited = app->term_exited;
    int tcode = app->term_exit_code;
    app->term_exited = false;
    pthread_mutex_unlock(&app->term_mu);

    if (texited && app->term_proc) {
        mcode_proc_destroy(app->term_proc);
        app->term_proc = NULL;
        term_active = false;
        if (tcode == 0) {
            cui_terminal_printf(&app->term_buf, "\x1b[32m[exit 0]\x1b[0m\n");
        } else {
            cui_terminal_printf(&app->term_buf, "\x1b[31m[exit %d]\x1b[0m\n", tcode);
        }
        mcode_thread *sel = mcode_store_selected_thread(&app->store);
        if (sel) mcode_ui_refresh_thread_diff(sel);
    }

    bool catalog_updated = mcode_model_catalog_poll(&app->models);
    if (running > 0 || term_active) {
        cui_request_wakeup(ui, 0.05f);
    } else if (title_running > 0 || app->models.loading_agy) {
        cui_request_wakeup(ui, 0.1f);
    } else if (any_finished || catalog_updated) {
        cui_request_redraw(ui);
    }

    /* Refresh CLI availability periodically when settings dialog closes or every 30s */
    double now = mcode_now_mono();
    if (now - app->cli_checked_at > 30.0) {
        bool had_agy = app->cli_status.available[APP_AGENT_AGY];
        mcode_agent_detect_clis(app_chrome_settings(app->chrome), &app->cli_status);
        app->cli_checked_at = now;
        if (app->cli_status.available[APP_AGENT_AGY] && (!had_agy || !app->models.loaded_agy)) {
            mcode_model_catalog_refresh(&app->models, &app->cli_status);
        }
    }

    /* Debounced atomic save of workspace state */
    if (app->state_dirty && (now - app->state_dirty_since) >= 0.45 && app->state_path[0]) {
        mcode_store_save(&app->store, app->state_path);
        app->state_dirty = false;
    }

    if (out_running_count) *out_running_count = running;
}

static void draw_status_bar(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int running_count) {
    cui_status_bar(ui, "mcode-status") {
        if (thread) {
            if (thread->branch[0]) {
                char br[160];
                snprintf(br, sizeof br, "%s###sb-branch", thread->branch);
                if (cui_status_bar_item(ui, br, .icon = CUI_ICON_GIT_BRANCH,
                                        .tooltip = thread->use_worktree ? "Isolated Git worktree branch" : "Repository branch")) {
                    mcode_ui_refresh_thread_diff(thread);
                }
            }
            if (thread->workdir[0]) {
                char wd[MCODE_PATH_MAX + 16];
                snprintf(wd, sizeof wd, "%s###sb-dir", thread->workdir);
                if (cui_status_bar_item(ui, wd, .icon = CUI_ICON_FOLDER,
                                        .tooltip = "Open folder in Finder")) {
                    if (!cui_app_open_url(thread->workdir)) {
                        app_chrome_notify(app->chrome, ui, "Could not open folder.", CUI_TONE_DANGER);
                    }
                }
            }
        } else {
            cui_status_bar_text(ui, "No active thread", .icon = CUI_ICON_INBOX);
        }

        cui_status_bar_spacer(ui);

        if (running_count > 0) {
            char run_buf[48];
            snprintf(run_buf, sizeof run_buf, "%d running###sb-run", running_count);
            cui_status_bar_text(ui, run_buf, .icon = CUI_ICON_ACTIVITY, .tone = CUI_TONE_INFO);
        }

        /* CLI readiness indicators */
        for (int i = 0; i < APP_AGENT_COUNT; ++i) {
            char lbl[48];
            snprintf(lbl, sizeof lbl, "%s###cli-%d", mcode_agent_short_label((app_agent_kind)i), i);
            cui_tone tone = app->cli_status.available[i] ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL;
            const char *icon = app->cli_status.available[i] ? CUI_ICON_CIRCLE_CHECK : CUI_ICON_CIRCLE_X;
            if (cui_status_bar_item(ui, lbl, .icon = icon, .tone = tone,
                                    .tooltip = app->cli_status.available[i]
                                        ? app->cli_status.resolved_path[i]
                                        : "Not detected (configure in Settings)")) {
                app->chrome->settings_open = true;
                app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
            }
        }
    }
}

void mcode_app_frame(mcode_app *app, cui_ctx *ui) {
    if (!app || !ui) return;
    int running_count = 0;
    poll_background_work(app, ui, &running_count);

    mcode_thread *thread = mcode_store_selected_thread(&app->store);

    /* Keep menu checkmarks in sync */
    app_chrome_set_action_checked(app->chrome, "toggle_diff", app->store.show_diff_panel);
    app_chrome_set_action_checked(app->chrome, "toggle_terminal", app->store.show_terminal_panel);
    app_chrome_set_action_enabled(app->chrome, "commit", thread != NULL);
    app_chrome_set_action_enabled(app->chrome, "create_pr", thread != NULL);
    app_chrome_set_action_enabled(app->chrome, "refresh_diff", thread != NULL);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL) {
            cui_split(ui, "mcode-main-split", &app->store.sidebar_ratio,
                      .min_first = 220, .min_second = 420,
                      .width = CUI_FILL, .height = CUI_FILL) {
                mcode_ui_draw_sidebar(app, ui);
                cui_split_next(ui);

                if (app->store.show_diff_panel && thread) {
                    cui_split(ui, "mcode-diff-split", &app->store.diff_ratio,
                              .min_first = 360, .min_second = 280,
                              .width = CUI_FILL, .height = CUI_FILL) {
                        if (app->store.show_terminal_panel) {
                            cui_split(ui, "mcode-term-split", &app->store.terminal_ratio,
                                      .vertical = true, .min_first = 220, .min_second = 150,
                                      .width = CUI_FILL, .height = CUI_FILL) {
                                mcode_ui_draw_chat(app, ui, thread);
                                cui_split_next(ui);
                                mcode_ui_draw_terminal(app, ui, thread);
                            }
                        } else {
                            mcode_ui_draw_chat(app, ui, thread);
                        }
                        cui_split_next(ui);
                        mcode_ui_draw_diff(app, ui, thread);
                    }
                } else if (app->store.show_terminal_panel) {
                    cui_split(ui, "mcode-term-split-wide", &app->store.terminal_ratio,
                              .vertical = true, .min_first = 220, .min_second = 150,
                              .width = CUI_FILL, .height = CUI_FILL) {
                        mcode_ui_draw_chat(app, ui, thread);
                        cui_split_next(ui);
                        mcode_ui_draw_terminal(app, ui, thread);
                    }
                } else {
                    mcode_ui_draw_chat(app, ui, thread);
                }
            }
        }
        draw_status_bar(app, ui, thread, running_count);
    }

    mcode_ui_draw_dialogs(app, ui);
}

/* ---- Actions & Command Palette ---- */

void mcode_app_action_add_project(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    char path[MCODE_PATH_MAX] = "";
    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_FOLDER,
            .title = "Select Project Repository Folder",
        }, path, sizeof path)) {
        mcode_project *p = mcode_store_add_project(&app->store, path, NULL);
        if (p) {
            snprintf(app->store.selected_project_id, sizeof app->store.selected_project_id, "%s", p->id);
            mcode_ui_mark_dirty(app);
            char msg[220];
            snprintf(msg, sizeof msg, "Added project: %s%s", p->name,
                     p->is_git_repo ? "" : " (not a Git repo)");
            if (ui) app_chrome_notify(app->chrome, ui, msg, CUI_TONE_SUCCESS);
        }
    }
}

void mcode_app_action_new_thread(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    if (app->store.project_count == 0) {
        mcode_app_action_add_project(ui, app);
        if (app->store.project_count == 0) return;
    }
    const app_settings *s = app_chrome_settings(app->chrome);
    app->new_thread_project_idx = 0;
    if (app->store.selected_project_id[0]) {
        for (int i = 0; i < app->store.project_count; ++i) {
            if (strcmp(app->store.projects[i].id, app->store.selected_project_id) == 0) {
                app->new_thread_project_idx = i;
                break;
            }
        }
    }
    app->new_thread_agent = (int)s->mcode.default_agent;
    app->new_thread_title[0] = '\0';
    app->new_thread_model[0] = '\0';
    app->new_thread_custom_model = false;
    app->new_thread_worktree = s->mcode.use_worktrees_by_default;
    app->new_thread_auto_approve = s->mcode.auto_approve_permissions;
    app->new_thread_open = true;
}

void mcode_app_action_toggle_diff(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->store.show_diff_panel = !app->store.show_diff_panel;
    if (app->store.show_diff_panel) {
        mcode_thread *t = mcode_store_selected_thread(&app->store);
        if (t) mcode_ui_refresh_thread_diff(t);
    }
    mcode_ui_mark_dirty(app);
}

void mcode_app_action_toggle_terminal(cui_ctx *ui, void *user) {
    (void)ui;
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    app->store.show_terminal_panel = !app->store.show_terminal_panel;
    mcode_ui_mark_dirty(app);
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
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t) {
        mcode_ui_refresh_thread_diff(t);
        if (ui) app_chrome_notify(app->chrome, ui, "Diff refreshed.", CUI_TONE_INFO);
    }
}

void mcode_app_action_project_actions(cui_ctx *ui, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;
    if (app->store.project_count == 0) {
        if (ui) app_chrome_notify(app->chrome, ui, "Add a project first.", CUI_TONE_WARNING);
        return;
    }
    app->qa_new_name[0] = '\0';
    app->qa_new_cmd[0] = '\0';
    app->actions_dialog_open = true;
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

static palette_target s_thread_targets[64];
static palette_target s_action_targets[MCODE_MAX_QUICK_ACTIONS];

static void on_palette_select_thread(cui_ctx *ui, void *user) {
    (void)ui;
    palette_target *pt = (palette_target *)user;
    if (!pt || !pt->app || pt->index < 0 || pt->index >= pt->app->store.thread_count) return;
    mcode_ui_select_thread(pt->app, pt->app->store.threads[pt->index].id);
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

void mcode_app_palette_provider(app_palette_builder *builder, void *user) {
    mcode_app *app = (mcode_app *)user;
    if (!app) return;

    int max_threads = app->store.thread_count < 64 ? app->store.thread_count : 64;
    for (int i = 0; i < max_threads; ++i) {
        const mcode_thread *t = &app->store.threads[i];
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
