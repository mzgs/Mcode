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

static bool thread_matches_filter(mcode_app *app, const mcode_thread *t) {
    if (app->store.selected_project_id[0] &&
        strcmp(t->project_id, app->store.selected_project_id) != 0) {
        return false;
    }
    if (!app->thread_search[0]) return true;
    if (contains_icase(t->title, app->thread_search)) return true;
    if (contains_icase(t->branch, app->thread_search)) return true;
    const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
    if (p && contains_icase(p->name, app->thread_search)) return true;
    return false;
}

/* Returns false if the thread was deleted during this frame. */
static bool draw_thread_card(mcode_app *app, cui_ctx *ui, int idx) {
    mcode_thread *t = &app->store.threads[idx];
    const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
    bool is_selected = strcmp(app->store.selected_thread_id, t->id) == 0;
    bool running = (t->active_run != NULL);
    bool last_failed = (t->turn_count > 0 && t->turns[t->turn_count - 1].failed);

    cui_push_id(ui, t->id);
    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "tcard",
        .width = CUI_FILL,
        .pad_x = 10,
        .pad_y = 8,
        .gap = 5,
        .hoverable = true,
        .selected = is_selected,
        .radius = 8,
    });

    /* Top row: live status indicator + title + timer/diff */
    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
        if (running) {
            cui_spinner(ui, 13);
        } else if (last_failed) {
            cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 13, CUI_COLOR_DANGER);
        } else if (t->state == MCODE_THREAD_SETTLED) {
            cui_icon(ui, CUI_ICON_CIRCLE_CHECK, 13, CUI_COLOR_SUCCESS);
        } else if (t->state == MCODE_THREAD_SNOOZED) {
            cui_icon(ui, CUI_ICON_CLOCK, 13, CUI_COLOR_WARNING);
        } else if (t->turn_count > 0) {
            cui_icon(ui, CUI_ICON_CIRCLE_DOT, 13, CUI_COLOR_SUCCESS);
        } else {
            cui_icon(ui, CUI_ICON_CIRCLE, 12, CUI_COLOR_TEXT_MUTED);
        }

        cui_text(ui, t->title, .font = CUI_FONT_MEDIUM, .size = 13,
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
            cui_text(ui, stat, .font = CUI_FONT_MONO, .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
        }
    }

    /* Bottom row: agent badge + project/branch */
    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
        cui_tone ag_tone = (t->agent == APP_AGENT_CLAUDE) ? CUI_TONE_WARNING
                         : (t->agent == APP_AGENT_CODEX)  ? CUI_TONE_SUCCESS
                         : CUI_TONE_INFO;
        cui_badge(ui, mcode_agent_short_label(t->agent), .tone = ag_tone, .size = CUI_SIZE_SM);

        char meta[192];
        if (p && t->branch[0]) {
            snprintf(meta, sizeof meta, "%s · %s", p->name, t->branch);
        } else if (p) {
            snprintf(meta, sizeof meta, "%s", p->name);
        } else {
            snprintf(meta, sizeof meta, "%s", t->branch);
        }
        cui_text(ui, meta, .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED,
                 .width = CUI_FILL, .nowrap = true);
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

void mcode_ui_draw_sidebar(mcode_app *app, cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_SURFACE_MUTED, .pad = 10, .gap = 10) {

        /* Brand & primary create actions */
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_SPARKLES, 16, CUI_COLOR_ACCENT);
            cui_text(ui, "Mcode", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_spacer(ui);
            if (cui_icon_button(ui, CUI_ICON_FOLDER_OPEN, .tooltip = "Add Project Folder",
                                .shortcut = "Cmd Shift O")) {
                mcode_app_action_add_project(ui, app);
            }
            if (cui_button(ui, "New", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_PLUS, .tooltip = "New Thread", .shortcut = "Cmd N")) {
                mcode_app_action_new_thread(ui, app);
            }
        }

        /* Project filter row */
        if (app->store.project_count > 0) {
            const char *labels[65];
            labels[0] = "All Projects";
            int sel_idx = 0;
            int n_proj = app->store.project_count < 64 ? app->store.project_count : 64;
            for (int i = 0; i < n_proj; ++i) {
                labels[i + 1] = app->store.projects[i].name;
                if (strcmp(app->store.selected_project_id, app->store.projects[i].id) == 0) {
                    sel_idx = i + 1;
                }
            }
            cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
                if (cui_select(ui, "project-filter", labels, n_proj + 1, &sel_idx,
                               .width = CUI_FILL)) {
                    if (sel_idx == 0) {
                        app->store.selected_project_id[0] = '\0';
                    } else {
                        snprintf(app->store.selected_project_id, sizeof app->store.selected_project_id,
                                 "%s", app->store.projects[sel_idx - 1].id);
                    }
                    mcode_ui_mark_dirty(app);
                }
                if (cui_icon_button(ui, CUI_ICON_SLIDERS_HORIZONTAL,
                                    .tooltip = "Project Quick Actions & Worktree Setup")) {
                    mcode_app_action_project_actions(ui, app);
                }
            }
        }

        /* Search threads */
        cui_search(ui, "thread-search", app->thread_search, sizeof app->thread_search,
                   .placeholder = "Filter threads...", .width = CUI_FILL);

        /* Inbox thread list */
        cui_scroll(ui, .id = "sidebar-threads", .width = CUI_FILL, .height = CUI_FILL, .gap = 8) {
            int active_count = 0, snoozed_count = 0, settled_count = 0;
            for (int i = 0; i < app->store.thread_count; ++i) {
                if (!thread_matches_filter(app, &app->store.threads[i])) continue;
                if (app->store.threads[i].state == MCODE_THREAD_ACTIVE) ++active_count;
                else if (app->store.threads[i].state == MCODE_THREAD_SNOOZED) ++snoozed_count;
                else ++settled_count;
            }

            /* Active threads */
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "ACTIVE", .font = CUI_FONT_MEDIUM, .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);
                char cnt[16];
                snprintf(cnt, sizeof cnt, "%d", active_count);
                cui_badge(ui, cnt, .size = CUI_SIZE_SM);
            }

            if (active_count == 0) {
                cui_column(ui, .width = CUI_FILL, .pad_y = 12, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_muted(ui, app->store.project_count == 0
                              ? "Add a Git project to start."
                              : "No active threads.");
                }
            } else {
                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                    if (app->store.threads[i].state != MCODE_THREAD_ACTIVE) continue;
                    if (!thread_matches_filter(app, &app->store.threads[i])) continue;
                    if (!draw_thread_card(app, ui, i)) {
                        /* Array shifted; adjust index cleanly */
                        if (i > app->store.thread_count) i = app->store.thread_count;
                    }
                }
            }

            /* Snoozed threads */
            if (snoozed_count > 0) {
                cui_separator(ui);
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "SNOOZED", .font = CUI_FONT_MEDIUM, .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED);
                    char cnt[16];
                    snprintf(cnt, sizeof cnt, "%d", snoozed_count);
                    cui_badge(ui, cnt, .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
                }
                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                    if (app->store.threads[i].state != MCODE_THREAD_SNOOZED) continue;
                    if (!thread_matches_filter(app, &app->store.threads[i])) continue;
                    if (!draw_thread_card(app, ui, i)) {
                        if (i > app->store.thread_count) i = app->store.thread_count;
                    }
                }
            }

            /* Collapsible Settled threads */
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
                        if (app->store.threads[i].state != MCODE_THREAD_SETTLED) continue;
                        if (!thread_matches_filter(app, &app->store.threads[i])) continue;
                        if (!draw_thread_card(app, ui, i)) {
                            if (i > app->store.thread_count) i = app->store.thread_count;
                        }
                    }
                }
            }
        }
    }
}
