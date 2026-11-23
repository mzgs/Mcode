#include "mcode_ui_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool contains_icase(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
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

static bool thread_matches_query(mcode_app *app, const mcode_thread *t, const mcode_project *p) {
    if (!app->thread_search[0]) return true;
    if (contains_icase(t->title, app->thread_search)) return true;
    if (contains_icase(t->branch, app->thread_search)) return true;
    if (p && contains_icase(p->name, app->thread_search)) return true;
    return false;
}

/* Returns false if the thread was deleted during this frame. */
static bool draw_thread_card(mcode_app *app, cui_ctx *ui, int idx, bool show_project_name) {
    mcode_thread *t = &app->store.threads[idx];
    const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
    bool is_selected = strcmp(app->store.selected_thread_id, t->id) == 0;
    bool running = (t->active_run != NULL);
    bool last_failed = (t->turn_count > 0 && t->turns[t->turn_count - 1].failed);

    cui_push_id(ui, t->id);
    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "tcard",
        .width = CUI_FILL,
        .pad_x = 9,
        .pad_y = 7,
        .gap = 4,
        .hoverable = true,
        .selected = is_selected,
        .radius = 8,
    });

    /* Top row: live status indicator + title + timer/diff */
    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
        if (running) {
            cui_spinner(ui, 12);
        } else if (last_failed) {
            cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 13, CUI_COLOR_DANGER);
        } else if (t->state == MCODE_THREAD_SETTLED) {
            cui_icon(ui, CUI_ICON_CIRCLE_CHECK, 13, CUI_COLOR_SUCCESS);
        } else if (t->state == MCODE_THREAD_SNOOZED) {
            cui_icon(ui, CUI_ICON_CLOCK, 13, CUI_COLOR_WARNING);
        } else if (t->turn_count > 0) {
            cui_icon(ui, CUI_ICON_CIRCLE_DOT, 13, CUI_COLOR_SUCCESS);
        } else {
            cui_icon(ui, CUI_ICON_CIRCLE, 11.5f, CUI_COLOR_TEXT_MUTED);
        }

        cui_text(ui, t->title, .font = CUI_FONT_MEDIUM, .size = 12.5f,
                 .color = (t->state == MCODE_THREAD_SNOOZED) ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_FOREGROUND,
                 .width = CUI_FILL, .nowrap = true);

        if (running) {
            char elapsed[24];
            float secs = (float)(mcode_now_mono() - t->run_started_mono);
            mcode_ui_format_elapsed(secs, elapsed, sizeof elapsed);
            cui_text(ui, elapsed, .font = CUI_FONT_MONO, .size = 11,
                     .color = CUI_COLOR_ACCENT, .tabular = true);
        } else if (t->total_added > 0 || t->total_removed > 0) {
            char stat[32];
            snprintf(stat, sizeof stat, "+%d -%d", t->total_added, t->total_removed);
            cui_text(ui, stat, .font = CUI_FONT_MONO, .size = 10.5f,
                     .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
        }
    }

    /* Bottom row: agent badge + branch (and project name only in Settled bucket) */
    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
        cui_tone ag_tone = (t->agent == APP_AGENT_CLAUDE) ? CUI_TONE_WARNING
                         : (t->agent == APP_AGENT_CODEX)  ? CUI_TONE_SUCCESS
                         : CUI_TONE_INFO;
        cui_badge(ui, mcode_agent_short_label(t->agent), .tone = ag_tone, .size = CUI_SIZE_SM);

        char meta[192] = "";
        if (show_project_name && p && t->branch[0]) {
            snprintf(meta, sizeof meta, "%s · %s", p->name, t->branch);
        } else if (show_project_name && p) {
            snprintf(meta, sizeof meta, "%s", p->name);
        } else if (t->branch[0]) {
            snprintf(meta, sizeof meta, "%s%s",
                     t->branch,
                     (t->state == MCODE_THREAD_SNOOZED) ? " · snoozed" : "");
        } else if (t->state == MCODE_THREAD_SNOOZED) {
            snprintf(meta, sizeof meta, "Snoozed");
        }
        if (meta[0]) {
            cui_text(ui, meta, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                     .width = CUI_FILL, .nowrap = true);
        }
    }

    cui_card_end(ui);

    bool deleted = false;
    cui_context_menu(ui, "tmenu") {
        if (t->state != MCODE_THREAD_ACTIVE) {
            if (cui_menu_item(ui, "Move to Active", .icon = CUI_ICON_INBOX)) {
                t->state = MCODE_THREAD_ACTIVE;
                mcode_ui_mark_dirty(app);
            }
        }
        if (t->state != MCODE_THREAD_SNOOZED) {
            if (cui_menu_item(ui, "Snooze", .icon = CUI_ICON_CLOCK)) {
                t->state = MCODE_THREAD_SNOOZED;
                mcode_ui_mark_dirty(app);
            }
        }
        if (t->state != MCODE_THREAD_SETTLED) {
            if (cui_menu_item(ui, "Mark Settled", .icon = CUI_ICON_CHECK)) {
                t->state = MCODE_THREAD_SETTLED;
                mcode_ui_mark_dirty(app);
            }
        }
        if (running) {
            if (cui_menu_item(ui, "Stop Turn", .icon = CUI_ICON_SQUARE)) {
                mcode_ui_stop_thread(app, ui, t);
            }
        }
        cui_menu_separator(ui);
        if (cui_menu_item(ui, "Open Folder in Finder", .icon = CUI_ICON_FOLDER_OPEN)) {
            if (t->workdir[0] && !cui_app_open_url(t->workdir)) {
                app_chrome_notify(app->chrome, ui, "Could not open folder.", CUI_TONE_DANGER);
            }
        }
        if (cui_menu_item(ui, "Refresh Diff", .icon = CUI_ICON_REFRESH_CW)) {
            mcode_ui_refresh_thread_diff(t);
        }
        if (cui_menu_item(ui, t->use_worktree ? "Delete Thread & Worktree" : "Delete Thread",
                          .icon = CUI_ICON_TRASH_2, .danger = true)) {
            char del_id[MCODE_ID_MAX];
            snprintf(del_id, sizeof del_id, "%s", t->id);
            mcode_ui_delete_thread(app, ui, del_id);
            deleted = true;
        }
    }

    if (clicked && !deleted) {
        mcode_ui_select_thread(app, t->id);
    }
    cui_pop_id(ui);
    return !deleted;
}

/* Draws one collapsible project section and its non-settled threads.
 * Returns false if the project was removed during this frame. */
static bool draw_project_section(mcode_app *app, cui_ctx *ui, int proj_idx) {
    mcode_project *p = &app->store.projects[proj_idx];
    char proj_id[MCODE_ID_MAX];
    snprintf(proj_id, sizeof proj_id, "%s", p->id);

    bool searching = (app->thread_search[0] != '\0');
    bool proj_name_matches = !searching || contains_icase(p->name, app->thread_search);

    int open_count = 0;
    int matching_open_count = 0;
    int running_count = 0;
    bool has_selected_thread = false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (strcmp(t->project_id, proj_id) != 0) continue;
        if (t->state == MCODE_THREAD_SETTLED) continue;
        ++open_count;
        if (t->active_run) ++running_count;
        if (strcmp(app->store.selected_thread_id, t->id) == 0) has_selected_thread = true;
        if (proj_name_matches || thread_matches_query(app, t, p)) ++matching_open_count;
    }

    if (searching && !proj_name_matches && matching_open_count == 0) {
        return true;
    }

    bool expanded = searching || !p->collapsed;
    bool is_active_proj = has_selected_thread ||
                          (strcmp(app->store.selected_project_id, proj_id) == 0);
    bool removed = false;

    cui_push_id(ui, proj_id);

    cui_column(ui, .width = CUI_FILL, .gap = 5) {
        /* Project header row: clickable folder toggle + sibling 1-click "+" button */
        cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
            cui_id hid = cui_make_id(ui, "phdr");
            cui_rect hr = cui_get_rect(ui, hid);
            cui_vec2 mp = cui_mouse_pos(ui);
            bool hov = hr.w > 0.0f && mp.x >= hr.x && mp.x <= hr.x + hr.w &&
                       mp.y >= hr.y && mp.y <= hr.y + hr.h;
            cui_color hbg = hov
                ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.06f)
                : CUI_RGBA(0x00000001u);

            bool hdr_clicked = cui_card_begin(ui, &(cui_card_opts){
                .id = "phdr",
                .dir = CUI_ROW,
                .align = CUI_ALIGN_CENTER,
                .width = CUI_FILL,
                .pad_x = 6,
                .pad_y = 5,
                .gap = 6,
                .hoverable = true,
                .border = -1.0f,
                .radius = 7,
                .bg = hbg,
            });
            cui_icon(ui, expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                     13, CUI_COLOR_TEXT_MUTED);
            cui_icon(ui, expanded ? CUI_ICON_FOLDER_OPEN : CUI_ICON_FOLDER,
                     14, is_active_proj ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, p->name, .font = CUI_FONT_MEDIUM, .size = 13,
                     .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL, .nowrap = true);
            if (running_count > 0) {
                cui_spinner(ui, 12);
            } else if (!expanded && open_count > 0) {
                char cnt[16];
                snprintf(cnt, sizeof cnt, "%d", open_count);
                cui_badge(ui, cnt, .size = CUI_SIZE_SM);
            }
            cui_card_end(ui);

            char tip[MCODE_PATH_MAX + 32];
            mcode_git_shorten_home_path(p->path, tip, sizeof tip);
            cui_tooltip(ui, tip);

            /* Right-click project management menu */
            cui_context_menu(ui, "pmenu") {
                if (cui_menu_item(ui, "New Thread", .icon = CUI_ICON_PLUS)) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur) {
                        cur->collapsed = false;
                        mcode_ui_quick_new_thread(app, ui, cur);
                    }
                }
                if (cui_menu_item(ui, "New Thread (Custom Options...)",
                                  .icon = CUI_ICON_SLIDERS_HORIZONTAL)) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur) mcode_ui_open_new_thread_dialog(app, cur);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "Open Folder in Finder", .icon = CUI_ICON_FOLDER_OPEN)) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur && cur->path[0] && !cui_app_open_url(cur->path)) {
                        app_chrome_notify(app->chrome, ui, "Could not open folder.", CUI_TONE_DANGER);
                    }
                }
                if (cui_menu_item(ui, "Setup & Quick Actions...", .icon = CUI_ICON_SQUARE_TERMINAL)) {
                    mcode_ui_open_project_actions_for(app, ui, proj_id);
                }
                if (!p->is_git_repo) {
                    if (cui_menu_item(ui, "Initialize Git Repository", .icon = CUI_ICON_GIT_BRANCH)) {
                        mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                        if (cur) mcode_ui_init_project_git(app, ui, cur);
                    }
                }
                if (cui_menu_item(ui, "Rename Project...", .icon = CUI_ICON_PENCIL)) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur) mcode_ui_open_rename_project(app, cur);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "Remove from Mcode", .icon = CUI_ICON_TRASH_2, .danger = true)) {
                    mcode_ui_remove_project(app, ui, proj_id);
                    removed = true;
                }
            }

            if (hdr_clicked && !removed) {
                mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                if (cur) {
                    if (!searching) cur->collapsed = !cur->collapsed;
                    snprintf(app->store.selected_project_id,
                             sizeof app->store.selected_project_id, "%s", cur->id);
                    mcode_ui_mark_dirty(app);
                }
            }

            if (!removed) {
                char add_tip[192];
                snprintf(add_tip, sizeof add_tip, "New thread in %s", p->name);
                if (cui_icon_button(ui, CUI_ICON_PLUS, .size = CUI_SIZE_SM,
                                    .variant = CUI_BUTTON_GHOST, .tooltip = add_tip)) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur) {
                        cur->collapsed = false;
                        mcode_ui_quick_new_thread(app, ui, cur);
                    }
                }
            }
        }

        /* Nested threads under this project */
        if (!removed && expanded) {
            cui_column(ui, .width = CUI_FILL, .pad_left = 10, .gap = 5) {
                int drawn = 0;
                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                    mcode_thread *t = &app->store.threads[i];
                    if (strcmp(t->project_id, proj_id) != 0) continue;
                    if (t->state == MCODE_THREAD_SETTLED) continue;
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (!proj_name_matches && !thread_matches_query(app, t, cur)) continue;
                    ++drawn;
                    if (!draw_thread_card(app, ui, i, false)) {
                        if (i > app->store.thread_count) i = app->store.thread_count;
                    }
                }
                if (drawn == 0 && !searching) {
                    if (cui_button(ui, "New thread", .variant = CUI_BUTTON_GHOST,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_PLUS,
                                   .full_width = true)) {
                        mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                        if (cur) mcode_ui_quick_new_thread(app, ui, cur);
                    }
                }
            }
        }
    }

    cui_pop_id(ui);
    return !removed;
}

void mcode_ui_draw_sidebar(mcode_app *app, cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_SURFACE_MUTED, .pad = 10, .gap = 10) {

        /* Brand & primary "+ Project" button */
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_SPARKLES, 16, CUI_COLOR_ACCENT);
            cui_text(ui, "Mcode", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_spacer(ui);
            if (cui_button(ui, "Project", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_PLUS,
                           .tooltip = "Open, create, or clone a project",
                           .shortcut = "Cmd Shift O")) {
                mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
            }
        }

        /* Filter projects & threads */
        cui_search(ui, "thread-search", app->thread_search, sizeof app->thread_search,
                   .placeholder = "Filter projects & threads...", .width = CUI_FILL);

        /* Project tree + settled footer */
        cui_scroll(ui, .id = "sidebar-threads", .width = CUI_FILL, .height = CUI_FILL, .gap = 8) {
            if (app->store.project_count == 0) {
                cui_column(ui, .width = CUI_FILL, .pad_y = 20, .pad_x = 6, .gap = 10,
                           .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_FOLDER_OPEN, 22, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, "No projects yet", .font = CUI_FONT_MEDIUM, .size = 13);
                    cui_muted(ui, "Open a repo or create a new folder to start.");
                    cui_column(ui, .width = CUI_FILL, .gap = 6) {
                        if (cui_button(ui, "Open Folder", .variant = CUI_BUTTON_PRIMARY,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_FOLDER_OPEN,
                                       .full_width = true)) {
                            mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
                        }
                        if (cui_button(ui, "New Project", .variant = CUI_BUTTON_OUTLINE,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_PLUS,
                                       .full_width = true)) {
                            mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CREATE);
                        }
                    }
                }
            } else {
                for (int pi = 0; pi < app->store.project_count; ++pi) {
                    if (!draw_project_section(app, ui, pi)) {
                        --pi;
                    }
                }

                /* Orphan threads (defensive fallback if a thread has no project) */
                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                    mcode_thread *t = &app->store.threads[i];
                    if (t->state == MCODE_THREAD_SETTLED) continue;
                    if (mcode_store_find_project(&app->store, t->project_id)) continue;
                    if (!thread_matches_query(app, t, NULL)) continue;
                    if (!draw_thread_card(app, ui, i, true)) {
                        if (i > app->store.thread_count) i = app->store.thread_count;
                    }
                }
            }

            /* Collapsible Settled threads across projects */
            int settled_count = 0;
            for (int i = 0; i < app->store.thread_count; ++i) {
                const mcode_thread *t = &app->store.threads[i];
                if (t->state != MCODE_THREAD_SETTLED) continue;
                const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
                if (thread_matches_query(app, t, p)) ++settled_count;
            }
            if (settled_count > 0) {
                cui_separator(ui);
                char hdr[48];
                snprintf(hdr, sizeof hdr, "Settled (%d)###settled-toggle", settled_count);
                if (cui_button(ui, hdr, .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = app->store.settled_expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                               .full_width = true)) {
                    app->store.settled_expanded = !app->store.settled_expanded;
                    mcode_ui_mark_dirty(app);
                }
                if (app->store.settled_expanded) {
                    for (int i = app->store.thread_count - 1; i >= 0; --i) {
                        mcode_thread *t = &app->store.threads[i];
                        if (t->state != MCODE_THREAD_SETTLED) continue;
                        const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
                        if (!thread_matches_query(app, t, p)) continue;
                        if (!draw_thread_card(app, ui, i, true)) {
                            if (i > app->store.thread_count) i = app->store.thread_count;
                        }
                    }
                }
            }
        }
    }
}
