#include "mcode_ui_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *tool_icon_for_name(const char *name) {
    if (!name) return CUI_ICON_TERMINAL;
    if (strstr(name, "bash") || strstr(name, "Bash") || strstr(name, "command") || strstr(name, "shell"))
        return CUI_ICON_SQUARE_TERMINAL;
    if (strstr(name, "Read") || strstr(name, "read") || strstr(name, "view"))
        return CUI_ICON_FILE_TEXT;
    if (strstr(name, "Edit") || strstr(name, "Write") || strstr(name, "edit") || strstr(name, "file_change"))
        return CUI_ICON_PENCIL;
    if (strstr(name, "Search") || strstr(name, "Grep") || strstr(name, "Glob") || strstr(name, "search"))
        return CUI_ICON_SEARCH;
    if (strstr(name, "Web") || strstr(name, "web") || strstr(name, "fetch"))
        return CUI_ICON_GLOBE;
    return CUI_ICON_ZAP;
}

static const cui_model k_agent_models[APP_AGENT_COUNT] = {
    { "Claude Code", "Anthropic Claude CLI (stream-json)", NULL, CUI_ICON_SPARKLES },
    { "Codex", "OpenAI Codex CLI (exec --json)", NULL, CUI_ICON_TERMINAL },
    { "Antigravity CLI", "Google Antigravity CLI (agy -p)", NULL, CUI_ICON_ROCKET },
};

static const cui_prompt_suggestion k_suggestions[] = {
    { CUI_ICON_COMPASS, "Explore this codebase", "Summarize the architecture, entry points, and build setup" },
    { CUI_ICON_BUG, "Find & fix a bug", "Inspect recent changes and check for edge cases or memory leaks" },
    { CUI_ICON_LIST_CHECKS, "Write unit tests", "Add thorough tests for the core logic and run the test suite" },
    { CUI_ICON_CODE, "Refactor cleanly", "Simplify a module while preserving behavior and verifying the build" },
};

static void draw_empty_workspace(mcode_app *app, cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
               .pad = 32, .gap = 18) {
        cui_card(ui, .width = CUI_FILL, .max_width = 520, .pad = 24, .gap = 14, .radius = 14) {
            cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_SPARKLES, 24, CUI_COLOR_ACCENT);
                cui_heading(ui, "Welcome to Mcode", 3);
            }
            cui_secondary(ui,
                "Orchestrate parallel Claude Code, Codex, and Antigravity CLI threads "
                "with isolated Git worktrees, inline diff reviews, and an integrated shell.");

            /* Detected CLI pills */
            cui_row(ui, .gap = 8, .wrap = true, .align = CUI_ALIGN_CENTER) {
                for (int i = 0; i < APP_AGENT_COUNT; ++i) {
                    cui_tone tone = app->cli_status.available[i] ? CUI_TONE_SUCCESS : CUI_TONE_NEUTRAL;
                    cui_badge(ui, mcode_agent_label((app_agent_kind)i),
                              .tone = tone, .dot = true);
                }
            }

            cui_separator(ui);

            cui_row(ui, .gap = 10, .wrap = true) {
                if (app->store.project_count == 0) {
                    if (cui_button(ui, "Add Project Folder", .variant = CUI_BUTTON_PRIMARY,
                                   .icon = CUI_ICON_FOLDER_OPEN)) {
                        mcode_app_action_add_project(ui, app);
                    }
                } else {
                    if (cui_button(ui, "New Thread", .variant = CUI_BUTTON_PRIMARY,
                                   .icon = CUI_ICON_PLUS)) {
                        mcode_app_action_new_thread(ui, app);
                    }
                    if (cui_button(ui, "Add Another Project", .variant = CUI_BUTTON_SECONDARY,
                                   .icon = CUI_ICON_FOLDER_OPEN)) {
                        mcode_app_action_add_project(ui, app);
                    }
                }
                if (cui_button(ui, "Settings", .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_SETTINGS)) {
                    app->chrome->settings_open = true;
                    app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
                }
            }
        }
    }
}

static void draw_thread_header(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    const mcode_project *proj = mcode_store_find_project(&app->store, thread->project_id);
    bool running = (thread->active_run != NULL);

    cui_column(ui, .width = CUI_FILL, .gap = 0) {
        cui_row(ui, .width = CUI_FILL, .pad_x = 16, .pad_y = 10, .gap = 10,
                .align = CUI_ALIGN_CENTER) {
            /* Editable thread title & branch metadata */
            cui_column(ui, .width = CUI_FILL, .gap = 4) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    if (cui_inline_edit(ui, "thread-title", thread->title, sizeof thread->title,
                                        .font = CUI_FONT_MEDIUM, .size = 15,
                                        .placeholder = "Untitled thread") & CUI_CHANGED) {
                        mcode_ui_mark_dirty(app);
                    }
                    if (running) {
                        float secs = (float)(mcode_now_mono() - thread->run_started_mono);
                        cui_agent_status(ui, thread->status_text[0] ? thread->status_text : "Working...",
                                         .status = CUI_TASK_RUNNING, .seconds = secs);
                    } else if (thread->state == MCODE_THREAD_SETTLED) {
                        cui_badge(ui, "Settled", .tone = CUI_TONE_SUCCESS, .icon = CUI_ICON_CHECK,
                                  .size = CUI_SIZE_SM);
                    }
                }
                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .wrap = true) {
                    if (proj) {
                        cui_text(ui, proj->name, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                    }
                    if (thread->branch[0]) {
                        cui_badge(ui, thread->branch, .icon = CUI_ICON_GIT_BRANCH,
                                  .tone = thread->use_worktree ? CUI_TONE_INFO : CUI_TONE_NEUTRAL,
                                  .size = CUI_SIZE_SM);
                    }
                    if (thread->use_worktree) {
                        cui_muted(ui, "Isolated worktree");
                    }
                }
            }

            /* Right-side Git & panel actions (T3 Code top-right controls) */
            cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
                if (thread->state != MCODE_THREAD_SETTLED) {
                    if (cui_icon_button(ui, CUI_ICON_CHECK, .tooltip = "Mark thread settled")) {
                        thread->state = MCODE_THREAD_SETTLED;
                        mcode_ui_mark_dirty(app);
                    }
                } else {
                    if (cui_icon_button(ui, CUI_ICON_INBOX, .tooltip = "Reopen thread")) {
                        thread->state = MCODE_THREAD_ACTIVE;
                        mcode_ui_mark_dirty(app);
                    }
                }

                if (proj && proj->is_git_repo) {
                    if (cui_button(ui, "Commit", .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM,
                                   .icon = CUI_ICON_CHECK)) {
                        mcode_app_action_commit(ui, app);
                    }
                    if (cui_button(ui, "PR", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM,
                                   .icon = CUI_ICON_ARROW_UP_RIGHT)) {
                        mcode_app_action_create_pr(ui, app);
                    }
                }

                char diff_lbl[32];
                if (thread->diff_file_count > 0) {
                    snprintf(diff_lbl, sizeof diff_lbl, "Diff (%d)###hdr-diff", thread->diff_file_count);
                } else {
                    snprintf(diff_lbl, sizeof diff_lbl, "Diff###hdr-diff");
                }
                if (cui_button(ui, diff_lbl, .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_LAYERS, .selected = app->store.show_diff_panel,
                               .tooltip = "Toggle Diff Panel", .shortcut = "Cmd D")) {
                    mcode_app_action_toggle_diff(ui, app);
                }
                if (cui_icon_button(ui, CUI_ICON_FOLDER_OPEN,
                                    .tooltip = "Open Folder in Finder",
                                    .shortcut = "Cmd Shift F")) {
                    mcode_app_action_open_folder(ui, app);
                }
                if (cui_icon_button(ui, CUI_ICON_SQUARE_TERMINAL,
                                    .selected = app->store.show_terminal_panel,
                                    .tooltip = "Toggle Integrated Terminal",
                                    .shortcut = "Cmd J")) {
                    mcode_app_action_toggle_terminal(ui, app);
                }
            }
        }
        cui_separator(ui);
    }
}

static void draw_turn(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int turn_idx) {
    mcode_turn *turn = &thread->turns[turn_idx];
    bool is_last_turn = (turn_idx == thread->turn_count - 1);
    cui_push_id_int(ui, turn_idx);

    cui_column(ui, .width = CUI_FILL, .gap = 10) {
        /* User prompt bubble */
        cui_chat_message(ui, mcode_str_cstr(&turn->prompt), .mine = true);

        /* Assistant stream blocks */
        const char *last_text = NULL;
        for (int bi = 0; bi < turn->block_count; ++bi) {
            mcode_block *b = &turn->blocks[bi];
            char bid[48];
            snprintf(bid, sizeof bid, "blk-%d-%d", turn_idx, bi);

            if (b->kind == MCODE_BLOCK_REASONING) {
                float secs = b->thinking ? (float)(mcode_now_mono() - b->started_mono) : b->duration_sec;
                cui_reasoning(ui, bid, .thinking = b->thinking, .seconds = secs) {
                    cui_text(ui, mcode_str_cstr(&b->text), .size = 13,
                             .color = CUI_COLOR_TEXT_SECONDARY, .selectable = true);
                }
            } else if (b->kind == MCODE_BLOCK_TOOL) {
                float secs = (b->status == CUI_TASK_RUNNING)
                    ? (float)(mcode_now_mono() - b->started_mono)
                    : b->duration_sec;
                const char *inp = b->input.len > 0 ? mcode_str_cstr(&b->input) : NULL;
                const char *out = b->output.len > 0 ? mcode_str_cstr(&b->output) : NULL;
                /* Guarantee single-line summary even for older persisted threads */
                char detail_line[104] = "";
                if (b->tool_detail[0]) {
                    const char *dp = b->tool_detail;
                    if (strncmp(dp, "/bin/zsh -lc ", 13) == 0) dp += 13;
                    else if (strncmp(dp, "/bin/bash -lc ", 14) == 0) dp += 14;
                    if (*dp == '\'' || *dp == '"') ++dp;
                    size_t w = 0;
                    bool cut = false;
                    while (*dp && w < 90) {
                        if (*dp == '\n' || *dp == '\r') { cut = true; break; }
                        detail_line[w++] = *dp++;
                    }
                    if (*dp) cut = true;
                    if (cut && w + 3 < sizeof detail_line) {
                        detail_line[w++] = '.';
                        detail_line[w++] = '.';
                        detail_line[w++] = '.';
                    }
                    detail_line[w] = '\0';
                }
                cui_tool_call(ui, bid, b->tool_name[0] ? b->tool_name : "tool",
                              .icon = tool_icon_for_name(b->tool_name),
                              .detail = detail_line[0] ? detail_line : NULL,
                              .status = b->status,
                              .seconds = secs,
                              .input = inp,
                              .output = out);
            } else if (b->kind == MCODE_BLOCK_TEXT) {
                const char *txt = mcode_str_cstr(&b->text);
                if (txt[0]) {
                    last_text = txt;
                    bool streaming = turn->running && (bi == turn->block_count - 1);
                    cui_markdown_result mr = cui_markdown(ui, bid, txt,
                                                          .length = (int)b->text.len,
                                                          .streaming = streaming);
                    if (mr.link && mr.link[0]) {
                        cui_app_open_url(mr.link);
                    }
                }
            } else if (b->kind == MCODE_BLOCK_ERROR) {
                cui_alert(ui, "Agent error", .description = mcode_str_cstr(&b->text),
                          .tone = CUI_TONE_DANGER);
            }
        }

        if (turn->running && turn->block_count == 0) {
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_loader(ui, .style = CUI_LOADER_DOTS, .size = 20, .color = CUI_COLOR_ACCENT);
                cui_shimmer_text(ui, thread->status_text[0] ? thread->status_text : "Thinking...");
            }
        }

        if (!turn->running) {
            cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
                int act = cui_response_actions(ui, "resp-act",
                                               .copy_text = last_text,
                                               .vote = &turn->vote,
                                               .no_regenerate = !is_last_turn || (thread->active_run != NULL),
                                               .no_share = true);
                if (act & CUI_RESPONSE_REGENERATE) {
                    char *retry_prompt = strdup(mcode_str_cstr(&turn->prompt));
                    if (retry_prompt) {
                        mcode_ui_send_prompt(app, ui, thread, retry_prompt);
                        free(retry_prompt);
                    }
                }
                cui_spacer(ui);
                char meta[128] = "";
                if (turn->input_tokens > 0 || turn->output_tokens > 0) {
                    if (turn->cost_usd > 0.0) {
                        snprintf(meta, sizeof meta, "%.1fs · %d in / %d out · $%.4f",
                                 turn->duration_sec, turn->input_tokens, turn->output_tokens, turn->cost_usd);
                    } else {
                        snprintf(meta, sizeof meta, "%.1fs · %d in / %d out",
                                 turn->duration_sec, turn->input_tokens, turn->output_tokens);
                    }
                } else if (turn->duration_sec > 0.05f) {
                    snprintf(meta, sizeof meta, "%.1fs", turn->duration_sec);
                }
                if (meta[0]) cui_muted(ui, meta);
            }
        }
    }

    cui_pop_id(ui);
}

void mcode_ui_draw_chat(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!thread) {
        draw_empty_workspace(app, ui);
        return;
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        draw_thread_header(app, ui, thread);

        /* Scrollable transcript */
        cui_scroll(ui, .id = "chat-transcript", .width = CUI_FILL, .height = CUI_FILL,
                   .pad = 16, .gap = 16, .fade_edges = true) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER) {
                cui_column(ui, .width = CUI_FILL, .max_width = 860, .gap = 18) {
                    if (thread->turn_count == 0) {
                        cui_column(ui, .width = CUI_FILL, .pad_y = 20, .gap = 14) {
                            cui_heading(ui, thread->title, 4);
                            char sub[256];
                            snprintf(sub, sizeof sub, "Ready in %s with %s.",
                                     thread->workdir[0] ? thread->workdir : "project",
                                     mcode_agent_label(thread->agent));
                            cui_secondary(ui, sub);

                            int pick = cui_prompt_suggestions(ui, "starter-prompts",
                                                              k_suggestions,
                                                              (int)(sizeof k_suggestions / sizeof k_suggestions[0]),
                                                              .columns = 2);
                            if (pick >= 0 && pick < (int)(sizeof k_suggestions / sizeof k_suggestions[0])) {
                                char prompt_buf[320];
                                snprintf(prompt_buf, sizeof prompt_buf, "%s: %s",
                                         k_suggestions[pick].title, k_suggestions[pick].detail);
                                mcode_ui_send_prompt(app, ui, thread, prompt_buf);
                            }
                        }
                    } else {
                        for (int i = 0; i < thread->turn_count; ++i) {
                            draw_turn(app, ui, thread, i);
                        }
                    }
                }
            }
        }
        if (thread->scroll_to_bottom_frames > 0) {
            cui_scroll_to(ui, "chat-transcript", 1e9f);
            thread->scroll_to_bottom_frames--;
            if (thread->scroll_to_bottom_frames > 0) cui_request_redraw(ui);
        }

        /* Composer dock */
        cui_separator(ui);
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad_x = 16, .pad_y = 12) {
            cui_column(ui, .width = CUI_FILL, .max_width = 860, .gap = 8) {
                /* Compact pre-composer options strip */
                cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER, .wrap = true) {
                    if (!app->cli_status.available[thread->agent]) {
                        cui_badge(ui, "CLI not found", .icon = CUI_ICON_TRIANGLE_ALERT,
                                  .tone = CUI_TONE_DANGER, .size = CUI_SIZE_SM);
                    }
                    if (thread->session_id[0]) {
                        char sshort[32];
                        snprintf(sshort, sizeof sshort, "Session %.8s", thread->session_id);
                        cui_muted(ui, sshort);
                    }
                    if (thread->model[0]) {
                        cui_badge(ui, thread->model, .size = CUI_SIZE_SM);
                    }
                    cui_spacer(ui);
                    if (cui_chip(ui, "Auto-approve", &thread->auto_approve)) {
                        mcode_ui_mark_dirty(app);
                    }
                }

                int agent_sel = (int)thread->agent;
                bool busy = (thread->active_run != NULL);
                static const char *const k_first_placeholders[APP_AGENT_COUNT] = {
                    "Ask Claude Code to implement a feature, fix a bug, or inspect code...",
                    "Ask Codex to implement a feature, fix a bug, or inspect code...",
                    "Ask Antigravity CLI to implement a feature, fix a bug, or inspect code...",
                };
                static const char *const k_followup_placeholders[APP_AGENT_COUNT] = {
                    "Send a follow-up to Claude Code...",
                    "Send a follow-up to Codex...",
                    "Send a follow-up to Antigravity CLI...",
                };
                const char *placeholder = thread->turn_count == 0
                    ? k_first_placeholders[thread->agent]
                    : k_followup_placeholders[thread->agent];

                int ev = cui_chat_composer(ui, "thread-composer",
                                           thread->composer_draft, sizeof thread->composer_draft,
                                           .placeholder = placeholder,
                                           .busy = busy,
                                           .models = k_agent_models,
                                           .model_count = APP_AGENT_COUNT,
                                           .model = &agent_sel,
                                           .max_lines = 8,
                                           .width = CUI_FILL);
                if (ev & CUI_COMPOSER_ATTACH) {
                    char fpath[MCODE_PATH_MAX] = "";
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_OPEN,
                            .title = "Reference File in Prompt",
                        }, fpath, sizeof fpath)) {
                        size_t cur_len = strlen(thread->composer_draft);
                        const char *rel = fpath;
                        size_t wd_len = strlen(thread->workdir);
                        if (wd_len > 0 && strncmp(fpath, thread->workdir, wd_len) == 0 && fpath[wd_len] == '/') {
                            rel = fpath + wd_len + 1;
                        }
                        snprintf(thread->composer_draft + cur_len,
                                 sizeof thread->composer_draft - cur_len,
                                 "%s@%s ", cur_len > 0 ? " " : "", rel);
                    }
                }
                if (ev & CUI_COMPOSER_MODEL) {
                    if (!busy && agent_sel >= 0 && agent_sel < APP_AGENT_COUNT &&
                        agent_sel != (int)thread->agent) {
                        thread->agent = (app_agent_kind)agent_sel;
                        /* Reset session ID when switching agent provider */
                        thread->session_id[0] = '\0';
                        mcode_ui_mark_dirty(app);
                    }
                }
                if ((ev & CUI_COMPOSER_SEND) && thread->composer_draft[0]) {
                    char *copy = strdup(thread->composer_draft);
                    thread->composer_draft[0] = '\0';
                    if (copy) {
                        mcode_ui_send_prompt(app, ui, thread, copy);
                        free(copy);
                    }
                }
                if (ev & CUI_COMPOSER_STOP) {
                    mcode_ui_stop_thread(app, ui, thread);
                }
            }
        }
    }
}
