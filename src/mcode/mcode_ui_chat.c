#include "mcode_ui_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOOL_KIND_CMD = 0,
    TOOL_KIND_EDIT,
    TOOL_KIND_READ,
    TOOL_KIND_SEARCH,
    TOOL_KIND_WEB,
    TOOL_KIND_OTHER,
} tool_kind;

static tool_kind classify_tool_name(const char *name) {
    if (!name || !name[0]) return TOOL_KIND_OTHER;
    if (strstr(name, "bash") || strstr(name, "Bash") || strstr(name, "command") || strstr(name, "shell"))
        return TOOL_KIND_CMD;
    if (strstr(name, "Edit") || strstr(name, "Write") || strstr(name, "edit") ||
        strstr(name, "write") || strstr(name, "file_change") || strstr(name, "patch"))
        return TOOL_KIND_EDIT;
    if (strstr(name, "Read") || strstr(name, "read") || strstr(name, "view"))
        return TOOL_KIND_READ;
    if (strstr(name, "Search") || strstr(name, "Grep") || strstr(name, "Glob") ||
        strstr(name, "search") || strstr(name, "grep") || strstr(name, "glob"))
        return TOOL_KIND_SEARCH;
    if (strstr(name, "Web") || strstr(name, "web") || strstr(name, "fetch"))
        return TOOL_KIND_WEB;
    return TOOL_KIND_OTHER;
}

static const char *tool_icon_for_kind(tool_kind kind) {
    switch (kind) {
    case TOOL_KIND_CMD:    return CUI_ICON_SQUARE_TERMINAL;
    case TOOL_KIND_EDIT:   return CUI_ICON_PENCIL;
    case TOOL_KIND_READ:   return CUI_ICON_FILE_TEXT;
    case TOOL_KIND_SEARCH: return CUI_ICON_SEARCH;
    case TOOL_KIND_WEB:    return CUI_ICON_GLOBE;
    default:               return CUI_ICON_ZAP;
    }
}

static bool looks_like_json(const char *s) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    return *s == '{' || *s == '[';
}

static void format_tool_detail(const char *raw, const char *workdir, char *out, size_t cap, size_t max_chars) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;

    const char *p = raw;
    while (*p == ' ' || *p == '\t') ++p;

    static const char *const prefixes[] = {
        "/bin/zsh -lc ", "/bin/bash -lc ", "/bin/sh -c ", "zsh -lc ", "bash -lc ", "sh -c "
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], plen) == 0) {
            p += plen;
            break;
        }
    }
    char quote = 0;
    if (*p == '\'' || *p == '"') {
        quote = *p++;
    }

    const char *home = getenv("HOME");
    size_t home_len = (home && home[0]) ? strlen(home) : 0;
    size_t wd_len = (workdir && workdir[0]) ? strlen(workdir) : 0;
    while (wd_len > 1 && workdir[wd_len - 1] == '/') --wd_len;
    while (home_len > 1 && home[home_len - 1] == '/') --home_len;

    if (max_chars >= cap) max_chars = cap - 1;
    size_t w = 0;
    bool cut = false;
    bool prev_space = false;

    while (*p) {
        if (*p == '\n' || *p == '\r') {
            cut = true;
            break;
        }
        if (quote && *p == quote && p[1] == '\0') break;

        if (wd_len > 0 && strncmp(p, workdir, wd_len) == 0 && p[wd_len] == '/') {
            p += wd_len + 1;
            continue;
        }
        if (home_len > 0 && strncmp(p, home, home_len) == 0 &&
            (p[home_len] == '/' || p[home_len] == '\0' || p[home_len] == ' ')) {
            if (w + 1 > max_chars) { cut = true; break; }
            out[w++] = '~';
            p += home_len;
            prev_space = false;
            continue;
        }

        char ch = (*p == '\t') ? ' ' : *p;
        ++p;
        if (ch == ' ') {
            if (prev_space || w == 0) continue;
            prev_space = true;
        } else {
            prev_space = false;
        }

        if (w >= max_chars) {
            cut = true;
            break;
        }
        out[w++] = ch;
    }

    while (w > 0 && out[w - 1] == ' ') --w;
    if (cut) {
        if (w + 3 >= cap) w = (cap > 4) ? cap - 4 : 0;
        out[w++] = '.';
        out[w++] = '.';
        out[w++] = '.';
    }
    out[w] = '\0';
}

static void format_tool_row_title(const mcode_block *b, const char *workdir, float secs,
                                  bool include_meta, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!b) return;

    tool_kind kind = classify_tool_name(b->tool_name);
    bool running = (b->status == CUI_TASK_RUNNING);
    char detail[96] = "";
    format_tool_detail(b->tool_detail, workdir, detail, sizeof detail, 60);

    const char *verb = NULL;
    const char *fallback = NULL;
    switch (kind) {
    case TOOL_KIND_CMD:
        verb = running ? "Running" : "Ran";
        fallback = "command";
        break;
    case TOOL_KIND_EDIT:
        verb = running ? "Editing" : "Edited";
        fallback = "file";
        break;
    case TOOL_KIND_READ:
        verb = running ? "Reading" : "Read";
        fallback = "file";
        break;
    case TOOL_KIND_SEARCH:
        verb = running ? "Searching" : "Searched";
        fallback = "files";
        break;
    case TOOL_KIND_WEB:
        verb = running ? "Fetching" : "Fetched";
        fallback = "web";
        break;
    default:
        verb = b->tool_name[0] ? b->tool_name : "Tool";
        fallback = "";
        break;
    }

    int n = 0;
    if (detail[0]) {
        n = snprintf(out, cap, "%s %s", verb, detail);
    } else if (fallback[0]) {
        n = snprintf(out, cap, "%s %s", verb, fallback);
    } else {
        n = snprintf(out, cap, "%s", verb);
    }
    if (n < 0 || (size_t)n >= cap) return;

    if (include_meta) {
        if (b->status == CUI_TASK_FAILED) {
            snprintf(out + n, cap - (size_t)n, " · failed");
        } else if (secs >= 1.0f && b->status != CUI_TASK_PENDING) {
            if (secs < 10.0f) snprintf(out + n, cap - (size_t)n, " · %.1fs", (double)secs);
            else snprintf(out + n, cap - (size_t)n, " · %.0fs", (double)secs);
        }
    }
}

static void append_clause(char *buf, size_t cap, int *pos, const char *fmt, int count) {
    if (!buf || !pos || *pos < 0 || (size_t)*pos >= cap) return;
    if (*pos > 0) {
        int s = snprintf(buf + *pos, cap - (size_t)*pos, ", ");
        if (s > 0) *pos += s;
    }
    if ((size_t)*pos < cap) {
        int w = snprintf(buf + *pos, cap - (size_t)*pos, fmt, count, count == 1 ? "" : "s");
        if (w > 0) *pos += w;
    }
}

static void format_tool_group_title(const mcode_block *blocks, int start, int count,
                                    const char *workdir, char *out, size_t cap,
                                    bool *out_running, int *out_failed) {
    int n_cmd = 0, n_edit = 0, n_read = 0, n_search = 0, n_web = 0, n_other = 0;
    int n_failed = 0;
    int running_idx = -1;
    float total_secs = 0.0f;

    for (int i = 0; i < count; ++i) {
        const mcode_block *b = &blocks[start + i];
        switch (classify_tool_name(b->tool_name)) {
        case TOOL_KIND_CMD:    ++n_cmd; break;
        case TOOL_KIND_EDIT:   ++n_edit; break;
        case TOOL_KIND_READ:   ++n_read; break;
        case TOOL_KIND_SEARCH: ++n_search; break;
        case TOOL_KIND_WEB:    ++n_web; break;
        default:               ++n_other; break;
        }
        if (b->status == CUI_TASK_RUNNING) running_idx = start + i;
        else if (b->status == CUI_TASK_FAILED) ++n_failed;
        if (b->duration_sec > 0.0f) total_secs += b->duration_sec;
    }

    if (out_running) *out_running = (running_idx >= 0);
    if (out_failed) *out_failed = n_failed;

    if (running_idx >= 0) {
        format_tool_row_title(&blocks[running_idx], workdir, 0.0f, false, out, cap);
        return;
    }

    int pos = 0;
    out[0] = '\0';
    if (n_read > 0)   append_clause(out, cap, &pos, "read %d file%s", n_read);
    if (n_search > 0) {
        if (pos > 0) pos += snprintf(out + pos, cap - (size_t)pos, ", ");
        if ((size_t)pos < cap) {
            pos += snprintf(out + pos, cap - (size_t)pos, "%d %s",
                            n_search, n_search == 1 ? "search" : "searches");
        }
    }
    if (n_cmd > 0)    append_clause(out, cap, &pos, "ran %d command%s", n_cmd);
    if (n_edit > 0)   append_clause(out, cap, &pos, "edited %d file%s", n_edit);
    if (n_web > 0)    append_clause(out, cap, &pos, "%d web lookup%s", n_web);
    if (n_other > 0)  append_clause(out, cap, &pos, "%d tool call%s", n_other);

    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - ('a' - 'A'));
    }

    if (n_failed > 0 && (size_t)pos < cap) {
        pos += snprintf(out + pos, cap - (size_t)pos, " · %d failed", n_failed);
    }
    if (total_secs >= 1.0f && (size_t)pos < cap) {
        snprintf(out + pos, cap - (size_t)pos, " · %.0fs", (double)total_secs);
    }
}

static void draw_tool_payload(cui_ctx *ui, const char *id, const char *label,
                              const char *body, const char *lang_hint, bool is_error) {
    if (!body || !body[0]) return;
    cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 11.5f,
             .color = is_error ? CUI_COLOR_DANGER : CUI_COLOR_TEXT_MUTED);
    const char *lang = looks_like_json(body) ? "json" : lang_hint;
    cui_code_block(ui, id, body, .lang = lang, .max_height = 180, .size = 12, .no_header = true);
}

static void draw_tool_row(cui_ctx *ui, const mcode_block *b, const char *workdir,
                          const char *bid) {
    float secs = (b->status == CUI_TASK_RUNNING)
        ? (float)(mcode_now_mono() - b->started_mono)
        : b->duration_sec;
    const char *inp = b->input.len > 0 ? mcode_str_cstr(&b->input) : NULL;
    const char *out = b->output.len > 0 ? mcode_str_cstr(&b->output) : NULL;
    bool expandable = (inp != NULL || out != NULL);
    bool running = (b->status == CUI_TASK_RUNNING);
    bool failed = (b->status == CUI_TASK_FAILED);
    tool_kind kind = classify_tool_name(b->tool_name);

    char title[144];
    format_tool_row_title(b, workdir, secs, true, title, sizeof title);

    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_START, .clip = true) {
        cui_box(ui, .pad_top = 6) {
            if (running) {
                cui_spinner(ui, 12);
            } else if (failed) {
                cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 13, CUI_COLOR_DANGER);
            } else {
                cui_icon(ui, tool_icon_for_kind(kind), 13, CUI_COLOR_TEXT_MUTED);
            }
        }

        if (expandable) {
            cui_reasoning(ui, bid, .thinking = false, .title = title) {
                bool dup = (inp && out && strcmp(inp, out) == 0);
                const char *in_lang = (kind == TOOL_KIND_CMD) ? "bash" : NULL;
                if (dup) {
                    draw_tool_payload(ui, "out", failed ? "Error" : "Details", out, NULL, failed);
                } else {
                    if (inp) draw_tool_payload(ui, "in", kind == TOOL_KIND_CMD ? "Command" : "Input",
                                               inp, in_lang, false);
                    if (out) draw_tool_payload(ui, "out", failed ? "Error" : "Output",
                                               out, NULL, failed);
                }
            }
        } else {
            cui_box(ui, .pad_y = 4, .width = CUI_FILL) {
                cui_text(ui, title, .font = CUI_FONT_MEDIUM, .size = 13,
                         .color = failed ? CUI_COLOR_DANGER : CUI_COLOR_TEXT_SECONDARY,
                         .nowrap = true);
            }
        }
    }
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
                int end = bi + 1;
                while (end < turn->block_count && turn->blocks[end].kind == MCODE_BLOCK_TOOL) {
                    ++end;
                }
                int count = end - bi;
                if (count == 1) {
                    draw_tool_row(ui, b, thread->workdir, bid);
                } else {
                    char grp_id[48];
                    snprintf(grp_id, sizeof grp_id, "tgrp-%d-%d", turn_idx, bi);
                    char grp_title[160];
                    bool any_running = false;
                    int failed_count = 0;
                    format_tool_group_title(turn->blocks, bi, count, thread->workdir,
                                            grp_title, sizeof grp_title, &any_running, &failed_count);
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_START, .clip = true) {
                        if (!any_running && failed_count > 0) {
                            cui_box(ui, .pad_top = 6) {
                                cui_icon(ui, CUI_ICON_CIRCLE_ALERT, 13, CUI_COLOR_DANGER);
                            }
                        }
                        cui_reasoning(ui, grp_id, .thinking = any_running, .title = grp_title) {
                            for (int ti = bi; ti < end; ++ti) {
                                char row_id[48];
                                snprintf(row_id, sizeof row_id, "blk-%d-%d", turn_idx, ti);
                                draw_tool_row(ui, &turn->blocks[ti], thread->workdir, row_id);
                            }
                        }
                    }
                }
                bi = end - 1;
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
