#include "mcode_ui_internal.h"
#include "mcode_git.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool dialog_contains_icase(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!hay || !hay[0]) return false;
    for (; *hay; ++hay) {
        const char *a = hay, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static void format_join_preview(const char *parent, const char *leaf, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char short_parent[MCODE_PATH_MAX];
    mcode_git_shorten_home_path(parent ? parent : "", short_parent, sizeof short_parent);
    size_t plen = strlen(short_parent);
    while (plen > 1 && (short_parent[plen - 1] == '/' || short_parent[plen - 1] == '\\')) {
        short_parent[--plen] = '\0';
    }
    const char *name = (leaf && leaf[0]) ? leaf : "<project-name>";
    if (short_parent[0]) {
        snprintf(out, cap, "%s/%s", short_parent, name);
    } else {
        snprintf(out, cap, "%s", name);
    }
}

static void try_create_project(mcode_app *app, cui_ctx *ui) {
    char created_path[MCODE_PATH_MAX] = "";
    char err[256] = "";
    if (!mcode_git_create_project_dir(app->new_proj_parent, app->new_proj_name,
                                      app->new_proj_init_git,
                                      created_path, sizeof created_path,
                                      err, sizeof err)) {
        snprintf(app->project_dialog_err, sizeof app->project_dialog_err, "%s",
                 err[0] ? err : "Could not create project folder.");
        return;
    }
    app->project_dialog_err[0] = '\0';
    mcode_ui_add_project_and_open(app, ui, created_path, app->new_proj_name);
    app->project_dialog_open = false;
}

static void draw_project_dialog(mcode_app *app, cui_ctx *ui) {
    bool was_open = app->project_dialog_open;
    bool cloning = (app->clone_proc != NULL);

    cui_dialog(ui, "Add Project", &app->project_dialog_open,
               .description = "Open an existing folder, create a new project, or clone from Git.",
               .width = 520) {
        static const char *const mode_labels[3] = {
            "Open Folder", "New Project", "Clone URL"
        };
        static const char *const mode_icons[3] = {
            CUI_ICON_FOLDER_OPEN, CUI_ICON_PLUS, CUI_ICON_DOWNLOAD
        };
        int seg_mode = app->project_dialog_mode;
        if (cui_segmented(ui, "proj-dialog-mode", mode_labels, 3, &seg_mode,
                          .icons = mode_icons, .full_width = true)) {
            if (!cloning && seg_mode != app->project_dialog_mode) {
                app->project_dialog_mode = seg_mode;
                app->project_dialog_err[0] = '\0';
                if (app->project_dialog_mode == MCODE_PROJ_DIALOG_OPEN) {
                    mcode_ui_refresh_discovered_repos(app);
                }
            }
        }

        if (app->project_dialog_mode == MCODE_PROJ_DIALOG_OPEN) {
            if (cui_button(ui, "Choose Folder...", .variant = CUI_BUTTON_PRIMARY,
                           .icon = CUI_ICON_FOLDER_OPEN, .full_width = true)) {
                char path[MCODE_PATH_MAX] = "";
                if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                        .kind = CUI_FILE_FOLDER,
                        .title = "Select Project Folder",
                    }, path, sizeof path)) {
                    mcode_ui_add_project_and_open(app, ui, path, NULL);
                    app->project_dialog_open = false;
                }
            }

            cui_separator(ui);

            /* Quick-add list of discovered local Git repositories */
            int avail_count = 0;
            for (int i = 0; i < app->discovered_repo_count; ++i) {
                if (!mcode_store_find_project_by_path(&app->store, app->discovered_repos[i].path)) {
                    ++avail_count;
                }
            }

            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Detected local repositories", .font = CUI_FONT_MEDIUM, .size = 12.5f);
                if (avail_count > 0) {
                    char cnt[16];
                    snprintf(cnt, sizeof cnt, "%d", avail_count);
                    cui_badge(ui, cnt, .size = CUI_SIZE_SM);
                }
                cui_spacer(ui);
                if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                    .variant = CUI_BUTTON_GHOST,
                                    .tooltip = "Rescan local developer folders")) {
                    mcode_ui_refresh_discovered_repos(app);
                }
            }

            if (avail_count > 5) {
                cui_search(ui, "disc-filter", app->discovered_filter, sizeof app->discovered_filter,
                           .placeholder = "Filter detected repositories...",
                           .size = CUI_SIZE_SM, .width = CUI_FILL);
            }

            if (avail_count == 0) {
                cui_box(ui, .pad_y = 14, .width = CUI_FILL) {
                    cui_muted(ui, "No additional Git repositories found in ~/Developer or ~/Projects. "
                                  "Use Choose Folder... above to open any directory.");
                }
            } else {
                float list_h = avail_count <= 4 ? (float)avail_count * 46.0f + 8.0f : 210.0f;
                cui_scroll(ui, .id = "disc-repo-list", .width = CUI_FILL,
                           .height = CUI_PX(list_h), .gap = 4) {
                    int shown = 0;
                    for (int i = 0; i < app->discovered_repo_count; ++i) {
                        const mcode_discovered_repo *r = &app->discovered_repos[i];
                        if (mcode_store_find_project_by_path(&app->store, r->path)) continue;
                        if (!dialog_contains_icase(r->name, app->discovered_filter) &&
                            !dialog_contains_icase(r->path, app->discovered_filter)) {
                            continue;
                        }
                        ++shown;
                        cui_push_id_int(ui, i);
                        bool clicked = cui_card_begin(ui, &(cui_card_opts){
                            .id = "drepo",
                            .dir = CUI_ROW,
                            .align = CUI_ALIGN_CENTER,
                            .width = CUI_FILL,
                            .pad_x = 10,
                            .pad_y = 7,
                            .gap = 8,
                            .hoverable = true,
                            .muted = true,
                            .radius = 7,
                        });
                        cui_icon(ui, CUI_ICON_FOLDER, 15, CUI_COLOR_TEXT_SECONDARY);
                        cui_column(ui, .width = CUI_FILL, .gap = 1) {
                            cui_text(ui, r->name, .font = CUI_FONT_MEDIUM, .size = 13,
                                     .nowrap = true);
                            char short_p[MCODE_PATH_MAX];
                            mcode_git_shorten_home_path(r->path, short_p, sizeof short_p);
                            cui_text(ui, short_p, .font = CUI_FONT_MONO, .size = 11,
                                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                        }
                        cui_icon(ui, CUI_ICON_PLUS, 14, CUI_COLOR_ACCENT);
                        cui_card_end(ui);
                        cui_pop_id(ui);

                        if (clicked) {
                            mcode_ui_add_project_and_open(app, ui, r->path, r->name);
                            app->project_dialog_open = false;
                            break;
                        }
                    }
                    if (shown == 0) {
                        cui_box(ui, .pad = 8) {
                            cui_muted(ui, "No repositories match your filter.");
                        }
                    }
                }
            }
        } else if (app->project_dialog_mode == MCODE_PROJ_DIALOG_CREATE) {
            int nf = cui_input(ui, "new-proj-name", app->new_proj_name, sizeof app->new_proj_name,
                               .label = "Project name",
                               .placeholder = "e.g. my-app",
                               .width = CUI_FILL);
            if (nf & CUI_CHANGED) app->project_dialog_err[0] = '\0';

            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                int lf = cui_input(ui, "new-proj-parent", app->new_proj_parent,
                                   sizeof app->new_proj_parent,
                                   .label = "Location",
                                   .placeholder = "~/Developer",
                                   .width = CUI_FILL);
                if (lf & CUI_CHANGED) app->project_dialog_err[0] = '\0';
                if (cui_button(ui, "Browse...", .variant = CUI_BUTTON_OUTLINE,
                               .icon = CUI_ICON_FOLDER_OPEN)) {
                    char dir[MCODE_PATH_MAX] = "";
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_FOLDER,
                            .title = "Choose Parent Folder",
                        }, dir, sizeof dir)) {
                        snprintf(app->new_proj_parent, sizeof app->new_proj_parent, "%s", dir);
                        app->project_dialog_err[0] = '\0';
                    }
                }
            }

            char preview[MCODE_PATH_MAX + MCODE_NAME_MAX + 32];
            format_join_preview(app->new_proj_parent, app->new_proj_name, preview, sizeof preview);
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_FOLDER, 13, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, preview, .font = CUI_FONT_MONO, .size = 11.5f,
                         .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL, .nowrap = true);
            }

            cui_switch(ui, "Initialize Git repository", &app->new_proj_init_git,
                       .description = "Runs git init so diffs and commits work immediately");

            if ((nf & CUI_SUBMITTED) && app->new_proj_name[0] && app->new_proj_parent[0]) {
                try_create_project(app, ui);
            }
        } else {
            int uf = cui_input(ui, "clone-url", app->clone_url, sizeof app->clone_url,
                               .label = "Git repository URL",
                               .placeholder = "https://github.com/owner/repo.git",
                               .disabled = cloning,
                               .width = CUI_FILL);
            if (uf & CUI_CHANGED) {
                app->project_dialog_err[0] = '\0';
                if (!app->clone_folder_edited) {
                    mcode_git_repo_name_from_url(app->clone_url, app->clone_folder,
                                                 sizeof app->clone_folder);
                }
            }

            int ff = cui_input(ui, "clone-folder", app->clone_folder, sizeof app->clone_folder,
                               .label = "Folder name",
                               .placeholder = "Derived automatically from URL",
                               .disabled = cloning,
                               .width = CUI_FILL);
            if (ff & CUI_CHANGED) {
                app->clone_folder_edited = (app->clone_folder[0] != '\0');
                app->project_dialog_err[0] = '\0';
            }

            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                int lf = cui_input(ui, "clone-parent", app->new_proj_parent,
                                   sizeof app->new_proj_parent,
                                   .label = "Location",
                                   .placeholder = "~/Developer",
                                   .disabled = cloning,
                                   .width = CUI_FILL);
                if (lf & CUI_CHANGED) app->project_dialog_err[0] = '\0';
                if (cui_button(ui, "Browse...", .variant = CUI_BUTTON_OUTLINE,
                               .icon = CUI_ICON_FOLDER_OPEN, .disabled = cloning)) {
                    char dir[MCODE_PATH_MAX] = "";
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_FOLDER,
                            .title = "Choose Parent Folder",
                        }, dir, sizeof dir)) {
                        snprintf(app->new_proj_parent, sizeof app->new_proj_parent, "%s", dir);
                        app->project_dialog_err[0] = '\0';
                    }
                }
            }

            char preview[MCODE_PATH_MAX + MCODE_NAME_MAX + 32];
            format_join_preview(app->new_proj_parent, app->clone_folder, preview, sizeof preview);
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_FOLDER, 13, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, preview, .font = CUI_FONT_MONO, .size = 11.5f,
                         .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL, .nowrap = true);
            }

            if (cloning) {
                char status[256] = "";
                pthread_mutex_lock(&app->clone_mu);
                snprintf(status, sizeof status, "%s",
                         app->clone_status[0] ? app->clone_status : "Cloning repository...");
                pthread_mutex_unlock(&app->clone_mu);
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_spinner(ui, 14);
                    cui_text(ui, status, .font = CUI_FONT_MONO, .size = 12,
                             .color = CUI_COLOR_TEXT_SECONDARY, .width = CUI_FILL, .nowrap = true);
                }
            }

            if (!cloning && ((uf & CUI_SUBMITTED) || (ff & CUI_SUBMITTED)) &&
                app->clone_url[0] && app->new_proj_parent[0] && app->clone_folder[0]) {
                mcode_ui_start_clone(app, ui, app->clone_url, app->new_proj_parent,
                                     app->clone_folder);
            }
        }

        if (app->project_dialog_err[0]) {
            cui_alert(ui, app->project_dialog_err, .tone = CUI_TONE_DANGER);
        }

        cui_dialog_footer(ui) {
            if (cloning) {
                if (cui_button(ui, "Stop Clone", .variant = CUI_BUTTON_DANGER,
                               .icon = CUI_ICON_SQUARE)) {
                    mcode_ui_cancel_clone(app);
                }
            } else {
                if (cui_button(ui, app->project_dialog_mode == MCODE_PROJ_DIALOG_OPEN ? "Close" : "Cancel")) {
                    app->project_dialog_open = false;
                }
            }

            if (app->project_dialog_mode == MCODE_PROJ_DIALOG_CREATE) {
                if (cui_button(ui, "Create Project", .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_PLUS,
                               .disabled = !app->new_proj_name[0] || !app->new_proj_parent[0])) {
                    try_create_project(app, ui);
                }
            } else if (app->project_dialog_mode == MCODE_PROJ_DIALOG_CLONE) {
                if (cui_button(ui, "Clone Project", .variant = CUI_BUTTON_PRIMARY,
                               .icon = CUI_ICON_DOWNLOAD,
                               .loading = cloning,
                               .disabled = !app->clone_url[0] || !app->new_proj_parent[0] ||
                                           !app->clone_folder[0])) {
                    mcode_ui_start_clone(app, ui, app->clone_url, app->new_proj_parent,
                                         app->clone_folder);
                }
            }
        }
    }

    /* If dialog was dismissed via Esc/backdrop while cloning, stop clone cleanly */
    if (was_open && !app->project_dialog_open && app->clone_proc) {
        mcode_ui_cancel_clone(app);
    }
}

static void draw_rename_project_dialog(mcode_app *app, cui_ctx *ui) {
    mcode_project *proj = mcode_store_find_project(&app->store, app->rename_project_id);
    if (!proj) {
        app->rename_project_open = false;
        return;
    }

    cui_dialog(ui, "Rename Project", &app->rename_project_open,
               .description = "Updates the display name in Mcode without renaming the folder on disk.",
               .width = 400) {
        int f = cui_input(ui, "rename-proj-input", app->rename_project_buf,
                          sizeof app->rename_project_buf,
                          .label = "Display name",
                          .width = CUI_FILL);
        bool save = (f & CUI_SUBMITTED) && app->rename_project_buf[0];

        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel")) {
                app->rename_project_open = false;
            }
            if (cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !app->rename_project_buf[0])) {
                save = true;
            }
        }

        if (save) {
            snprintf(proj->name, sizeof proj->name, "%s", app->rename_project_buf);
            mcode_ui_promote_project(app, app->rename_project_id);
            mcode_ui_mark_dirty(app);
            app->rename_project_open = false;
        }
    }
}

static void draw_new_thread_dialog(mcode_app *app, cui_ctx *ui) {
    cui_dialog(ui, "New Thread", &app->new_thread_open,
               .description = "Launch an AI coding thread with custom options.",
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
            if (cui_segmented(ui, "nt-agent", agent_labels, APP_AGENT_COUNT, &app->new_thread_agent,
                              .full_width = true)) {
                app->new_thread_model[0] = '\0';
                app->new_thread_custom_model = false;
            }
            if (!app->cli_status.available[app->new_thread_agent]) {
                cui_alert(ui, "CLI not detected",
                          .description = "You can still create the thread and configure the CLI path in Settings.",
                          .tone = CUI_TONE_WARNING);
            }

            cui_input(ui, "nt-title", app->new_thread_title, sizeof app->new_thread_title,
                      .label = "Title (optional)",
                      .placeholder = "e.g. Fix auth token refresh (auto-titled from first prompt)",
                      .width = CUI_FILL);

            {
                app_agent_kind ag = (app_agent_kind)app->new_thread_agent;
                const app_settings *settings = app_chrome_settings(app->chrome);
                const char *def_model = mcode_agent_default_model(settings, ag);
                int cat_count = (ag >= 0 && ag < APP_AGENT_COUNT) ? app->models.count[ag] : 0;
                const char *model_labels[MCODE_MAX_MODELS_PER_AGENT + 2];
                char def_label[128];
                if (def_model[0]) {
                    snprintf(def_label, sizeof def_label, "Default (%s)",
                             mcode_model_display_label(&app->models, ag, def_model));
                } else {
                    snprintf(def_label, sizeof def_label, "CLI Default");
                }
                model_labels[0] = def_label;

                int custom_idx = cat_count + 1;
                int sel_idx = 0;
                if (app->new_thread_custom_model) {
                    sel_idx = custom_idx;
                } else if (app->new_thread_model[0]) {
                    sel_idx = custom_idx;
                }
                for (int i = 0; i < cat_count; ++i) {
                    model_labels[i + 1] = app->models.items[ag][i].label[0]
                        ? app->models.items[ag][i].label
                        : app->models.items[ag][i].id;
                    if (!app->new_thread_custom_model && app->new_thread_model[0] &&
                        strcmp(app->new_thread_model, app->models.items[ag][i].id) == 0) {
                        sel_idx = i + 1;
                    }
                }
                model_labels[custom_idx] = "Custom model ID...";

                if (cui_select(ui, "nt-model-select", model_labels, custom_idx + 1, &sel_idx,
                               .label = "Model", .searchable = true, .width = CUI_FILL)) {
                    if (sel_idx == 0) {
                        app->new_thread_model[0] = '\0';
                        app->new_thread_custom_model = false;
                    } else if (sel_idx == custom_idx) {
                        app->new_thread_custom_model = true;
                    } else if (sel_idx >= 1 && sel_idx <= cat_count) {
                        snprintf(app->new_thread_model, sizeof app->new_thread_model, "%s",
                                 app->models.items[ag][sel_idx - 1].id);
                        app->new_thread_custom_model = false;
                    }
                }
                if (sel_idx == custom_idx) {
                    cui_input(ui, "nt-model-custom", app->new_thread_model,
                              sizeof app->new_thread_model,
                              .placeholder = "Enter custom model ID (e.g. sonnet, gpt-6.2-sol)",
                              .width = CUI_FILL);
                }
            }

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
                p->collapsed = false;
                mcode_ui_create_thread(app, ui, p,
                                       app->new_thread_title,
                                       (app_agent_kind)app->new_thread_agent,
                                       app->new_thread_model,
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
                cui_column(ui, .width = CUI_FILL, .gap = 6) {
                    cui_label(ui, "Commit message");
                    mcode_ui_draw_commit_msg_input(
                        app, ui, "git-commit-msg", app->commit_msg, sizeof app->commit_msg,
                        "Describe your changes...", CUI_SIZE_MD, 0.0f,
                        (cui_color){ 0 }, (cui_color){ 0 }, 0.0f,
                        t->workdir, false, t->diff_file_count > 0,
                        MCODE_COMMIT_GEN_DIALOG);
                }
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
                        mcode_ui_promote_project(app, t->project_id);
                        mcode_ui_refresh_workspace_git(app);
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
                        mcode_ui_promote_project(app, t->project_id);
                        mcode_ui_refresh_workspace_git(app);
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
                        mcode_ui_promote_project(app, t->project_id);
                        mcode_ui_refresh_workspace_git(app);
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

    if (!app->git_dialog_open &&
        app->commit_gen_run && app->commit_gen_target == MCODE_COMMIT_GEN_DIALOG) {
        mcode_ui_cancel_commit_msg_gen(app);
    }
}

static void draw_project_actions_dialog(mcode_app *app, cui_ctx *ui) {
    mcode_project *proj = app->actions_project_id[0]
        ? mcode_store_find_project(&app->store, app->actions_project_id) : NULL;
    if (!proj) {
        mcode_thread *sel = mcode_store_selected_thread(&app->store);
        proj = sel ? mcode_store_find_project(&app->store, sel->project_id) : NULL;
    }
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }
    if (!proj && app->store.project_count > 0) proj = &app->store.projects[0];

    char proj_id[MCODE_ID_MAX] = "";
    char proj_name[MCODE_NAME_MAX] = "";
    if (proj) {
        snprintf(proj_id, sizeof proj_id, "%s", proj->id);
        snprintf(proj_name, sizeof proj_name, "%s", proj->name);
    }

    cui_dialog(ui, "Project Quick Actions", &app->actions_dialog_open,
               .description = proj_name[0] ? proj_name : NULL,
               .width = 520) {
        if (proj) {
            char short_p[MCODE_PATH_MAX];
            mcode_git_shorten_home_path(proj->path, short_p, sizeof short_p);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_FOLDER, 14, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, short_p, .font = CUI_FONT_MONO, .size = 12,
                         .color = CUI_COLOR_TEXT_SECONDARY, .width = CUI_FILL, .nowrap = true);
                if (proj->is_git_repo) {
                    cui_badge(ui, proj->default_branch[0] ? proj->default_branch : "Git",
                              .icon = CUI_ICON_GIT_BRANCH, .tone = CUI_TONE_SUCCESS,
                              .size = CUI_SIZE_SM);
                } else {
                    if (cui_button(ui, "Init Git", .variant = CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_GIT_BRANCH)) {
                        mcode_ui_init_project_git(app, ui, proj);
                        proj = mcode_store_find_project(&app->store, proj_id);
                    }
                }
            }

            if (proj) {
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
                    mcode_ui_promote_project(app, proj_id);
                    proj = mcode_store_find_project(&app->store, proj_id);
                    mcode_ui_mark_dirty(app);
                }

                if (proj && proj->action_count < MCODE_MAX_QUICK_ACTIONS) {
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
                            mcode_ui_promote_project(app, proj_id);
                            proj = mcode_store_find_project(&app->store, proj_id);
                            mcode_ui_mark_dirty(app);
                        }
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
    draw_project_dialog(app, ui);
    draw_rename_project_dialog(app, ui);
    draw_new_thread_dialog(app, ui);
    draw_git_dialog(app, ui);
    draw_project_actions_dialog(app, ui);
}
