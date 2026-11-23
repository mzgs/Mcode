#include "mcode_ui_internal.h"
#include "mcode_git.h"
#include <stdio.h>
#include <string.h>

static void draw_new_thread_dialog(mcode_app *app, cui_ctx *ui) {
    cui_dialog(ui, "New Thread", &app->new_thread_open,
               .description = "Launch an isolated AI coding thread for a project.",
               .width = 480) {
        if (app->store.project_count > 0) {
            const char *proj_names[64];
            int n_proj = app->store.project_count < 64 ? app->store.project_count : 64;
            for (int i = 0; i < n_proj; ++i) proj_names[i] = app->store.projects[i].name;
            if (app->new_thread_project_idx < 0 || app->new_thread_project_idx >= n_proj) {
                app->new_thread_project_idx = 0;
            }
            cui_select(ui, "nt-project", proj_names, n_proj, &app->new_thread_project_idx,
                       .label = "Project", .width = CUI_FILL);

            static const char *const agent_labels[APP_AGENT_COUNT] = {
                "Claude Code", "Codex", "Antigravity CLI"
            };
            cui_text(ui, "Agent", .font = CUI_FONT_MEDIUM);
            cui_segmented(ui, "nt-agent", agent_labels, APP_AGENT_COUNT, &app->new_thread_agent,
                          .full_width = true);
            if (!app->cli_status.available[app->new_thread_agent]) {
                cui_alert(ui, "CLI not detected",
                          .description = "You can still create the thread and configure the CLI path in Settings.",
                          .tone = CUI_TONE_WARNING);
            }

            cui_input(ui, "nt-title", app->new_thread_title, sizeof app->new_thread_title,
                      .label = "Title (optional)",
                      .placeholder = "e.g. Fix auth token refresh (auto-titled from first prompt)",
                      .width = CUI_FILL);

            cui_input(ui, "nt-model", app->new_thread_model, sizeof app->new_thread_model,
                      .label = "Model override (optional)",
                      .placeholder = "Leave blank to use CLI default",
                      .width = CUI_FILL);

            mcode_project *sel_proj = &app->store.projects[app->new_thread_project_idx];
            cui_switch(ui, "Create isolated Git worktree", &app->new_thread_worktree,
                       .description = sel_proj->is_git_repo
                           ? "Creates a fresh mcode/* branch under ~/.mcode/worktrees/"
                           : "Unavailable: selected folder is not a Git repository",
                       .disabled = !sel_proj->is_git_repo);

            cui_switch(ui, "Auto-approve permissions", &app->new_thread_auto_approve,
                       .description = "Skip interactive permission prompts for unattended runs");
        } else {
            cui_secondary(ui, "Add a project folder first to create a thread.");
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel")) {
                app->new_thread_open = false;
            }
            if (cui_button(ui, "Create Thread", .variant = CUI_BUTTON_PRIMARY,
                           .icon = CUI_ICON_PLUS, .disabled = app->store.project_count == 0)) {
                mcode_project *p = &app->store.projects[app->new_thread_project_idx];
                mcode_ui_create_thread(app, ui, p,
                                       app->new_thread_title,
                                       (app_agent_kind)app->new_thread_agent,
                                       app->new_thread_model,
                                       app->new_thread_worktree && p->is_git_repo,
                                       app->new_thread_auto_approve);
                app->new_thread_open = false;
            }
        }
    }
}

static void draw_git_dialog(mcode_app *app, cui_ctx *ui) {
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    const char *title = app->git_dialog_is_pr ? "Push & Create Pull Request" : "Commit Changes";

    cui_dialog(ui, title, &app->git_dialog_open,
               .description = t ? t->branch : NULL,
               .width = 500) {
        if (t) {
            char summary[128];
            snprintf(summary, sizeof summary, "%d changed file(s) (+%d / -%d) in %s",
                     t->diff_file_count, t->total_added, t->total_removed,
                     t->branch[0] ? t->branch : "HEAD");
            cui_secondary(ui, summary);

            if (t->diff_file_count > 0 || !app->git_dialog_is_pr) {
                cui_input(ui, "git-commit-msg", app->commit_msg, sizeof app->commit_msg,
                          .label = "Commit message",
                          .placeholder = "Describe your changes...",
                          .width = CUI_FILL);
            }

            if (app->git_dialog_is_pr) {
                cui_input(ui, "git-pr-title", app->pr_title, sizeof app->pr_title,
                          .label = "Pull Request title", .width = CUI_FILL);
                cui_textarea(ui, "git-pr-body", app->pr_body, sizeof app->pr_body,
                             .label = "Description", .height = 96, .width = CUI_FILL);
            }
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel")) {
                app->git_dialog_open = false;
            }
            if (t && !app->git_dialog_is_pr) {
                if (cui_button(ui, "Commit", .variant = CUI_BUTTON_SECONDARY,
                               .disabled = !app->commit_msg[0] || t->diff_file_count == 0)) {
                    char err[256] = "";
                    if (mcode_git_commit_all(t->workdir, app->commit_msg, err, sizeof err)) {
                        mcode_ui_refresh_thread_diff(t);
                        app_chrome_notify(app->chrome, ui, "Changes committed.", CUI_TONE_SUCCESS);
                        app->git_dialog_open = false;
                    } else {
                        app_chrome_notify(app->chrome, ui, err[0] ? err : "Commit failed.", CUI_TONE_DANGER);
                    }
                }
                if (cui_button(ui, "Commit & Push", .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_UPLOAD,
                               .disabled = !app->commit_msg[0])) {
                    char err[256] = "";
                    bool ok = true;
                    if (t->diff_file_count > 0) {
                        ok = mcode_git_commit_all(t->workdir, app->commit_msg, err, sizeof err);
                    }
                    if (ok) ok = mcode_git_push(t->workdir, err, sizeof err);
                    if (ok) {
                        mcode_ui_refresh_thread_diff(t);
                        app_chrome_notify(app->chrome, ui, "Committed and pushed.", CUI_TONE_SUCCESS);
                        app->git_dialog_open = false;
                    } else {
                        app_chrome_notify(app->chrome, ui, err[0] ? err : "Push failed.", CUI_TONE_DANGER);
                    }
                }
            } else if (t && app->git_dialog_is_pr) {
                if (cui_button(ui, "Push & Open PR", .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_EXTERNAL_LINK)) {
                    char err[256] = "";
                    bool ok = true;
                    if (t->diff_file_count > 0 && app->commit_msg[0]) {
                        ok = mcode_git_commit_all(t->workdir, app->commit_msg, err, sizeof err);
                    }
                    char url[1024] = "";
                    bool used_gh = false;
                    if (ok) {
                        ok = mcode_git_create_pr(t->workdir, app->pr_title, app->pr_body,
                                                 url, sizeof url, &used_gh, err, sizeof err);
                    }
                    if (ok) {
                        mcode_ui_refresh_thread_diff(t);
                        if (url[0]) cui_app_open_url(url);
                        app_chrome_notify(app->chrome, ui,
                                          used_gh ? "Pull Request created and opened."
                                                  : "Branch pushed — opened PR page in browser.",
                                          CUI_TONE_SUCCESS);
                        app->git_dialog_open = false;
                    } else {
                        app_chrome_notify(app->chrome, ui, err[0] ? err : "Could not create PR.", CUI_TONE_DANGER);
                    }
                }
            }
        }
    }
}

static void draw_project_actions_dialog(mcode_app *app, cui_ctx *ui) {
    mcode_thread *sel = mcode_store_selected_thread(&app->store);
    mcode_project *proj = sel ? mcode_store_find_project(&app->store, sel->project_id) : NULL;
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }
    if (!proj && app->store.project_count > 0) proj = &app->store.projects[0];

    cui_dialog(ui, "Project Quick Actions & Setup", &app->actions_dialog_open,
               .description = proj ? proj->name : NULL,
               .width = 520) {
        if (proj) {
            int f = cui_input(ui, "proj-setup-script", proj->setup_script, sizeof proj->setup_script,
                              .label = "Worktree setup script (auto-runs on new worktrees)",
                              .placeholder = "e.g. npm install  or  cmake -S . -B build",
                              .width = CUI_FILL);
            if (f & CUI_CHANGED) mcode_ui_mark_dirty(app);

            cui_separator(ui);
            cui_text(ui, "Quick Actions", .font = CUI_FONT_MEDIUM);

            int remove_idx = -1;
            for (int i = 0; i < proj->action_count; ++i) {
                cui_push_id_int(ui, i);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_badge(ui, proj->actions[i].name, .size = CUI_SIZE_SM);
                    cui_text(ui, proj->actions[i].command, .font = CUI_FONT_MONO, .size = 12,
                             .width = CUI_FILL, .nowrap = true);
                    if (cui_icon_button(ui, CUI_ICON_TRASH_2, .tooltip = "Remove action")) {
                        remove_idx = i;
                    }
                }
                cui_pop_id(ui);
            }
            if (remove_idx >= 0) {
                if (remove_idx + 1 < proj->action_count) {
                    memmove(&proj->actions[remove_idx], &proj->actions[remove_idx + 1],
                            (size_t)(proj->action_count - remove_idx - 1) * sizeof proj->actions[0]);
                }
                proj->action_count--;
                mcode_ui_mark_dirty(app);
            }

            if (proj->action_count < MCODE_MAX_QUICK_ACTIONS) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                    cui_input(ui, "qa-name", app->qa_new_name, sizeof app->qa_new_name,
                              .label = "Label", .placeholder = "Lint", .width = CUI_PX(120));
                    cui_input(ui, "qa-cmd", app->qa_new_cmd, sizeof app->qa_new_cmd,
                              .label = "Shell command", .placeholder = "npm run lint", .width = CUI_FILL);
                    if (cui_button(ui, "Add", .variant = CUI_BUTTON_SECONDARY,
                                   .disabled = !app->qa_new_name[0] || !app->qa_new_cmd[0])) {
                        mcode_quick_action *a = &proj->actions[proj->action_count++];
                        snprintf(a->name, sizeof a->name, "%s", app->qa_new_name);
                        snprintf(a->command, sizeof a->command, "%s", app->qa_new_cmd);
                        app->qa_new_name[0] = '\0';
                        app->qa_new_cmd[0] = '\0';
                        mcode_ui_mark_dirty(app);
                    }
                }
            }
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, "Done", .variant = CUI_BUTTON_PRIMARY)) {
                app->actions_dialog_open = false;
            }
        }
    }
}

void mcode_ui_draw_dialogs(mcode_app *app, cui_ctx *ui) {
    draw_new_thread_dialog(app, ui);
    draw_git_dialog(app, ui);
    draw_project_actions_dialog(app, ui);
}
