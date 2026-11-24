#include "mcode_ui_internal.h"
#include "mcode_git.h"
#include <stdio.h>
#include <string.h>

static cui_tone tone_for_git_status(const char *st) {
    if (!st) return CUI_TONE_NEUTRAL;
    if (st[0] == 'A' || st[0] == '?') return CUI_TONE_SUCCESS;
    if (st[0] == 'D') return CUI_TONE_DANGER;
    if (st[0] == 'M' || st[0] == 'R') return CUI_TONE_WARNING;
    return CUI_TONE_NEUTRAL;
}

void mcode_ui_draw_diff(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_BACKGROUND, .gap = 0) {
        /* Diff header */
        cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 8, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Changes", .font = CUI_FONT_MEDIUM, .size = 13.5f);
            if (thread && thread->diff_file_count > 0) {
                char badge[32];
                snprintf(badge, sizeof badge, "%d", thread->diff_file_count);
                cui_badge(ui, badge, .size = CUI_SIZE_SM);

                char stats[48];
                snprintf(stats, sizeof stats, "+%d -%d", thread->total_added, thread->total_removed);
                cui_text(ui, stats, .font = CUI_FONT_MONO, .size = 11.5f,
                         .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
            }
            cui_spacer(ui);

            static const char *const modes[] = { "Split", "Inline" };
            int mode = app->store.diff_inline_mode ? 1 : 0;
            if (cui_segmented(ui, "diff-mode", modes, 2, &mode, .size = CUI_SIZE_SM)) {
                app->store.diff_inline_mode = (mode == 1);
                mcode_ui_mark_dirty(app);
            }
            if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .tooltip = "Refresh Diff",
                                .shortcut = "Cmd R")) {
                if (thread) mcode_ui_refresh_thread_diff(thread);
            }
        }
        cui_separator(ui);

        if (!thread || thread->diff_file_count == 0) {
            mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                       .pad = 24, .gap = 10) {
                if (proj && !proj->is_git_repo) {
                    cui_icon(ui, CUI_ICON_GIT_BRANCH, 24, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, "Not a Git repository", .font = CUI_FONT_MEDIUM);
                    cui_muted(ui, "Initialize Git to track agent edits and inspect diffs.");
                    if (cui_button(ui, "Initialize Git Repo", .variant = CUI_BUTTON_PRIMARY,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_GIT_BRANCH)) {
                        mcode_ui_init_project_git(app, ui, proj);
                    }
                } else {
                    cui_icon(ui, CUI_ICON_CHECK_CHECK, 24, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, "Working tree clean", .font = CUI_FONT_MEDIUM);
                    cui_muted(ui, "File edits made by the agent will appear here.");
                    if (thread) {
                        if (cui_button(ui, "Refresh", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                                       .icon = CUI_ICON_REFRESH_CW)) {
                            mcode_ui_refresh_thread_diff(thread);
                        }
                    }
                }
            }
        } else {
            /* Changed file list */
            float list_h = thread->diff_file_count <= 4
                ? (float)thread->diff_file_count * 34.0f + 12.0f
                : 152.0f;
            cui_scroll(ui, .id = "diff-file-list", .width = CUI_FILL, .height = CUI_PX(list_h),
                       .pad = 6, .gap = 4) {
                for (int i = 0; i < thread->diff_file_count; ++i) {
                    const mcode_diff_file *f = &thread->diff_files[i];
                    bool selected = (i == thread->selected_diff_file);
                    cui_push_id_int(ui, i);
                    bool clicked = cui_card_begin(ui, &(cui_card_opts){
                        .id = "dfile",
                        .width = CUI_FILL,
                        .pad_x = 8,
                        .pad_y = 5,
                        .dir = CUI_ROW,
                        .align = CUI_ALIGN_CENTER,
                        .gap = 8,
                        .hoverable = true,
                        .selected = selected,
                        .muted = !selected,
                        .radius = 6,
                    });
                    cui_badge(ui, f->status, .tone = tone_for_git_status(f->status), .size = CUI_SIZE_SM);
                    cui_text(ui, f->path, .font = CUI_FONT_MONO, .size = 12,
                             .width = CUI_FILL, .nowrap = true);
                    char delta[32];
                    snprintf(delta, sizeof delta, "+%d -%d", f->added, f->removed);
                    cui_text(ui, delta, .font = CUI_FONT_MONO, .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
                    cui_card_end(ui);
                    if (clicked) {
                        thread->selected_diff_file = i;
                        mcode_ui_load_selected_diff(thread);
                    }
                    cui_pop_id(ui);
                }
            }
            cui_separator(ui);

            /* Selected file diff viewer */
            if (thread->selected_diff_file >= 0 && thread->selected_diff_file < thread->diff_file_count) {
                const mcode_diff_file *cur = &thread->diff_files[thread->selected_diff_file];
                if (strcmp(thread->diff_loaded_path, cur->path) != 0) {
                    mcode_ui_load_selected_diff(thread);
                }
                const char *old_t = thread->diff_old_text ? thread->diff_old_text : "";
                const char *new_t = thread->diff_new_text ? thread->diff_new_text : "";

                char old_hdr[544];
                snprintf(old_hdr, sizeof old_hdr, "%s (HEAD)", cur->path);

                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6) {
                    cui_diff_result dr = cui_diff_view(ui, "mcode-diff-view", old_t, new_t,
                                                       .mode = app->store.diff_inline_mode ? CUI_DIFF_INLINE : CUI_DIFF_SPLIT,
                                                       .language = mcode_lang_from_path(cur->path),
                                                       .old_title = old_hdr,
                                                       .new_title = cur->path,
                                                       .current = &thread->diff_change_idx,
                                                       .ignore_eol = true,
                                                       .height = -1);
                    if (dr.changes > 0) {
                        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                            char hunk[48];
                            snprintf(hunk, sizeof hunk, "Change %d of %d",
                                     thread->diff_change_idx + 1, dr.changes);
                            cui_muted(ui, hunk);
                            cui_spacer(ui);
                            if (cui_icon_button(ui, CUI_ICON_ARROW_UP, .tooltip = "Previous change",
                                                .disabled = thread->diff_change_idx <= 0)) {
                                thread->diff_change_idx--;
                            }
                            if (cui_icon_button(ui, CUI_ICON_ARROW_DOWN, .tooltip = "Next change",
                                                .disabled = thread->diff_change_idx >= dr.changes - 1)) {
                                thread->diff_change_idx++;
                            }
                        }
                    }
                }
            }
        }
    }
}
