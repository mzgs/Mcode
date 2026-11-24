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

static bool rect_contains(cui_rect r, cui_vec2 p) {
    return r.w > 0.0f && r.h > 0.0f &&
           p.x >= r.x && p.x <= r.x + r.w &&
           p.y >= r.y && p.y <= r.y + r.h;
}

static bool thread_matches_query(mcode_app *app, const mcode_thread *t, const mcode_project *p) {
    if (!app->thread_search[0]) return true;
    if (contains_icase(t->title, app->thread_search)) return true;
    if (contains_icase(t->branch, app->thread_search)) return true;
    if (p && contains_icase(p->name, app->thread_search)) return true;
    return false;
}

static cui_color sidebar_surface_bg(cui_ctx *ui) {
    return (cui_get_theme_mode(ui) == CUI_THEME_DARK)
        ? CUI_RGBA(0x000000ffu)
        : cui_color_of(ui, CUI_COLOR_SURFACE_MUTED);
}

static cui_color sidebar_hover_bg(cui_ctx *ui, float strength) {
    cui_color base = sidebar_surface_bg(ui);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    return cui_mix(base, fg, strength);
}

static void draw_thread_status_glyph(cui_ctx *ui, const mcode_thread *t,
                                     bool running, bool needs_review, bool last_failed) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    /* Fixed 14x14 slot keeps all thread titles aligned on a single vertical axis. */
    cui_box(ui, .width = CUI_PX(14), .height = CUI_PX(14),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (needs_review) {
            cui_icon(ui, CUI_ICON_SHIELD, 12.5f, CUI_COLOR_WARNING);
        } else if (running) {
            cui_spinner(ui, 11.5f);
        } else if (last_failed) {
            cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 12.5f, CUI_COLOR_DANGER);
        } else if (t->state == MCODE_THREAD_SETTLED) {
            cui_icon(ui, CUI_ICON_CHECK, 12.0f,
                     cui_alpha(cui_color_of(ui, CUI_COLOR_SUCCESS), 0.78f));
        } else if (t->state == MCODE_THREAD_SNOOZED) {
            cui_icon(ui, CUI_ICON_CLOCK, 12.0f,
                     cui_alpha(cui_color_of(ui, CUI_COLOR_WARNING), 0.85f));
        } else if (t->turn_count > 0) {
            cui_box(ui, .width = CUI_PX(6), .height = CUI_PX(6), .radius = 3.0f,
                    .bg = CUI_COLOR_SUCCESS) {}
        } else {
            cui_box(ui, .width = CUI_PX(6), .height = CUI_PX(6), .radius = 3.0f,
                    .border = 1.25f,
                    .border_color = cui_alpha(fg, dark ? 0.30f : 0.28f)) {}
        }
    }
}

/* Returns false if the thread was deleted during this frame. */
static bool draw_thread_row(mcode_app *app, cui_ctx *ui, int idx, bool show_project_name) {
    mcode_thread *t = &app->store.threads[idx];
    const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
    bool is_selected = strcmp(app->store.selected_thread_id, t->id) == 0;
    bool running = (t->active_run != NULL);
    bool needs_review = (running && t->pending_approval.active);
    bool last_failed = (t->turn_count > 0 && t->turns[t->turn_count - 1].failed);
    bool muted_state = (t->state == MCODE_THREAD_SNOOZED || t->state == MCODE_THREAD_SETTLED);

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color warn = cui_color_of(ui, CUI_COLOR_WARNING);

    cui_push_id(ui, t->id);

    cui_id row_id = cui_make_id(ui, "trow");
    bool hov = rect_contains(cui_get_rect(ui, row_id), cui_mouse_pos(ui));

    cui_card_opts row_opts = {
        .id = "trow",
        .width = CUI_FILL,
        .pad_x = 7,
        .pad_y = 6,
        .gap = 2,
        .hoverable = true,
        .border = -1.0f,
        .radius = 7.0f,
        .bg = CUI_RGBA(0x00000001u),
    };

    if (needs_review) {
        cui_color base = sidebar_surface_bg(ui);
        row_opts.bg = cui_mix(base, warn, is_selected ? (dark ? 0.16f : 0.14f)
                                                      : (hov ? 0.12f : 0.09f));
        row_opts.border = 1.0f;
        row_opts.border_color = cui_alpha(warn, is_selected ? 0.45f : 0.28f);
    } else if (is_selected) {
        row_opts.bg = dark ? CUI_RGBA(0x18181cffu) : CUI_RGBA(0xffffffffu);
        row_opts.border = 1.0f;
        row_opts.border_color = dark ? CUI_RGBA(0x28282effu) : cui_alpha(fg, 0.10f);
    } else if (hov) {
        row_opts.bg = sidebar_hover_bg(ui, dark ? 0.06f : 0.045f);
    }

    bool clicked = cui_card_begin(ui, &row_opts);

    /* Top line: aligned status glyph + title + live timer/review + subtle provider icon */
    cui_row(ui, .width = CUI_FILL, .gap = 7, .align = CUI_ALIGN_CENTER) {
        draw_thread_status_glyph(ui, t, running, needs_review, last_failed);

        cui_color title_color;
        if (is_selected) {
            title_color = CUI_COLOR_FOREGROUND;
        } else if (muted_state) {
            title_color = CUI_COLOR_TEXT_MUTED;
        } else {
            title_color = cui_mix(fg, cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY), 0.30f);
        }
        cui_text(ui, t->title,
                 .font = is_selected ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                 .size = 12.5f,
                 .color = title_color,
                 .width = CUI_FILL,
                 .nowrap = true);

        if (needs_review) {
            cui_badge(ui, "Review", .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
        } else if (running) {
            char elapsed[24];
            float secs = (float)(mcode_now_mono() - t->run_started_mono);
            mcode_ui_format_elapsed(secs, elapsed, sizeof elapsed);
            cui_text(ui, elapsed, .font = CUI_FONT_MONO, .size = 10.5f,
                     .color = CUI_COLOR_TEXT_SECONDARY, .tabular = true);
        }

        float icon_alpha = is_selected ? 0.65f
                         : muted_state ? 0.26f
                         : hov         ? 0.55f
                                       : 0.36f;
        cui_color icon_tint = cui_alpha(cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY), icon_alpha);
        mcode_ui_draw_provider_icon(app, ui, t->agent, 13.0f, icon_tint);
    }

    /* Optional second line: cleanly indented under the title text */
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
        cui_color meta_color = cui_alpha(
            cui_color_of(ui, CUI_COLOR_TEXT_MUTED),
            is_selected ? 0.92f : 0.78f);
        cui_row(ui, .width = CUI_FILL, .pad_left = 21, .gap = 4, .align = CUI_ALIGN_CENTER) {
            if (t->use_worktree && t->branch[0] && !show_project_name) {
                cui_icon(ui, CUI_ICON_GIT_BRANCH, 10.0f, cui_alpha(meta_color, 0.85f));
            }
            cui_text(ui, meta, .size = 11.0f, .color = meta_color,
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
 * Sets *out_visible when the section was visible (not filtered out).
 * Returns false if the project was removed during this frame. */
static bool draw_project_section(mcode_app *app, cui_ctx *ui, int proj_idx, bool *out_visible) {
    mcode_project *p = &app->store.projects[proj_idx];
    char proj_id[MCODE_ID_MAX];
    snprintf(proj_id, sizeof proj_id, "%s", p->id);

    bool searching = (app->thread_search[0] != '\0');
    bool proj_name_matches = !searching || contains_icase(p->name, app->thread_search);

    int matching_open_count = 0;
    int running_count = 0;
    int review_count = 0;
    bool has_selected_thread = false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->is_draft) continue;
        if (strcmp(t->project_id, proj_id) != 0) continue;
        if (t->state == MCODE_THREAD_SETTLED) continue;
        if (t->active_run) {
            ++running_count;
            if (t->pending_approval.active) ++review_count;
        }
        if (strcmp(app->store.selected_thread_id, t->id) == 0) has_selected_thread = true;
        if (proj_name_matches || thread_matches_query(app, t, p)) ++matching_open_count;
    }

    if (searching && !proj_name_matches && matching_open_count == 0) {
        return true;
    }
    if (out_visible) *out_visible = true;

    bool expanded = searching || !p->collapsed;
    bool is_active_proj = has_selected_thread ||
                          (strcmp(app->store.selected_project_id, proj_id) == 0);
    bool removed = false;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    cui_push_id(ui, proj_id);

    cui_column(ui, .width = CUI_FILL, .gap = 3) {
        /* Project header row: clickable folder toggle + subtle 1-click "+" action */
        cui_vec2 mp = cui_mouse_pos(ui);
        cui_id hid = cui_make_id(ui, "phdr");
        cui_id pid = cui_make_id(ui, "padd");
        bool hdr_hov = rect_contains(cui_get_rect(ui, hid), mp);
        bool add_hov = rect_contains(cui_get_rect(ui, pid), mp);
        bool row_hov = hdr_hov || add_hov;

        cui_row(ui, .width = CUI_FILL, .gap = 2, .align = CUI_ALIGN_CENTER) {
            cui_color hbg = hdr_hov
                ? sidebar_hover_bg(ui, dark ? 0.065f : 0.045f)
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
                .radius = 7.0f,
                .bg = hbg,
            });
            cui_icon(ui, expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                     11.5f, cui_alpha(fg, hdr_hov ? 0.65f : 0.40f));

            cui_color folder_col;
            if (is_active_proj && cui_get_accent(ui) != CUI_ACCENT_NEUTRAL) {
                folder_col = CUI_COLOR_ACCENT;
            } else if (is_active_proj || expanded) {
                folder_col = CUI_COLOR_TEXT_SECONDARY;
            } else {
                folder_col = CUI_COLOR_TEXT_MUTED;
            }
            cui_icon(ui, expanded ? CUI_ICON_FOLDER_OPEN : CUI_ICON_FOLDER,
                     13.5f, folder_col);

            cui_text(ui, p->name,
                     .font = CUI_FONT_MEDIUM,
                     .size = 12.5f,
                     .color = (expanded || is_active_proj)
                                  ? CUI_COLOR_FOREGROUND
                                  : CUI_COLOR_TEXT_SECONDARY,
                     .width = CUI_FILL,
                     .nowrap = true);

            if (review_count > 0) {
                char rlbl[24];
                if (review_count == 1) snprintf(rlbl, sizeof rlbl, "Review");
                else snprintf(rlbl, sizeof rlbl, "Review (%d)", review_count);
                cui_badge(ui, rlbl, .tone = CUI_TONE_WARNING, .dot = true, .size = CUI_SIZE_SM);
            } else if (running_count > 0) {
                cui_spinner(ui, 11.5f);
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
                cui_color add_bg = add_hov
                    ? sidebar_hover_bg(ui, dark ? 0.12f : 0.08f)
                    : CUI_RGBA(0x00000001u);
                cui_color add_ic = (add_hov || row_hov)
                    ? CUI_COLOR_FOREGROUND
                    : cui_mix(fg, cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY), 0.35f);
                bool add_clicked = cui_card_begin(ui, &(cui_card_opts){
                    .id = "padd",
                    .width = CUI_PX(24),
                    .height = CUI_PX(24),
                    .pad = 1,
                    .hoverable = true,
                    .radius = 6.0f,
                    .bg = add_bg,
                    .border = -1.0f,
                });
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_icon(ui, CUI_ICON_PLUS, 13.5f, add_ic);
                }
                cui_card_end(ui);
                cui_tooltip(ui, add_tip);
                if (add_clicked) {
                    mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                    if (cur) {
                        cur->collapsed = false;
                        mcode_ui_quick_new_thread(app, ui, cur);
                    }
                }
            }
        }

        /* Nested threads under this project with a delicate vertical tree rail */
        if (!removed && expanded) {
            cui_color rail = cui_alpha(fg, dark ? 0.09f : 0.08f);
            cui_row(ui, .width = CUI_FILL, .pad_left = 11, .pad_bottom = 4, .gap = 6,
                    .align = CUI_ALIGN_STRETCH) {
                cui_box(ui, .width = CUI_PX(1), .bg = rail) {}
                cui_column(ui, .width = CUI_FILL, .gap = 2) {
                    int drawn = 0;
                    for (int i = app->store.thread_count - 1; i >= 0; --i) {
                        mcode_thread *t = &app->store.threads[i];
                        if (t->is_draft) continue;
                        if (strcmp(t->project_id, proj_id) != 0) continue;
                        if (t->state == MCODE_THREAD_SETTLED) continue;
                        mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                        if (!proj_name_matches && !thread_matches_query(app, t, cur)) continue;
                        ++drawn;
                        if (!draw_thread_row(app, ui, i, false)) {
                            if (i > app->store.thread_count) i = app->store.thread_count;
                        }
                    }
                    if (drawn == 0 && !searching) {
                        cui_id nid = cui_make_id(ui, "empty-new-thr");
                        bool nhov = rect_contains(cui_get_rect(ui, nid), cui_mouse_pos(ui));
                        bool nclk = cui_card_begin(ui, &(cui_card_opts){
                            .id = "empty-new-thr",
                            .dir = CUI_ROW,
                            .align = CUI_ALIGN_CENTER,
                            .width = CUI_FILL,
                            .pad_x = 7,
                            .pad_y = 6,
                            .gap = 7,
                            .hoverable = true,
                            .border = -1.0f,
                            .radius = 7.0f,
                            .bg = nhov ? sidebar_hover_bg(ui, dark ? 0.06f : 0.045f)
                                       : CUI_RGBA(0x00000001u),
                        });
                        cui_box(ui, .width = CUI_PX(14), .height = CUI_PX(14),
                                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                            cui_icon(ui, CUI_ICON_PLUS, 12.0f,
                                     nhov ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_TEXT_MUTED);
                        }
                        cui_text(ui, "New thread", .size = 12.0f,
                                 .color = nhov ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_TEXT_MUTED,
                                 .width = CUI_FILL, .nowrap = true);
                        cui_card_end(ui);
                        if (nclk) {
                            mcode_project *cur = mcode_store_find_project(&app->store, proj_id);
                            if (cur) mcode_ui_quick_new_thread(app, ui, cur);
                        }
                    }
                }
            }
        }
    }

    cui_pop_id(ui);
    return !removed;
}

typedef struct {
    bool hovered;
    bool pressed;
} folder_plus_paint;

static void paint_folder_plus_btn(cui_ctx *ui, cui_rect r, void *user) {
    const folder_plus_paint *p = (const folder_plus_paint *)user;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color base_bg = dark ? CUI_RGBA(0x0e0e11ffu) : cui_color_of(ui, CUI_COLOR_SURFACE);
    cui_color bg = p->pressed ? cui_mix(base_bg, fg, dark ? 0.14f : 0.08f)
                 : p->hovered ? cui_mix(base_bg, fg, dark ? 0.08f : 0.04f)
                              : base_bg;
    cui_color bdr = dark ? (p->hovered ? CUI_RGBA(0x2e2e36ffu) : CUI_RGBA(0x202026ffu))
                         : (p->hovered ? cui_color_of(ui, CUI_COLOR_BORDER_STRONG)
                                       : cui_color_of(ui, CUI_COLOR_BORDER));
    cui_draw_rect_border(ui, r, CUI_CORNERS(8.0f), bg, bdr, 1.0f);

    cui_color ic = p->hovered ? fg : cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);
    cui_draw_icon(ui, CUI_ICON_FOLDER, r, 15.0f, ic);

    /* Centered "+" inside the Lucide folder body (matching Lucide folder-plus) */
    float cx = r.x + r.w * 0.5f;
    float cy = r.y + r.h * 0.5f + 1.0f;
    float arm = 2.0f;
    cui_draw_line(ui, (cui_vec2){ cx - arm, cy }, (cui_vec2){ cx + arm, cy }, 1.25f, ic);
    cui_draw_line(ui, (cui_vec2){ cx, cy - arm }, (cui_vec2){ cx, cy + arm }, 1.25f, ic);
}

void mcode_ui_draw_sidebar(mcode_app *app, cui_ctx *ui) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color sidebar_bg = sidebar_surface_bg(ui);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = sidebar_bg, .pad_x = 10, .pad_top = 10, .pad_bottom = 8, .gap = 10) {

        /* Unified filter input + add-project button */
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_color search_bg = dark ? CUI_RGBA(0x0e0e11ffu)
                                       : cui_color_of(ui, CUI_COLOR_SURFACE);
            cui_color search_bdr = dark ? CUI_RGBA(0x202026ffu)
                                        : cui_color_of(ui, CUI_COLOR_BORDER);
            cui_search(ui, "thread-search", app->thread_search, sizeof app->thread_search,
                       .placeholder = "Filter threads...",
                       .size = CUI_SIZE_SM,
                       .width = CUI_FILL,
                       .bg = search_bg,
                       .border_color = search_bdr,
                       .radius = 8.0f);

            cui_id aid = cui_make_id(ui, "add-project-btn");
            bool ahov = rect_contains(cui_get_rect(ui, aid), cui_mouse_pos(ui));
            folder_plus_paint fpp = { ahov, ahov && cui_mouse_down(ui, CUI_MOUSE_LEFT) };
            cui_canvas_result acr = cui_canvas(ui, "add-project-btn", CUI_PX(30), CUI_PX(30),
                                               paint_folder_plus_btn, &fpp, sizeof fpp);
            cui_tooltip(ui, "Open, create, or clone a project  (Cmd Shift O)");
            if (acr.clicked) {
                mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
            }
        }

        /* Project tree + settled section */
        cui_scroll(ui, .id = "sidebar-threads", .width = CUI_FILL, .height = CUI_FILL,
                   .gap = 6, .fade_edges = true, .fade_color = sidebar_bg) {
            bool searching = (app->thread_search[0] != '\0');
            bool any_visible = false;

            if (app->store.project_count == 0) {
                cui_column(ui, .width = CUI_FILL, .pad_top = 36, .pad_bottom = 16,
                           .pad_x = 12, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_box(ui, .width = CUI_PX(36), .height = CUI_PX(36), .radius = 10,
                            .bg = cui_alpha(fg, dark ? 0.06f : 0.05f),
                            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                        cui_icon(ui, CUI_ICON_FOLDER_OPEN, 16, CUI_COLOR_TEXT_MUTED);
                    }
                    cui_text(ui, "No projects yet", .font = CUI_FONT_MEDIUM, .size = 12.5f,
                             .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_text(ui, "Open a repo or create a folder to start.",
                             .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED,
                             .align = CUI_TEXT_CENTER);
                    cui_gap(ui, 4);
                    cui_column(ui, .width = CUI_FILL, .gap = 5) {
                        if (cui_button(ui, "Open Folder", .variant = CUI_BUTTON_SECONDARY,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_FOLDER_OPEN,
                                       .full_width = true, .radius = 8.0f)) {
                            mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
                        }
                        if (cui_button(ui, "New Project", .variant = CUI_BUTTON_GHOST,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_PLUS,
                                       .full_width = true, .radius = 8.0f)) {
                            mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CREATE);
                        }
                    }
                }
                any_visible = true;
            } else {
                for (int pi = 0; pi < app->store.project_count; ++pi) {
                    if (!draw_project_section(app, ui, pi, &any_visible)) {
                        --pi;
                    }
                }

                /* Orphan threads (defensive fallback if a thread has no project) */
                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                    mcode_thread *t = &app->store.threads[i];
                    if (t->is_draft) continue;
                    if (t->state == MCODE_THREAD_SETTLED) continue;
                    if (mcode_store_find_project(&app->store, t->project_id)) continue;
                    if (!thread_matches_query(app, t, NULL)) continue;
                    any_visible = true;
                    if (!draw_thread_row(app, ui, i, true)) {
                        if (i > app->store.thread_count) i = app->store.thread_count;
                    }
                }
            }

            /* Collapsible Settled threads across projects */
            int settled_count = 0;
            for (int i = 0; i < app->store.thread_count; ++i) {
                const mcode_thread *t = &app->store.threads[i];
                if (t->is_draft) continue;
                if (t->state != MCODE_THREAD_SETTLED) continue;
                const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
                if (thread_matches_query(app, t, p)) ++settled_count;
            }
            if (settled_count > 0) {
                any_visible = true;
                cui_column(ui, .width = CUI_FILL, .pad_top = 4, .gap = 4) {
                    cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1),
                            .bg = cui_alpha(fg, dark ? 0.08f : 0.07f)) {}
                    cui_gap(ui, 2);

                    cui_id sid = cui_make_id(ui, "settled-toggle");
                    bool shov = rect_contains(cui_get_rect(ui, sid), cui_mouse_pos(ui));
                    bool sclk = cui_card_begin(ui, &(cui_card_opts){
                        .id = "settled-toggle",
                        .dir = CUI_ROW,
                        .align = CUI_ALIGN_CENTER,
                        .width = CUI_FILL,
                        .pad_x = 6,
                        .pad_y = 5,
                        .gap = 6,
                        .hoverable = true,
                        .border = -1.0f,
                        .radius = 7.0f,
                        .bg = shov ? sidebar_hover_bg(ui, dark ? 0.065f : 0.045f)
                                   : CUI_RGBA(0x00000001u),
                    });
                    cui_icon(ui, app->store.settled_expanded
                                     ? CUI_ICON_CHEVRON_DOWN
                                     : CUI_ICON_CHEVRON_RIGHT,
                             11.5f, cui_alpha(fg, shov ? 0.65f : 0.40f));
                    cui_text(ui, "Settled", .font = CUI_FONT_MEDIUM, .size = 12.0f,
                             .color = shov ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_TEXT_MUTED,
                             .width = CUI_FILL, .nowrap = true);
                    char cnt[16];
                    snprintf(cnt, sizeof cnt, "%d", settled_count);
                    cui_text(ui, cnt, .font = CUI_FONT_MONO, .size = 11.0f,
                             .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
                    cui_card_end(ui);

                    if (sclk) {
                        app->store.settled_expanded = !app->store.settled_expanded;
                        mcode_ui_mark_dirty(app);
                    }

                    if (app->store.settled_expanded) {
                        cui_color rail = cui_alpha(fg, dark ? 0.07f : 0.065f);
                        cui_row(ui, .width = CUI_FILL, .pad_left = 11, .gap = 6,
                                .align = CUI_ALIGN_STRETCH) {
                            cui_box(ui, .width = CUI_PX(1), .bg = rail) {}
                            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                                for (int i = app->store.thread_count - 1; i >= 0; --i) {
                                    mcode_thread *t = &app->store.threads[i];
                                    if (t->is_draft) continue;
                                    if (t->state != MCODE_THREAD_SETTLED) continue;
                                    const mcode_project *p = mcode_store_find_project(&app->store, t->project_id);
                                    if (!thread_matches_query(app, t, p)) continue;
                                    if (!draw_thread_row(app, ui, i, true)) {
                                        if (i > app->store.thread_count) i = app->store.thread_count;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (searching && !any_visible) {
                cui_column(ui, .width = CUI_FILL, .pad_top = 28, .pad_x = 12, .gap = 8,
                           .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_SEARCH, 18, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, "No matching threads", .font = CUI_FONT_MEDIUM, .size = 12.5f,
                             .color = CUI_COLOR_TEXT_SECONDARY);
                    if (cui_button(ui, "Clear filter", .variant = CUI_BUTTON_GHOST,
                                   .size = CUI_SIZE_SM)) {
                        app->thread_search[0] = '\0';
                    }
                }
            }
        }
    }
}
