#include "mcode_ui_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
        strstr(name, "write") || strstr(name, "file_change") || strstr(name, "patch") ||
        strstr(name, "replace"))
        return TOOL_KIND_EDIT;
    if (strstr(name, "Web") || strstr(name, "web") || strstr(name, "fetch") || strstr(name, "url"))
        return TOOL_KIND_WEB;
    if (strstr(name, "Read") || strstr(name, "read") || strstr(name, "view"))
        return TOOL_KIND_READ;
    if (strstr(name, "Search") || strstr(name, "Grep") || strstr(name, "Glob") ||
        strstr(name, "search") || strstr(name, "grep") || strstr(name, "glob") ||
        strstr(name, "find_") || strstr(name, "list_dir"))
        return TOOL_KIND_SEARCH;
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

static bool link_starts_with_icase(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        ++s;
        ++prefix;
    }
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool has_non_file_uri_scheme(const char *s) {
    if (!s || !isalpha((unsigned char)s[0])) return false;
    if (link_starts_with_icase(s, "mailto:") || link_starts_with_icase(s, "tel:")) return true;
    const char *p = s + 1;
    while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) ++p;
    return (p[0] == ':' && p[1] == '/' && p[2] == '/');
}

static bool path_exists_on_disk(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_line_range_token(const char *s) {
    if (!s || !isdigit((unsigned char)s[0])) return false;
    bool saw_dash = false;
    for (const char *p = s; *p; ++p) {
        if (isdigit((unsigned char)*p)) continue;
        if (*p == '-' && !saw_dash && isdigit((unsigned char)p[1])) {
            saw_dash = true;
            continue;
        }
        return false;
    }
    return true;
}

static void strip_line_col_suffix(char *path) {
    if (!path || !path[0]) return;
    for (int pass = 0; pass < 2; ++pass) {
        const char *slash = strrchr(path, '/');
        const char *bslash = strrchr(path, '\\');
        const char *sep = (bslash && (!slash || bslash > slash)) ? bslash : slash;
        char *colon = strrchr(path, ':');
        if (!colon) break;
        if (sep && colon < sep) break;
        /* Preserve Windows drive letter prefix like "C:" */
        if (colon == path + 1 && isalpha((unsigned char)path[0])) break;
        if (!is_line_range_token(colon + 1)) break;
        *colon = '\0';
    }
}

static void open_markdown_link(mcode_app *app, cui_ctx *ui, const mcode_thread *thread,
                               const char *raw_link) {
    if (!raw_link || !raw_link[0]) return;
    while (*raw_link == ' ' || *raw_link == '\t') ++raw_link;
    if (!raw_link[0]) return;

    bool is_file_uri = link_starts_with_icase(raw_link, "file://");
    if (!is_file_uri && has_non_file_uri_scheme(raw_link)) {
        cui_app_open_url(raw_link);
        return;
    }

    char path[MCODE_PATH_MAX] = "";
    if (is_file_uri) {
        const char *p = raw_link + 7;
        if (link_starts_with_icase(p, "localhost/")) p += 9;
#ifdef _WIN32
        if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') ++p;
#endif
        size_t w = 0;
        while (*p && w + 1 < sizeof path) {
            if (*p == '#' || *p == '?') break;
            if (*p == '%' && p[1] && p[2]) {
                int hi = hex_nibble(p[1]);
                int lo = hex_nibble(p[2]);
                if (hi >= 0 && lo >= 0) {
                    path[w++] = (char)((hi << 4) | lo);
                    p += 3;
                    continue;
                }
            }
            path[w++] = *p++;
        }
        path[w] = '\0';
    } else {
        snprintf(path, sizeof path, "%s", raw_link);
        size_t len = strlen(path);
        while (len > 0 && isspace((unsigned char)path[len - 1])) path[--len] = '\0';
    }

    if (!path[0]) return;

    /* Strip #L... fragment and :line[:col] suffixes when the literal path does not exist. */
    if (!path_exists_on_disk(path)) {
        char *hash = strchr(path, '#');
        if (hash) *hash = '\0';
        if (!path_exists_on_disk(path)) {
            strip_line_col_suffix(path);
        }
    }

    /* Expand leading ~/ */
    char resolved[MCODE_PATH_MAX] = "";
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(resolved, sizeof resolved, "%s%s", home, path + 1);
        } else {
            snprintf(resolved, sizeof resolved, "%s", path);
        }
    } else {
        bool is_abs = (path[0] == '/' || path[0] == '\\' ||
                       (isalpha((unsigned char)path[0]) && path[1] == ':' &&
                        (path[2] == '/' || path[2] == '\\')));
        if (!is_abs && thread && thread->workdir[0]) {
            size_t wlen = strlen(thread->workdir);
            while (wlen > 1 && (thread->workdir[wlen - 1] == '/' ||
                                thread->workdir[wlen - 1] == '\\')) {
                --wlen;
            }
            snprintf(resolved, sizeof resolved, "%.*s/%s", (int)wlen, thread->workdir, path);
        } else {
            snprintf(resolved, sizeof resolved, "%s", path);
        }
    }

    if (path_exists_on_disk(resolved)) {
        cui_app_open_url(resolved);
    } else if (app && ui) {
        char msg[256];
        snprintf(msg, sizeof msg, "File not found: %s", resolved);
        app_chrome_notify(app->chrome, ui, msg, CUI_TONE_WARNING);
    }
}

static bool looks_like_json(const char *s) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    return *s == '{' || *s == '[';
}

static void format_work_duration(float seconds, bool live, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (seconds < 0.0f) seconds = 0.0f;
    int total = (int)seconds;
    if (!live && total == 0) {
        snprintf(out, cap, "<1s");
        return;
    }
    if (total < 60) {
        snprintf(out, cap, "%ds", total);
    } else if (total < 3600) {
        int m = total / 60;
        int s = total % 60;
        if (!live && s == 0) {
            snprintf(out, cap, "%dm", m);
        } else {
            snprintf(out, cap, "%dm %ds", m, s);
        }
    } else {
        int h = total / 3600;
        int m = (total % 3600) / 60;
        if (!live && m == 0) {
            snprintf(out, cap, "%dh", h);
        } else {
            snprintf(out, cap, "%dh %dm", h, m);
        }
    }
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
            char dur[32];
            format_work_duration(secs, running, dur, sizeof dur);
            snprintf(out + n, cap - (size_t)n, " · %s", dur);
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
        const mcode_block *rb = &blocks[running_idx];
        float rsecs = (rb->started_mono > 0.0) ? (float)(mcode_now_mono() - rb->started_mono) : rb->duration_sec;
        format_tool_row_title(rb, workdir, rsecs, true, out, cap);
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
        char dur[32];
        format_work_duration(total_secs, false, dur, sizeof dur);
        snprintf(out + pos, cap - (size_t)pos, " · %s", dur);
    }
}

static void draw_tool_payload(cui_ctx *ui, const char *id, const char *label,
                              const char *body, const char *lang_hint, bool is_error) {
    if (!body || !body[0]) return;
    cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 11.5f,
             .color = is_error ? CUI_COLOR_DANGER : CUI_COLOR_TEXT_MUTED);
    if (is_error && !looks_like_json(body)) {
        cui_column(ui, .id = id, .width = CUI_FILL, .max_height = 180, .scroll_y = true,
                   .pad_x = 12, .pad_y = 10, .radius = 8, .border = 1,
                   .border_color = cui_alpha(cui_color_of(ui, CUI_COLOR_DANGER), 0.35f),
                   .bg = cui_alpha(cui_color_of(ui, CUI_COLOR_DANGER), 0.06f)) {
            cui_text(ui, body, .font = CUI_FONT_MONO, .size = 12,
                     .color = CUI_COLOR_FOREGROUND, .selectable = true, .width = CUI_FILL);
        }
    } else {
        const char *lang = looks_like_json(body) ? "json" : (lang_hint ? lang_hint : "text");
        cui_code_block(ui, id, body, .lang = lang, .max_height = 180, .size = 12, .no_header = true);
    }
}

static void draw_tool_row(cui_ctx *ui, const mcode_block *b, const char *workdir,
                          const char *bid) {
    float secs = (b->status == CUI_TASK_RUNNING && b->started_mono > 0.0)
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

static void draw_empty_workspace(mcode_app *app, cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
               .pad = 32, .gap = 18) {
        cui_card(ui, .width = CUI_FILL, .max_width = 560, .pad = 24, .gap = 14, .radius = 14) {
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

            if (app->store.project_count == 0) {
                cui_row(ui, .gap = 10, .wrap = true) {
                    if (cui_button(ui, "Open Folder", .variant = CUI_BUTTON_PRIMARY,
                                   .icon = CUI_ICON_FOLDER_OPEN)) {
                        mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
                    }
                    if (cui_button(ui, "New Project", .variant = CUI_BUTTON_SECONDARY,
                                   .icon = CUI_ICON_PLUS)) {
                        mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CREATE);
                    }
                    if (cui_button(ui, "Clone URL", .variant = CUI_BUTTON_OUTLINE,
                                   .icon = CUI_ICON_DOWNLOAD)) {
                        mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_CLONE);
                    }
                    if (cui_button(ui, "Settings", .variant = CUI_BUTTON_GHOST,
                                   .icon = CUI_ICON_SETTINGS)) {
                        app->chrome->settings_open = true;
                        app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
                    }
                }

                /* One-click quick-start for detected local Git repositories */
                int shown = 0;
                int avail = 0;
                for (int i = 0; i < app->discovered_repo_count; ++i) {
                    if (!mcode_store_find_project_by_path(&app->store, app->discovered_repos[i].path)) {
                        ++avail;
                    }
                }
                if (avail > 0) {
                    cui_separator(ui);
                    cui_text(ui, "Quick-open detected repositories", .font = CUI_FONT_MEDIUM,
                             .size = 12.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_column(ui, .width = CUI_FILL, .gap = 5) {
                        for (int i = 0; i < app->discovered_repo_count && shown < 4; ++i) {
                            const mcode_discovered_repo *r = &app->discovered_repos[i];
                            if (mcode_store_find_project_by_path(&app->store, r->path)) continue;
                            ++shown;
                            cui_push_id_int(ui, i);
                            bool clicked = cui_card_begin(ui, &(cui_card_opts){
                                .id = "welcome-repo",
                                .dir = CUI_ROW,
                                .align = CUI_ALIGN_CENTER,
                                .width = CUI_FILL,
                                .pad_x = 10,
                                .pad_y = 7,
                                .gap = 8,
                                .hoverable = true,
                                .muted = true,
                                .radius = 8,
                            });
                            cui_icon(ui, CUI_ICON_FOLDER, 14, CUI_COLOR_ACCENT);
                            cui_text(ui, r->name, .font = CUI_FONT_MEDIUM, .size = 13,
                                     .nowrap = true);
                            char short_p[MCODE_PATH_MAX];
                            mcode_git_shorten_home_path(r->path, short_p, sizeof short_p);
                            cui_text(ui, short_p, .font = CUI_FONT_MONO, .size = 11,
                                     .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL,
                                     .nowrap = true);
                            cui_icon(ui, CUI_ICON_ARROW_RIGHT, 13, CUI_COLOR_TEXT_MUTED);
                            cui_card_end(ui);
                            cui_pop_id(ui);
                            if (clicked) {
                                mcode_ui_add_project_and_open(app, ui, r->path, r->name);
                                break;
                            }
                        }
                    }
                    if (avail > 4) {
                        char more_lbl[48];
                        snprintf(more_lbl, sizeof more_lbl, "Browse all %d detected repositories...", avail);
                        if (cui_button(ui, more_lbl, .variant = CUI_BUTTON_GHOST,
                                       .size = CUI_SIZE_SM)) {
                            mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
                        }
                    }
                }
            } else {
                cui_text(ui, "Start a thread in one of your projects", .font = CUI_FONT_MEDIUM,
                         .size = 12.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_column(ui, .width = CUI_FILL, .gap = 5) {
                    int max_show = app->store.project_count < 5 ? app->store.project_count : 5;
                    for (int i = 0; i < max_show; ++i) {
                        mcode_project *p = &app->store.projects[i];
                        cui_push_id(ui, p->id);
                        bool clicked = cui_card_begin(ui, &(cui_card_opts){
                            .id = "welcome-proj",
                            .dir = CUI_ROW,
                            .align = CUI_ALIGN_CENTER,
                            .width = CUI_FILL,
                            .pad_x = 10,
                            .pad_y = 7,
                            .gap = 8,
                            .hoverable = true,
                            .muted = true,
                            .radius = 8,
                        });
                        cui_icon(ui, CUI_ICON_FOLDER_OPEN, 14, CUI_COLOR_ACCENT);
                        cui_text(ui, p->name, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                        char short_p[MCODE_PATH_MAX];
                        mcode_git_shorten_home_path(p->path, short_p, sizeof short_p);
                        cui_text(ui, short_p, .font = CUI_FONT_MONO, .size = 11,
                                 .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL, .nowrap = true);
                        cui_icon(ui, CUI_ICON_PLUS, 14, CUI_COLOR_ACCENT);
                        cui_card_end(ui);
                        cui_pop_id(ui);
                        if (clicked) {
                            mcode_ui_quick_new_thread(app, ui, p);
                            break;
                        }
                    }
                }

                cui_row(ui, .gap = 10, .wrap = true) {
                    if (cui_button(ui, "Add or Create Project", .variant = CUI_BUTTON_SECONDARY,
                                   .icon = CUI_ICON_PLUS)) {
                        mcode_ui_open_project_dialog(app, MCODE_PROJ_DIALOG_OPEN);
                    }
                    if (cui_button(ui, "Settings", .variant = CUI_BUTTON_GHOST,
                                   .icon = CUI_ICON_SETTINGS)) {
                        app->chrome->settings_open = true;
                        app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
                    }
                }
            }
        }
    }
}

static void draw_user_message(cui_ctx *ui, const char *thread_id, const char *text) {
    if (!text) text = "";
    cui_push_id(ui, (thread_id && thread_id[0]) ? thread_id : "thread");
    cui_push_id(ui, "user-msg");

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_accent accent = cui_get_accent(ui);
    cui_color surface = cui_color_of(ui, CUI_COLOR_SURFACE);
    cui_color bg;
    cui_color border_col;
    if (accent == CUI_ACCENT_NEUTRAL) {
        cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
        bg = cui_mix(surface, fg, dark ? 0.08f : 0.055f);
        border_col = cui_color_of(ui, dark ? CUI_COLOR_BORDER : CUI_COLOR_BORDER_SUBTLE);
    } else {
        cui_color acc = cui_color_of(ui, CUI_COLOR_ACCENT);
        bg = cui_mix(surface, acc, dark ? 0.14f : 0.09f);
        border_col = cui_alpha(acc, dark ? 0.30f : 0.22f);
    }

    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END, .pad_top = 10) {
        cui_column(ui, .max_width = 480, .pad_x = 13, .pad_y = 8,
                   .bg = bg, .border = 1.0f, .border_color = border_col,
                   .radius_tl = 18, .radius_tr = 18, .radius_br = 6, .radius_bl = 18) {
            cui_text(ui, text, .color = CUI_COLOR_FOREGROUND,
                     .line_height = 1.45f, .selectable = true);
        }
    }

    cui_pop_id(ui);
    cui_pop_id(ui);
}

static void draw_turn(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int turn_idx) {
    mcode_turn *turn = &thread->turns[turn_idx];
    bool is_last_turn = (turn_idx == thread->turn_count - 1);
    cui_push_id_int(ui, turn_idx);

    cui_column(ui, .width = CUI_FILL, .gap = 10) {
        /* User prompt bubble */
        draw_user_message(ui, thread->id, mcode_str_cstr(&turn->prompt));

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
                        open_markdown_link(app, ui, thread, mr.link);
                    }
                }
            } else if (b->kind == MCODE_BLOCK_ERROR) {
                cui_alert(ui, "Agent error", .description = mcode_str_cstr(&b->text),
                          .tone = CUI_TONE_DANGER);
            }
        }

        if (is_last_turn && turn->running && thread->pending_approval.active) {
            const mcode_pending_approval *pa = &thread->pending_approval;
            bool can_allow_session = pa->can_allow_session;
            cui_column(ui, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 10,
                       .border = 1,
                       .border_color = cui_alpha(cui_color_of(ui, CUI_COLOR_WARNING), 0.55f),
                       .bg = cui_alpha(cui_color_of(ui, CUI_COLOR_WARNING), 0.08f)) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_SHIELD, 16, CUI_COLOR_WARNING);
                    cui_text(ui, pa->title[0] ? pa->title : "Approval required",
                             .font = CUI_FONT_MEDIUM, .size = 13.5f);
                    if (pa->tool_name[0]) {
                        cui_badge(ui, pa->tool_name, .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
                    }
                }
                if (pa->reason[0]) {
                    cui_text(ui, pa->reason, .size = 12.5f,
                             .color = CUI_COLOR_TEXT_SECONDARY, .width = CUI_FILL);
                }
                const char *preview = pa->command[0] ? pa->command
                                    : (pa->details[0] ? pa->details : NULL);
                if (preview) {
                    cui_column(ui, .id = "approval-preview", .width = CUI_FILL,
                               .max_height = 140, .scroll_y = true,
                               .pad_x = 10, .pad_y = 8, .radius = 7, .border = 1,
                               .border_color = CUI_COLOR_BORDER,
                               .bg = CUI_COLOR_SURFACE) {
                        cui_text(ui, preview, .font = CUI_FONT_MONO, .size = 12,
                                 .color = CUI_COLOR_FOREGROUND, .selectable = true,
                                 .width = CUI_FILL);
                    }
                }
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER, .wrap = true) {
                    if (cui_button(ui, "Approve", .variant = CUI_BUTTON_PRIMARY,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_CHECK)) {
                        mcode_agent_respond_approval(thread->active_run, thread,
                                                     MCODE_APPROVAL_ALLOW_ONCE);
                    }
                    if (can_allow_session) {
                        if (cui_button(ui, "Allow for session", .variant = CUI_BUTTON_SECONDARY,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_CHECK)) {
                            mcode_agent_respond_approval(thread->active_run, thread,
                                                         MCODE_APPROVAL_ALLOW_SESSION);
                        }
                    }
                    if (cui_button(ui, "Deny", .variant = CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_X)) {
                        mcode_agent_respond_approval(thread->active_run, thread,
                                                     MCODE_APPROVAL_DENY);
                    }
                }
            }
        }

        if (turn->running) {
            float live_secs = (turn->started_mono > 0.0)
                ? (float)(mcode_now_mono() - turn->started_mono)
                : turn->duration_sec;
            char elapsed[32];
            format_work_duration(live_secs, true, elapsed, sizeof elapsed);
            char status_lbl[192];
            snprintf(status_lbl, sizeof status_lbl, "%s · %s",
                     thread->status_text[0] ? thread->status_text : "Working", elapsed);
            cui_row(ui, .width = CUI_FILL, .clip = true) {
                cui_agent_status(ui, status_lbl, .status = CUI_TASK_RUNNING);
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
                char meta[160] = "";
                int pos = 0;
                if (turn->duration_sec > 0.05f) {
                    char dur[32];
                    format_work_duration(turn->duration_sec, false, dur, sizeof dur);
                    pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "Worked for %s", dur);
                }
                if (turn->input_tokens > 0 || turn->output_tokens > 0) {
                    pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%d in / %d out",
                                    pos > 0 ? " · " : "", turn->input_tokens, turn->output_tokens);
                    if (turn->cost_usd > 0.0 && (size_t)pos < sizeof meta) {
                        snprintf(meta + pos, sizeof meta - (size_t)pos, " · $%.4f", turn->cost_usd);
                    }
                }
                if (meta[0]) {
                    cui_text(ui, meta, .size = 12, .color = CUI_COLOR_TEXT_MUTED, .tabular = true);
                }
            }
        }
    }

    cui_pop_id(ui);
}

static bool model_contains_icase(const char *hay, const char *needle) {
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

static const char *reasoning_row_label(const char *slug) {
    if (!slug || !slug[0]) return "";
    if (strcmp(slug, "xhigh") == 0 || strcmp(slug, "XHigh") == 0) return "Extra High";
    return mcode_effort_display_label(slug);
}

static int reasoning_effort_rank(const char *tag) {
    if (!tag || !tag[0]) return 100;
    if (strcmp(tag, "minimal") == 0 || strcmp(tag, "Minimal") == 0) return 0;
    if (strcmp(tag, "low") == 0 || strcmp(tag, "Low") == 0) return 1;
    if (strcmp(tag, "medium") == 0 || strcmp(tag, "Medium") == 0) return 2;
    if (strcmp(tag, "high") == 0 || strcmp(tag, "High") == 0) return 3;
    if (strcmp(tag, "xhigh") == 0 || strcmp(tag, "XHigh") == 0 || strcmp(tag, "Extra High") == 0) return 4;
    if (strcmp(tag, "max") == 0 || strcmp(tag, "Max") == 0) return 5;
    if (strcmp(tag, "ultra") == 0 || strcmp(tag, "Ultra") == 0) return 6;
    return 50;
}

static void split_paren_label(const char *label, char *base_out, size_t base_cap,
                              char *tag_out, size_t tag_cap) {
    if (base_out && base_cap > 0) base_out[0] = '\0';
    if (tag_out && tag_cap > 0) tag_out[0] = '\0';
    if (!label || !label[0]) return;
    size_t len = strlen(label);
    if (len >= 4 && label[len - 1] == ')') {
        const char *lp = strrchr(label, '(');
        if (lp && lp > label && lp[-1] == ' ') {
            size_t tag_len = (size_t)((label + len - 1) - (lp + 1));
            size_t base_len = (size_t)((lp - 1) - label);
            if (tag_len > 0 && base_len > 0) {
                if (base_out && base_cap > 0) {
                    if (base_len >= base_cap) base_len = base_cap - 1;
                    memcpy(base_out, label, base_len);
                    base_out[base_len] = '\0';
                }
                if (tag_out && tag_cap > 0) {
                    if (tag_len >= tag_cap) tag_len = tag_cap - 1;
                    memcpy(tag_out, lp + 1, tag_len);
                    tag_out[tag_len] = '\0';
                }
                return;
            }
        }
    }
    if (base_out && base_cap > 0) snprintf(base_out, base_cap, "%s", label);
}

static void format_short_model_tag(const char *model_name, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!model_name || !model_name[0]) return;
    const char *p = model_name;
    static const char *const k_prefixes[] = { "GPT-", "GPT ", "Claude ", "Gemini " };
    for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; ++i) {
        size_t plen = strlen(k_prefixes[i]);
        if (strncmp(p, k_prefixes[i], plen) == 0 && p[plen] != '\0') {
            p += plen;
            break;
        }
    }
    snprintf(out, cap, "%s", p);
}

static bool draw_picker_list_row(cui_ctx *ui, const char *id, float height, bool *out_hovered) {
    cui_id cid = cui_make_id(ui, id);
    cui_rect r = cui_get_rect(ui, cid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = r.w > 0.0f && mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    if (out_hovered) *out_hovered = hov;
    cui_color bg = hov
        ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
        : CUI_RGBA(0x00000001u);
    return cui_card_begin(ui, &(cui_card_opts){
        .id = id,
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_FILL,
        .height = CUI_PX(height),
        .pad_x = 8,
        .pad_y = 1,
        .gap = 9,
        .hoverable = true,
        .border = -1.0f,
        .radius = 7,
        .bg = bg,
    });
}

static void draw_thread_model_selector(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                       bool busy, cui_placement popover_placement) {
    const app_settings *settings = app_chrome_settings(app->chrome);
    const char *eff_model = mcode_agent_effective_model(thread, settings);
    const char *cli_def_model = (thread->agent >= 0 && thread->agent < APP_AGENT_COUNT)
        ? app->models.cli_default_model[thread->agent] : "";
    const char *cli_def_effort = (thread->agent >= 0 && thread->agent < APP_AGENT_COUNT)
        ? app->models.cli_default_effort[thread->agent] : "";

    char model_name[128] = "";
    char effort_label[32] = "";
    mcode_model_format_display(&app->models, thread, settings,
                               model_name, sizeof model_name,
                               effort_label, sizeof effort_label);

    char tooltip[224];
    if (effort_label[0]) {
        snprintf(tooltip, sizeof tooltip, "%s · %s · %s (click to change)",
                 mcode_agent_label(thread->agent), model_name, effort_label);
    } else if (eff_model[0] || cli_def_model[0]) {
        snprintf(tooltip, sizeof tooltip, "%s · %s (click to change)",
                 mcode_agent_label(thread->agent), model_name);
    } else {
        snprintf(tooltip, sizeof tooltip, "%s · Select model", mcode_agent_label(thread->agent));
    }

    cui_id btn_id = cui_make_id(ui, "model-btn");
    cui_rect prev_r = cui_get_rect(ui, btn_id);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hovered = !busy && prev_r.w > 0.0f &&
                   mp.x >= prev_r.x && mp.x <= prev_r.x + prev_r.w &&
                   mp.y >= prev_r.y && mp.y <= prev_r.y + prev_r.h;
    bool open = cui_popover_is_open(ui, "model-picker-pop");
    cui_color btn_bg = (open || hovered)
        ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
        : CUI_RGBA(0x00000001u);
    float lbl_size = cui_theme_get(ui)->text_sm;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "model-btn",
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .height = CUI_PX(30),
        .pad_x = 9,
        .pad_y = 1,
        .gap = 6,
        .hoverable = !busy,
        .border = -1.0f,
        .radius = 9,
        .bg = btn_bg,
    });
    mcode_ui_draw_provider_icon(app, ui, thread->agent, 15.0f,
                                busy ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_TEXT_SECONDARY);
    cui_text(ui, model_name, .font = CUI_FONT_MEDIUM, .size = lbl_size,
             .color = busy ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_FOREGROUND, .nowrap = true);
    if (effort_label[0]) {
        cui_text(ui, effort_label, .font = CUI_FONT_BODY, .size = lbl_size,
                 .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
    }
    cui_icon(ui, open ? CUI_ICON_CHEVRON_UP : CUI_ICON_CHEVRON_DOWN, 13, CUI_COLOR_TEXT_MUTED);
    cui_card_end(ui);
    cui_tooltip(ui, tooltip);

    if (clicked && !busy) {
        app->model_search[0] = '\0';
        cui_popover_toggle(ui, "model-picker-pop");
    }

    const char *fast_desc = "";
    bool supports_fast = mcode_model_supports_fast(&app->models, thread, settings, &fast_desc);
    bool is_fast = supports_fast && mcode_thread_is_fast_mode(&app->models, thread);

    cui_popover(ui, "model-picker-pop", .placement = popover_placement,
                .width = 528, .pad = 1, .gap = 0) {
        cui_column(ui, .width = CUI_FILL, .gap = 0) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(336.0f), .gap = 0,
                .align = CUI_ALIGN_STRETCH) {
            /* Column 1: Vertical provider icon rail */
            cui_column(ui, .width = CUI_PX(48), .height = CUI_FILL,
                       .pad_y = 10, .gap = 6, .align = CUI_ALIGN_CENTER) {
                for (int pi = 0; pi < APP_AGENT_COUNT; ++pi) {
                    bool psel = ((int)thread->agent == pi);
                    cui_push_id_int(ui, pi);
                    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(36),
                            .align = CUI_ALIGN_CENTER, .gap = 0) {
                        cui_id pid = cui_make_id(ui, "prail");
                        cui_rect pr = cui_get_rect(ui, pid);
                        bool phov = pr.w > 0.0f && mp.x >= pr.x && mp.x <= pr.x + pr.w &&
                                    mp.y >= pr.y && mp.y <= pr.y + pr.h;
                        cui_box(ui, .width = CUI_PX(5)) {}
                        bool pclick = cui_card_begin(ui, &(cui_card_opts){
                            .id = "prail",
                            .dir = CUI_ROW,
                            .align = CUI_ALIGN_CENTER,
                            .width = CUI_FILL,
                            .height = CUI_PX(34),
                            .pad = 1,
                            .gap = 0,
                            .hoverable = true,
                            .border = -1.0f,
                            .radius = 8,
                            .bg = phov ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
                                       : CUI_RGBA(0x00000001u),
                        });
                        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER,
                                .align = CUI_ALIGN_CENTER) {
                            mcode_ui_draw_provider_icon(app, ui, (app_agent_kind)pi, 18.0f,
                                                        psel ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
                        }
                        cui_card_end(ui);
                        cui_tooltip(ui, mcode_agent_label((app_agent_kind)pi));
                        cui_box(ui, .width = CUI_PX(2.5f), .height = CUI_PX(20), .radius = 1.5f,
                                .bg = psel ? CUI_COLOR_ACCENT : CUI_TRANSPARENT) {}
                        if (pclick && !busy && pi != (int)thread->agent) {
                            thread->agent = (app_agent_kind)pi;
                            thread->session_id[0] = '\0';
                            thread->model[0] = '\0';
                            thread->reasoning_effort[0] = '\0';
                            thread->speed_mode = MCODE_SPEED_DEFAULT;
                            app->model_search[0] = '\0';
                            mcode_ui_mark_dirty(app);
                            eff_model = mcode_agent_effective_model(thread, settings);
                            cli_def_model = app->models.cli_default_model[thread->agent];
                            cli_def_effort = app->models.cli_default_effort[thread->agent];
                            mcode_model_format_display(&app->models, thread, settings,
                                                       model_name, sizeof model_name,
                                                       effort_label, sizeof effort_label);
                            supports_fast = mcode_model_supports_fast(&app->models, thread, settings, &fast_desc);
                            is_fast = supports_fast && mcode_thread_is_fast_mode(&app->models, thread);
                        }
                    }
                    cui_pop_id(ui);
                }

                cui_spacer(ui);
                if (thread->agent == APP_AGENT_AGY && app->models.loading_agy) {
                    cui_spinner(ui, 13);
                } else if (thread->agent == APP_AGENT_CODEX || thread->agent == APP_AGENT_AGY) {
                    if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                        .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Refresh available models")) {
                        mcode_agent_detect_clis(settings, &app->cli_status);
                        mcode_model_catalog_refresh(&app->models, &app->cli_status);
                    }
                }
            }

            cui_separator(ui);

            /* Resolve active model & grouped entries for the selected provider */
            const char *active_id = eff_model[0] ? eff_model : cli_def_model;
            int cat_count = (thread->agent >= 0 && thread->agent < APP_AGENT_COUNT)
                ? app->models.count[thread->agent] : 0;

            typedef struct {
                char label[96];
                char primary_id[APP_MODEL_NAME_MAX];
                int variant_indices[MCODE_MAX_EFFORTS_PER_MODEL];
                char variant_tags[MCODE_MAX_EFFORTS_PER_MODEL][32];
                int variant_count;
                bool active;
            } picker_model_entry;

            picker_model_entry entries[MCODE_MAX_MODELS_PER_AGENT + 1];
            int entry_count = 0;
            int active_entry_idx = -1;
            char active_variant_tag[32] = "";

            if (thread->agent == APP_AGENT_AGY) {
                for (int i = 0; i < cat_count; ++i) {
                    const mcode_model_option *m = &app->models.items[APP_AGENT_AGY][i];
                    char base[96], tag[32];
                    split_paren_label(m->label, base, sizeof base, tag, sizeof tag);
                    if (!base[0]) snprintf(base, sizeof base, "%s", m->id);

                    int found = -1;
                    for (int g = 0; g < entry_count; ++g) {
                        if (strcmp(entries[g].label, base) == 0) { found = g; break; }
                    }
                    if (found < 0) {
                        if (entry_count >= MCODE_MAX_MODELS_PER_AGENT) continue;
                        found = entry_count++;
                        memset(&entries[found], 0, sizeof entries[found]);
                        snprintf(entries[found].label, sizeof entries[found].label, "%s", base);
                        snprintf(entries[found].primary_id, sizeof entries[found].primary_id, "%s", m->id);
                    }
                    picker_model_entry *e = &entries[found];
                    if (e->variant_count < MCODE_MAX_EFFORTS_PER_MODEL) {
                        int vi = e->variant_count++;
                        e->variant_indices[vi] = i;
                        snprintf(e->variant_tags[vi], sizeof e->variant_tags[vi], "%s",
                                 tag[0] ? tag : "Default");
                    }
                    if (active_id[0] && strcmp(active_id, m->id) == 0) {
                        e->active = true;
                        active_entry_idx = found;
                        snprintf(active_variant_tag, sizeof active_variant_tag, "%s", tag);
                    }
                }
                /* Sort each model's effort variants in canonical ascending order (Low -> Extra High). */
                for (int g = 0; g < entry_count; ++g) {
                    picker_model_entry *e = &entries[g];
                    for (int a = 0; a < e->variant_count; ++a) {
                        for (int b = a + 1; b < e->variant_count; ++b) {
                            if (reasoning_effort_rank(e->variant_tags[b]) <
                                reasoning_effort_rank(e->variant_tags[a])) {
                                int tmp_idx = e->variant_indices[a];
                                e->variant_indices[a] = e->variant_indices[b];
                                e->variant_indices[b] = tmp_idx;
                                char tmp_tag[32];
                                memcpy(tmp_tag, e->variant_tags[a], sizeof tmp_tag);
                                memcpy(e->variant_tags[a], e->variant_tags[b], sizeof tmp_tag);
                                memcpy(e->variant_tags[b], tmp_tag, sizeof tmp_tag);
                            }
                        }
                    }
                }
            } else {
                for (int i = 0; i < cat_count && entry_count < MCODE_MAX_MODELS_PER_AGENT; ++i) {
                    const mcode_model_option *m = &app->models.items[thread->agent][i];
                    picker_model_entry *e = &entries[entry_count];
                    memset(e, 0, sizeof *e);
                    snprintf(e->label, sizeof e->label, "%s", m->label[0] ? m->label : m->id);
                    snprintf(e->primary_id, sizeof e->primary_id, "%s", m->id);
                    e->active = (active_id[0] && strcmp(active_id, m->id) == 0);
                    if (e->active) active_entry_idx = entry_count;
                    ++entry_count;
                }
            }

            /* Include custom model override if not in catalog */
            if (thread->model[0] && active_entry_idx < 0 && entry_count <= MCODE_MAX_MODELS_PER_AGENT) {
                picker_model_entry *e = &entries[entry_count];
                memset(e, 0, sizeof *e);
                snprintf(e->label, sizeof e->label, "%s", thread->model);
                snprintf(e->primary_id, sizeof e->primary_id, "%s", thread->model);
                e->active = true;
                active_entry_idx = entry_count++;
            }

            /* Column 2: Search + clean model list */
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .pad_x = 12, .pad_y = 10, .gap = 8) {
                int sf = cui_search(ui, "model-filter", app->model_search, sizeof app->model_search,
                                    .placeholder = "Search models...", .size = CUI_SIZE_SM,
                                    .width = CUI_FILL, .bg = CUI_RGBA(0x00000001u),
                                    .border_color = CUI_RGBA(0x00000001u));
                cui_separator(ui);

                bool filtering = (app->model_search[0] != '\0');
                int match_indices[MCODE_MAX_MODELS_PER_AGENT + 1];
                int match_count = 0;
                bool exact_id_match = false;
                for (int i = 0; i < entry_count; ++i) {
                    if (filtering && strcmp(entries[i].primary_id, app->model_search) == 0) {
                        exact_id_match = true;
                    }
                    bool ok = !filtering ||
                              model_contains_icase(entries[i].label, app->model_search) ||
                              model_contains_icase(entries[i].primary_id, app->model_search);
                    if (ok) match_indices[match_count++] = i;
                }

                cui_scroll(ui, .id = "model-picker-list", .width = CUI_FILL, .height = CUI_FILL,
                           .gap = 2) {
                    for (int mi = 0; mi < match_count; ++mi) {
                        int idx = match_indices[mi];
                        const picker_model_entry *e = &entries[idx];
                        cui_push_id_int(ui, idx);
                        bool row_clicked = draw_picker_list_row(ui, "mrow", 34.0f, NULL);
                        mcode_ui_draw_provider_icon(app, ui, thread->agent, 15.0f,
                                                    CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, e->label,
                                 .font = e->active ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                                 .size = 13.5f,
                                 .color = CUI_COLOR_FOREGROUND,
                                 .nowrap = true);
                        if (e->active) {
                            cui_icon(ui, CUI_ICON_CHECK, 14, CUI_COLOR_TEXT_SECONDARY);
                        }
                        cui_card_end(ui);
                        cui_pop_id(ui);

                        if (row_clicked) {
                            const char *chosen_id = e->primary_id;
                            if (thread->agent == APP_AGENT_AGY && e->variant_count > 0) {
                                /* Preserve the currently selected tier if this model offers it. */
                                const char *want_tag = active_variant_tag[0] ? active_variant_tag : "High";
                                for (int vi = 0; vi < e->variant_count; ++vi) {
                                    if (strcmp(e->variant_tags[vi], want_tag) == 0) {
                                        chosen_id = app->models.items[APP_AGENT_AGY][e->variant_indices[vi]].id;
                                        break;
                                    }
                                }
                            }
                            snprintf(thread->model, sizeof thread->model, "%s", chosen_id);
                            mcode_ui_mark_dirty(app);
                        }
                    }

                    /* Allow using an unlisted custom model ID directly from the search box */
                    if (filtering && !exact_id_match) {
                        const char *q = app->model_search;
                        while (*q && isspace((unsigned char)*q)) ++q;
                        size_t qlen = strlen(q);
                        while (qlen > 0 && isspace((unsigned char)q[qlen - 1])) --qlen;
                        if (qlen > 0) {
                            char custom_lbl[128];
                            snprintf(custom_lbl, sizeof custom_lbl, "Use \"%.*s\"", (int)qlen, q);
                            bool use_custom = (sf & CUI_SUBMITTED) != 0;
                            if (draw_picker_list_row(ui, "use-custom", 32.0f, NULL)) {
                                use_custom = true;
                            }
                            cui_icon(ui, CUI_ICON_PLUS, 14, CUI_COLOR_TEXT_MUTED);
                            cui_text(ui, custom_lbl, .size = 13, .color = CUI_COLOR_TEXT_SECONDARY,
                                     .nowrap = true);
                            cui_card_end(ui);
                            if (use_custom) {
                                if (qlen >= sizeof thread->model) qlen = sizeof thread->model - 1;
                                memcpy(thread->model, q, qlen);
                                thread->model[qlen] = '\0';
                                mcode_ui_mark_dirty(app);
                                cui_popover_close(ui, "model-picker-pop");
                            }
                        }
                    } else if (match_count == 0) {
                        cui_box(ui, .pad = 8, .width = CUI_FILL) {
                            cui_muted(ui, "No matching models.");
                        }
                    }
                }
            }

            cui_separator(ui);

            /* Column 3: Reasoning effort column */
            cui_column(ui, .width = CUI_PX(188), .height = CUI_FILL,
                       .pad_x = 12, .pad_y = 10, .gap = 8) {
                char short_model[64] = "";
                format_short_model_tag(model_name, short_model, sizeof short_model);
                cui_row(ui, .width = CUI_FILL, .height = CUI_PX(30),
                        .align = CUI_ALIGN_CENTER, .gap = 6) {
                    cui_text(ui, "Reasoning", .size = 13, .color = CUI_COLOR_TEXT_MUTED,
                             .nowrap = true);
                    cui_spacer(ui);
                    if (short_model[0] && strcmp(short_model, "Default model") != 0) {
                        cui_text(ui, short_model, .size = 12.5f, .color = CUI_COLOR_TEXT_MUTED,
                                 .nowrap = true);
                    }
                }
                cui_separator(ui);

                cui_scroll(ui, .id = "effort-picker-list", .width = CUI_FILL, .height = CUI_FILL,
                           .gap = 2) {
                    if (thread->agent == APP_AGENT_AGY) {
                        int agy_idx = active_entry_idx >= 0 ? active_entry_idx : (entry_count > 0 ? 0 : -1);
                        if (agy_idx >= 0 && entries[agy_idx].variant_count > 0) {
                            const picker_model_entry *ae = &entries[agy_idx];
                            for (int vi = 0; vi < ae->variant_count; ++vi) {
                                const mcode_model_option *vm =
                                    &app->models.items[APP_AGENT_AGY][ae->variant_indices[vi]];
                                bool vsel = (active_id[0] && strcmp(active_id, vm->id) == 0);
                                cui_push_id_int(ui, vi);
                                bool vclick = draw_picker_list_row(ui, "vrow", 34.0f, NULL);
                                cui_text(ui, reasoning_row_label(ae->variant_tags[vi]),
                                         .font = vsel ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                                         .size = 13.5f,
                                         .color = CUI_COLOR_FOREGROUND,
                                         .nowrap = true);
                                cui_spacer(ui);
                                if (vsel) {
                                    cui_icon(ui, CUI_ICON_CHECK, 14, CUI_COLOR_FOREGROUND);
                                }
                                cui_card_end(ui);
                                cui_pop_id(ui);
                                if (vclick) {
                                    snprintf(thread->model, sizeof thread->model, "%s", vm->id);
                                    mcode_ui_mark_dirty(app);
                                }
                            }
                        } else {
                            cui_box(ui, .pad = 6) {
                                cui_muted(ui, "Select a model");
                            }
                        }
                    } else {
                        const mcode_model_option *active_opt =
                            mcode_model_find(&app->models, thread->agent, active_id);
                        static const char *const k_fallback_efforts[] = {
                            "low", "medium", "high", "xhigh", "max"
                        };
                        int n_eff = (active_opt && active_opt->effort_count > 0)
                            ? active_opt->effort_count
                            : (int)(sizeof k_fallback_efforts / sizeof k_fallback_efforts[0]);
                        const char *cur_eff = thread->reasoning_effort[0]
                            ? thread->reasoning_effort
                            : (cli_def_effort[0] ? cli_def_effort
                               : (active_opt ? active_opt->default_effort : ""));

                        for (int ei = 0; ei < n_eff; ++ei) {
                            const char *eslug = (active_opt && active_opt->effort_count > 0)
                                ? active_opt->efforts[ei]
                                : k_fallback_efforts[ei];
                            bool esel = (cur_eff[0] && strcmp(cur_eff, eslug) == 0);
                            cui_push_id_int(ui, ei);
                            bool eclick = draw_picker_list_row(ui, "erow", 34.0f, NULL);
                            cui_text(ui, reasoning_row_label(eslug),
                                     .font = esel ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                                     .size = 13.5f,
                                     .color = CUI_COLOR_FOREGROUND,
                                     .nowrap = true);
                            cui_spacer(ui);
                            if (esel) {
                                cui_icon(ui, CUI_ICON_CHECK, 14, CUI_COLOR_FOREGROUND);
                            }
                            cui_card_end(ui);
                            cui_pop_id(ui);
                            if (eclick) {
                                snprintf(thread->reasoning_effort, sizeof thread->reasoning_effort,
                                         "%s", eslug);
                                mcode_ui_mark_dirty(app);
                            }
                        }
                    }
                }
            }
        }

        if (supports_fast) {
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 8, .gap = 10,
                    .align = CUI_ALIGN_CENTER) {
                static const char *const k_speed_labels[2] = { "Standard", "Fast" };
                static const char *const k_speed_icons[2] = { NULL, CUI_ICON_ZAP };
                int spd_idx = is_fast ? 1 : 0;
                if (cui_segmented(ui, "speed-tier-seg", k_speed_labels, 2, &spd_idx,
                                  .size = CUI_SIZE_SM, .icons = k_speed_icons)) {
                    thread->speed_mode = (spd_idx == 1) ? MCODE_SPEED_FAST : MCODE_SPEED_STANDARD;
                    is_fast = (spd_idx == 1);
                    mcode_ui_mark_dirty(app);
                }
                if (fast_desc && fast_desc[0]) {
                    cui_text(ui, fast_desc, .size = 12, .color = CUI_COLOR_TEXT_MUTED,
                             .nowrap = true);
                }
            }
        }
        }
    }

    /* Inline bolt toggle right next to the model selector when the active model supports Fast mode */
    if (supports_fast) {
        cui_id zid = cui_make_id(ui, "speed-bolt-btn");
        cui_rect zr = cui_get_rect(ui, zid);
        bool zhov = !busy && zr.w > 0.0f &&
                    mp.x >= zr.x && mp.x <= zr.x + zr.w &&
                    mp.y >= zr.y && mp.y <= zr.y + zr.h;
        cui_color zbg = is_fast
            ? cui_alpha(cui_color_of(ui, CUI_COLOR_WARNING), zhov ? 0.22f : 0.14f)
            : (zhov ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
                    : CUI_RGBA(0x00000001u));
        cui_color zfg = busy
            ? CUI_COLOR_TEXT_MUTED
            : (is_fast ? CUI_COLOR_WARNING : CUI_COLOR_TEXT_MUTED);
        bool zclick = cui_card_begin(ui, &(cui_card_opts){
            .id = "speed-bolt-btn",
            .dir = CUI_ROW,
            .align = CUI_ALIGN_CENTER,
            .width = CUI_PX(30),
            .height = CUI_PX(30),
            .pad = 1,
            .gap = 0,
            .hoverable = !busy,
            .border = -1.0f,
            .radius = 9,
            .bg = zbg,
        });
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_ZAP, 14, zfg);
        }
        cui_card_end(ui);
        char ztip[160];
        if (is_fast) {
            if (fast_desc && fast_desc[0]) {
                snprintf(ztip, sizeof ztip, "Speed: Fast (%s) — click for Standard", fast_desc);
            } else {
                snprintf(ztip, sizeof ztip, "Speed: Fast — click for Standard");
            }
        } else {
            if (fast_desc && fast_desc[0]) {
                snprintf(ztip, sizeof ztip, "Speed: Standard — click for Fast (%s)", fast_desc);
            } else {
                snprintf(ztip, sizeof ztip, "Speed: Standard — click for Fast");
            }
        }
        cui_tooltip(ui, ztip);
        if (zclick && !busy) {
            thread->speed_mode = is_fast ? MCODE_SPEED_STANDARD : MCODE_SPEED_FAST;
            mcode_ui_mark_dirty(app);
        }
    }
}

static void draw_thread_permission_selector(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                            bool busy, cui_placement popover_placement) {
    static const struct {
        mcode_permission_mode mode;
        const char *icon;
        const char *title;
        const char *desc;
    } k_modes[3] = {
        {
            MCODE_PERM_SUPERVISED,
            CUI_ICON_LOCK,
            "Supervised",
            "Ask before commands and file changes.",
        },
        {
            MCODE_PERM_ACCEPT_EDITS,
            CUI_ICON_SQUARE_PEN,
            "Auto-accept edits",
            "Auto-approve edits, ask before other actions.",
        },
        {
            MCODE_PERM_FULL_ACCESS,
            CUI_ICON_TRIANGLE_ALERT,
            "Full access",
            "Allow commands and edits without prompts.",
        },
    };

    mcode_permission_mode cur = thread->perm_mode;
    if (thread->auto_approve) cur = MCODE_PERM_FULL_ACCESS;
    int cur_idx = 1;
    for (int i = 0; i < 3; ++i) {
        if (k_modes[i].mode == cur) { cur_idx = i; break; }
    }

    cui_vec2 mp = cui_mouse_pos(ui);
    cui_id pid = cui_make_id(ui, "perm-mode-btn");
    cui_rect pr = cui_get_rect(ui, pid);
    bool hovered = !busy && pr.w > 0.0f &&
                   mp.x >= pr.x && mp.x <= pr.x + pr.w &&
                   mp.y >= pr.y && mp.y <= pr.y + pr.h;
    bool open = cui_popover_is_open(ui, "perm-mode-pop");

    cui_color bg = (open || hovered)
        ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
        : CUI_RGBA(0x00000001u);
    cui_color fg = busy ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_TEXT_SECONDARY;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "perm-mode-btn",
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_PX(30),
        .height = CUI_PX(30),
        .pad = 1,
        .gap = 0,
        .hoverable = !busy,
        .border = -1.0f,
        .radius = 9,
        .bg = bg,
    });
    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, k_modes[cur_idx].icon, 15, fg);
    }
    cui_card_end(ui);

    char tip[192];
    snprintf(tip, sizeof tip, "%s — %s", k_modes[cur_idx].title, k_modes[cur_idx].desc);
    cui_tooltip(ui, tip);

    if (clicked && !busy) {
        cui_popover_toggle(ui, "perm-mode-pop");
    }

    bool close_pop = false;
    cui_popover(ui, "perm-mode-pop", .placement = popover_placement,
                .width = 336, .pad = 6, .gap = 4) {
        cui_column(ui, .width = CUI_FILL, .gap = 4) {
            for (int i = 0; i < 3; ++i) {
                bool sel = (k_modes[i].mode == cur);
                cui_push_id_int(ui, i);
                cui_id rid = cui_make_id(ui, "pm-row");
                cui_rect rr = cui_get_rect(ui, rid);
                bool rhov = rr.w > 0.0f && mp.x >= rr.x && mp.x <= rr.x + rr.w &&
                            mp.y >= rr.y && mp.y <= rr.y + rr.h;
                cui_color rbg = sel
                    ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.10f)
                    : (rhov ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.06f)
                            : CUI_RGBA(0x00000001u));
                bool row_click = cui_card_begin(ui, &(cui_card_opts){
                    .id = "pm-row",
                    .dir = CUI_ROW,
                    .align = CUI_ALIGN_START,
                    .width = CUI_FILL,
                    .pad_x = 12,
                    .pad_y = 10,
                    .gap = 12,
                    .hoverable = true,
                    .border = -1.0f,
                    .radius = 9,
                    .bg = rbg,
                });
                cui_box(ui, .pad_top = 2) {
                    cui_icon(ui, k_modes[i].icon, 16, CUI_COLOR_TEXT_SECONDARY);
                }
                cui_column(ui, .width = CUI_FILL, .gap = 3) {
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, k_modes[i].title, .font = CUI_FONT_MEDIUM, .size = 13.5f,
                                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL, .nowrap = true);
                        if (sel) {
                            cui_icon(ui, CUI_ICON_CHECK, 14, CUI_COLOR_TEXT_SECONDARY);
                        }
                    }
                    cui_text(ui, k_modes[i].desc, .size = 12.0f,
                             .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL);
                }
                cui_card_end(ui);
                cui_pop_id(ui);

                if (row_click) {
                    thread->perm_mode = k_modes[i].mode;
                    thread->auto_approve = (k_modes[i].mode == MCODE_PERM_FULL_ACCESS);
                    mcode_ui_mark_dirty(app);
                    close_pop = true;
                }
            }
        }
    }
    if (close_pop) {
        cui_popover_close(ui, "perm-mode-pop");
    }
}

static const char *thread_project_display_name(mcode_app *app, const mcode_thread *thread) {
    const mcode_project *proj = mcode_store_find_project(&app->store, thread->project_id);
    if (proj && proj->name[0]) return proj->name;
    if (thread->workdir[0]) {
        const char *slash = strrchr(thread->workdir, '/');
        const char *bslash = strrchr(thread->workdir, '\\');
        const char *sep = (bslash && (!slash || bslash > slash)) ? bslash : slash;
        if (sep && sep[1]) return sep + 1;
        return thread->workdir;
    }
    return NULL;
}

static void draw_composer_options_strip(mcode_app *app, cui_ctx *ui, mcode_thread *thread, bool busy) {
    const mcode_project *comp_proj = mcode_store_find_project(&app->store, thread->project_id);
    bool show_missing = !app->cli_status.available[thread->agent];
    bool show_session = (thread->session_id[0] != '\0');
    bool show_worktree = (thread->turn_count == 0 && !busy && comp_proj && comp_proj->is_git_repo);
    if (!show_missing && !show_session && !show_worktree) return;

    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER, .wrap = true) {
        if (show_missing) {
            cui_badge(ui, "CLI not found", .icon = CUI_ICON_TRIANGLE_ALERT,
                      .tone = CUI_TONE_DANGER, .size = CUI_SIZE_SM);
        }
        if (show_session) {
            char sshort[32];
            snprintf(sshort, sizeof sshort, "Session %.8s", thread->session_id);
            cui_muted(ui, sshort);
        }
        cui_spacer(ui);
        if (show_worktree) {
            bool wt = thread->use_worktree;
            if (cui_chip(ui, "Worktree", &wt)) {
                mcode_ui_toggle_thread_worktree(app, ui, thread);
            }
        }
    }
}

static void draw_thread_composer(mcode_app *app, cui_ctx *ui, mcode_thread *thread, bool centered) {
    bool busy = (thread->active_run != NULL);

    if (!centered) {
        draw_composer_options_strip(app, ui, thread, busy);
    }

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

    int ev = 0;
    float toolbar_left = 10.0f + cui_theme_get(ui)->control_sm + 6.0f;
    cui_placement pop_place = centered ? CUI_PLACE_BOTTOM_START : CUI_PLACE_TOP_START;
    cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL,
            .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END) {
        ev = cui_chat_composer(ui, "thread-composer",
                               thread->composer_draft, sizeof thread->composer_draft,
                               .placeholder = placeholder,
                               .busy = busy,
                               .max_lines = 8,
                               .width = CUI_FILL);

        cui_row(ui, .width = CUI_FILL,
                .pad_left = toolbar_left, .pad_right = 48.0f, .pad_bottom = 11.0f,
                .gap = 4, .align = CUI_ALIGN_CENTER, .clip = true) {
            draw_thread_model_selector(app, ui, thread, busy, pop_place);
            draw_thread_permission_selector(app, ui, thread, busy, pop_place);
        }
    }

    if (centered) {
        draw_composer_options_strip(app, ui, thread, busy);
    }

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

void mcode_ui_draw_chat(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!thread) {
        draw_empty_workspace(app, ui);
        return;
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        if (thread->turn_count == 0) {
            const char *proj_name = thread_project_display_name(app, thread);
            char hero_title[MCODE_NAME_MAX + 64];
            if (proj_name && proj_name[0]) {
                snprintf(hero_title, sizeof hero_title, "What should we work on in %s?", proj_name);
            } else {
                snprintf(hero_title, sizeof hero_title, "What should we work on?");
            }

            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                       .pad = 24, .gap = 0) {
                cui_column(ui, .width = CUI_FILL, .max_width = 760,
                           .align = CUI_ALIGN_CENTER, .gap = 22) {
                    cui_text(ui, hero_title,
                             .font = CUI_FONT_DISPLAY,
                             .size = 32.0f,
                             .color = CUI_COLOR_FOREGROUND,
                             .align = CUI_TEXT_CENTER,
                             .line_height = 1.18f,
                             .width = CUI_FILL);

                    cui_column(ui, .width = CUI_FILL, .gap = 8) {
                        draw_thread_composer(app, ui, thread, true);
                    }
                }
            }
        } else {
            /* Scrollable transcript */
            cui_scroll(ui, .id = "chat-transcript", .width = CUI_FILL, .height = CUI_FILL,
                       .pad = 16, .gap = 16, .fade_edges = true) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER) {
                    cui_column(ui, .width = CUI_FILL, .max_width = 860, .gap = 18) {
                        for (int i = 0; i < thread->turn_count; ++i) {
                            draw_turn(app, ui, thread, i);
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
                    draw_thread_composer(app, ui, thread, false);
                }
            }
        }
    }
}
