#include "mcode_ui_internal.h"
#include <ctype.h>
#include <math.h>
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
    TOOL_KIND_IMAGE,
    TOOL_KIND_AGENT,
    TOOL_KIND_WAIT,
    TOOL_KIND_OTHER,
} tool_kind;

static tool_kind classify_tool_name(const char *name) {
    if (!name || !name[0]) return TOOL_KIND_OTHER;
    if (strstr(name, "image_gen") || strstr(name, "imageGeneration") ||
        strstr(name, "view_image") || strstr(name, "imageView") ||
        strstr(name, "generate_image"))
        return TOOL_KIND_IMAGE;
    if (strstr(name, "subagent") || strstr(name, "collab") ||
        strcmp(name, "Agent") == 0 || strcmp(name, "Task") == 0 ||
        strstr(name, "invoke_subagent"))
        return TOOL_KIND_AGENT;
    if (strcmp(name, "sleep") == 0 || strstr(name, "clock.sleep"))
        return TOOL_KIND_WAIT;
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
    case TOOL_KIND_IMAGE:  return CUI_ICON_IMAGE;
    case TOOL_KIND_AGENT:  return CUI_ICON_CPU;
    case TOOL_KIND_WAIT:   return CUI_ICON_CLOCK;
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

static bool has_non_file_uri_scheme(const char *s) {
    if (!s || !isalpha((unsigned char)s[0])) return false;
    if (link_starts_with_icase(s, "mailto:") || link_starts_with_icase(s, "tel:")) return true;
    const char *p = s + 1;
    while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) ++p;
    return (p[0] == ':' && p[1] == '/' && p[2] == '/');
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

    char resolved[MCODE_PATH_MAX] = "";
    const char *workdir = (thread && thread->workdir[0]) ? thread->workdir : NULL;
    if (mcode_ui_resolve_local_path(raw_link, workdir, true, resolved, sizeof resolved)) {
        if (app && ui && mcode_image_has_supported_ext(resolved) &&
            mcode_ui_image_get(app, ui, resolved) != NULL) {
            mcode_ui_open_image_lightbox(app, resolved, NULL);
        } else {
            cui_app_open_url(resolved);
        }
    } else if (app && ui) {
        char msg[256];
        snprintf(msg, sizeof msg, "File not found: %s", resolved[0] ? resolved : raw_link);
        app_chrome_notify(app->chrome, ui, msg, CUI_TONE_WARNING);
    }
}

/* ---- Inline chat image scanning (Markdown images, image links, @image refs) ---- */

enum {
    MCODE_CHAT_MAX_INLINE_IMAGES = 24,
    MCODE_CHAT_MAX_REF_IMAGES = 8,
};

typedef struct {
    size_t start;
    size_t end;
    char alt[160];
    char resolved[MCODE_PATH_MAX];
} chat_md_image;

typedef struct {
    char label[160];
    char resolved[MCODE_PATH_MAX];
} chat_ref_image;

static bool has_matching_backticks(const char *s, size_t len, size_t pos, size_t count) {
    size_t i = pos;
    while (i < len) {
        if (s[i] == '`') {
            size_t run = 0;
            while (i + run < len && s[i + run] == '`') ++run;
            if (run == count) return true;
            i += run;
        } else {
            ++i;
        }
    }
    return false;
}

static size_t skip_to_next_line(const char *s, size_t len, size_t pos) {
    while (pos < len && s[pos] != '\n') ++pos;
    if (pos < len && s[pos] == '\n') ++pos;
    return pos;
}

/* Parse a markdown `[label](dest ...)` starting at `open_bracket` (where s[open_bracket] == '[').
 * Returns true on a complete balanced link, writing label/dest (trimmed) and end offset past ')'. */
static bool parse_md_bracket_link(const char *s, size_t len, size_t open_bracket,
                                  char *label_out, size_t label_cap,
                                  char *dest_out, size_t dest_cap,
                                  size_t *out_end) {
    if (!s || open_bracket >= len || s[open_bracket] != '[') return false;
    size_t i = open_bracket + 1;
    int bracket_depth = 0;
    size_t label_start = i;
    size_t close_bracket = 0;
    bool found_close = false;

    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (s[i] == '\n' && i + 1 < len && s[i + 1] == '\n') {
            return false;
        }
        if (s[i] == '[') {
            ++bracket_depth;
        } else if (s[i] == ']') {
            if (bracket_depth == 0) {
                close_bracket = i;
                found_close = true;
                break;
            }
            --bracket_depth;
        }
        ++i;
    }
    if (!found_close || close_bracket + 1 >= len || s[close_bracket + 1] != '(') {
        return false;
    }

    size_t j = close_bracket + 2;
    while (j < len && (s[j] == ' ' || s[j] == '\t')) ++j;
    if (j >= len || s[j] == '\n' || s[j] == '\r') return false;

    size_t dest_start = j;
    size_t dest_end = j;
    if (s[j] == '<') {
        ++j;
        dest_start = j;
        while (j < len && s[j] != '>' && s[j] != '\n' && s[j] != '\r') {
            if (s[j] == '\\' && j + 1 < len) {
                j += 2;
                continue;
            }
            ++j;
        }
        if (j >= len || s[j] != '>') return false;
        dest_end = j;
        ++j;
    } else {
        int paren_depth = 0;
        while (j < len) {
            unsigned char ch = (unsigned char)s[j];
            if (ch == '\\' && j + 1 < len) {
                j += 2;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == '(') {
                ++paren_depth;
            } else if (ch == ')') {
                if (paren_depth == 0) break;
                --paren_depth;
            }
            ++j;
        }
        dest_end = j;
    }
    if (dest_end <= dest_start) return false;

    while (j < len && (s[j] == ' ' || s[j] == '\t')) ++j;
    /* Optional title in "...", '...', or (...) */
    if (j < len && (s[j] == '"' || s[j] == '\'' || s[j] == '(')) {
        char close_q = (s[j] == '(') ? ')' : s[j];
        ++j;
        while (j < len && s[j] != close_q && s[j] != '\n' && s[j] != '\r') {
            if (s[j] == '\\' && j + 1 < len) {
                j += 2;
                continue;
            }
            ++j;
        }
        if (j >= len || s[j] != close_q) return false;
        ++j;
        while (j < len && (s[j] == ' ' || s[j] == '\t')) ++j;
    }
    if (j >= len || s[j] != ')') return false;

    if (label_out && label_cap > 0) {
        size_t lstart = label_start;
        size_t lend = close_bracket;
        while (lstart < lend && isspace((unsigned char)s[lstart])) ++lstart;
        while (lend > lstart && isspace((unsigned char)s[lend - 1])) --lend;
        size_t llen = lend - lstart;
        if (llen >= label_cap) llen = label_cap - 1;
        memcpy(label_out, s + lstart, llen);
        label_out[llen] = '\0';
    }
    if (dest_out && dest_cap > 0) {
        size_t dlen = dest_end - dest_start;
        if (dlen >= dest_cap) dlen = dest_cap - 1;
        memcpy(dest_out, s + dest_start, dlen);
        dest_out[dlen] = '\0';
    }
    if (out_end) *out_end = j + 1;
    return true;
}

static bool has_resolved_in_inline(const chat_md_image *imgs, int count, const char *resolved) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(imgs[i].resolved, resolved) == 0) return true;
    }
    return false;
}

static bool has_resolved_in_refs(const chat_ref_image *refs, int count, const char *resolved) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(refs[i].resolved, resolved) == 0) return true;
    }
    return false;
}

static void scan_markdown_images(mcode_app *app, cui_ctx *ui, const char *workdir,
                                 const char *s, size_t len,
                                 chat_md_image *inline_out, int *inline_count,
                                 chat_ref_image *ref_out, int *ref_count) {
    *inline_count = 0;
    *ref_count = 0;
    if (!app || !ui || !s || len == 0) return;

    bool at_line_start = true;
    bool in_fence = false;
    char fence_char = 0;
    size_t fence_len = 0;
    size_t code_ticks = 0;

    size_t i = 0;
    while (i < len) {
        if (at_line_start) {
            size_t p = i;
            int spaces = 0;
            while (p < len && s[p] == ' ' && spaces < 3) { ++p; ++spaces; }
            if (p < len && (s[p] == '`' || s[p] == '~')) {
                char fc = s[p];
                size_t fl = 0;
                while (p + fl < len && s[p + fl] == fc) ++fl;
                if (fl >= 3) {
                    if (!in_fence) {
                        bool ok = true;
                        if (fc == '`') {
                            for (size_t q = p + fl; q < len && s[q] != '\n'; ++q) {
                                if (s[q] == '`') { ok = false; break; }
                            }
                        }
                        if (ok) {
                            in_fence = true;
                            fence_char = fc;
                            fence_len = fl;
                            code_ticks = 0;
                            i = skip_to_next_line(s, len, p + fl);
                            at_line_start = true;
                            continue;
                        }
                    } else if (fc == fence_char && fl >= fence_len) {
                        size_t q = p + fl;
                        while (q < len && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r')) ++q;
                        if (q >= len || s[q] == '\n') {
                            in_fence = false;
                            i = (q < len) ? q + 1 : len;
                            at_line_start = true;
                            continue;
                        }
                    }
                }
            }
            if (in_fence) {
                i = skip_to_next_line(s, len, i);
                at_line_start = true;
                continue;
            }
        }

        if (code_ticks == 0 && s[i] == '\\' && i + 1 < len) {
            at_line_start = (s[i + 1] == '\n');
            i += 2;
            continue;
        }

        if (s[i] == '`') {
            size_t run = 0;
            while (i + run < len && s[i + run] == '`') ++run;
            if (code_ticks == 0) {
                if (has_matching_backticks(s, len, i + run, run)) {
                    code_ticks = run;
                }
            } else if (run == code_ticks) {
                code_ticks = 0;
            }
            i += run;
            at_line_start = false;
            continue;
        }

        if (code_ticks > 0) {
            at_line_start = (s[i] == '\n');
            ++i;
            continue;
        }

        /* Case 1: Markdown image ![alt](dest), optionally wrapped as [![alt](dest)](url) */
        if (s[i] == '!' && i + 1 < len && s[i + 1] == '[') {
            char alt[160] = "";
            char dest[MCODE_PATH_MAX] = "";
            size_t end = 0;
            if (parse_md_bracket_link(s, len, i + 1, alt, sizeof alt, dest, sizeof dest, &end)) {
                char resolved[MCODE_PATH_MAX] = "";
                if (mcode_image_has_supported_ext(dest) &&
                    mcode_ui_resolve_local_path(dest, workdir, false, resolved, sizeof resolved) &&
                    mcode_ui_image_get(app, ui, resolved) != NULL) {
                    size_t span_start = i;
                    size_t span_end = end;
                    if (i > 0 && s[i - 1] == '[' && end < len && s[end] == ']' &&
                        end + 1 < len && s[end + 1] == '(') {
                        size_t outer_end = 0;
                        if (parse_md_bracket_link(s, len, i - 1, NULL, 0, NULL, 0, &outer_end)) {
                            span_start = i - 1;
                            span_end = outer_end;
                        }
                    }
                    if (*inline_count < MCODE_CHAT_MAX_INLINE_IMAGES) {
                        chat_md_image *im = &inline_out[(*inline_count)++];
                        im->start = span_start;
                        im->end = span_end;
                        snprintf(im->alt, sizeof im->alt, "%s", alt);
                        snprintf(im->resolved, sizeof im->resolved, "%s", resolved);
                    }
                    at_line_start = false;
                    i = span_end;
                    continue;
                }
                at_line_start = false;
                i = end;
                continue;
            }
        }

        /* Case 2: Regular markdown link [label](dest) targeting a local PNG/JPEG */
        if (s[i] == '[' && !(i + 2 < len && s[i + 1] == '!' && s[i + 2] == '[')) {
            char label[160] = "";
            char dest[MCODE_PATH_MAX] = "";
            size_t end = 0;
            if (parse_md_bracket_link(s, len, i, label, sizeof label, dest, sizeof dest, &end)) {
                char resolved[MCODE_PATH_MAX] = "";
                if (mcode_image_has_supported_ext(dest) &&
                    mcode_ui_resolve_local_path(dest, workdir, false, resolved, sizeof resolved) &&
                    mcode_ui_image_get(app, ui, resolved) != NULL) {
                    if (*ref_count < MCODE_CHAT_MAX_REF_IMAGES &&
                        !has_resolved_in_refs(ref_out, *ref_count, resolved)) {
                        chat_ref_image *rf = &ref_out[(*ref_count)++];
                        snprintf(rf->label, sizeof rf->label, "%s", label);
                        snprintf(rf->resolved, sizeof rf->resolved, "%s", resolved);
                    }
                }
                at_line_start = false;
                i = end;
                continue;
            }
        }

        /* Case 3: Bare file:// URI or absolute/home path to an existing local PNG/JPEG */
        if (i == 0 || isspace((unsigned char)s[i - 1]) ||
            s[i - 1] == '<' || s[i - 1] == '(' || s[i - 1] == '"' || s[i - 1] == '\'') {
            bool is_file_uri = link_starts_with_icase(s + i, "file://");
            bool is_home_path = (s[i] == '~' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '\\'));
            bool is_abs_path = (s[i] == '/' && i + 1 < len && !isspace((unsigned char)s[i + 1]) &&
                                s[i + 1] != '/');
            if (is_file_uri || is_home_path || is_abs_path) {
                size_t end = i;
                while (end < len && !isspace((unsigned char)s[end]) &&
                       s[end] != '>' && s[end] != ')' && s[end] != ']' &&
                       s[end] != '"' && s[end] != '\'') {
                    ++end;
                }
                char tok[MCODE_PATH_MAX];
                size_t tlen = end - i;
                if (tlen >= sizeof tok) tlen = sizeof tok - 1;
                memcpy(tok, s + i, tlen);
                tok[tlen] = '\0';
                while (tlen > 0 && (tok[tlen - 1] == '.' || tok[tlen - 1] == ',' ||
                                    tok[tlen - 1] == ';' || tok[tlen - 1] == ':')) {
                    tok[--tlen] = '\0';
                }
                char resolved[MCODE_PATH_MAX] = "";
                if (mcode_image_has_supported_ext(tok) &&
                    mcode_ui_resolve_local_path(tok, workdir, false, resolved, sizeof resolved) &&
                    mcode_ui_image_get(app, ui, resolved) != NULL) {
                    if (*ref_count < MCODE_CHAT_MAX_REF_IMAGES &&
                        !has_resolved_in_refs(ref_out, *ref_count, resolved)) {
                        chat_ref_image *rf = &ref_out[(*ref_count)++];
                        rf->label[0] = '\0';
                        snprintf(rf->resolved, sizeof rf->resolved, "%s", resolved);
                    }
                }
                at_line_start = false;
                i = end;
                continue;
            }
        }

        at_line_start = (s[i] == '\n');
        ++i;
    }

    /* Filter out referenced links that were already rendered inline as ![alt](path). */
    int kept = 0;
    for (int r = 0; r < *ref_count; ++r) {
        if (!has_resolved_in_inline(inline_out, *inline_count, ref_out[r].resolved)) {
            if (kept != r) ref_out[kept] = ref_out[r];
            ++kept;
        }
    }
    *ref_count = kept;
}

static bool is_token_boundary_before(const char *s, size_t i) {
    if (i == 0) return true;
    unsigned char prev = (unsigned char)s[i - 1];
    return isspace(prev) || prev == '(' || prev == '[' || prev == '"' || prev == '\'' || prev == '<';
}

static bool is_token_delim(unsigned char c) {
    return isspace(c) || c == ')' || c == ']' || c == '>' || c == '"' || c == '\'' || c == ',' || c == ';';
}

static void trim_trailing_punct(char *tok) {
    size_t n = strlen(tok);
    while (n > 0 && (tok[n - 1] == '.' || tok[n - 1] == ',' || tok[n - 1] == ';' ||
                     tok[n - 1] == ':' || tok[n - 1] == ')' || tok[n - 1] == ']' ||
                     tok[n - 1] == '>' || tok[n - 1] == '"' || tok[n - 1] == '\'')) {
        tok[--n] = '\0';
    }
}

/* Try to match an unquoted path with spaces on the same line ending in .png/.jpg/.jpeg. */
static size_t find_spaced_image_end(const char *s, size_t len, size_t start) {
    static const char *const k_exts[] = { ".png", ".jpg", ".jpeg" };
    size_t line_end = start;
    while (line_end < len && s[line_end] != '\n' && s[line_end] != '\r') ++line_end;
    if (line_end - start >= MCODE_PATH_MAX) line_end = start + MCODE_PATH_MAX - 1;

    for (size_t p = start; p < line_end; ++p) {
        if (s[p] != '.') continue;
        for (size_t ei = 0; ei < sizeof k_exts / sizeof k_exts[0]; ++ei) {
            size_t elen = strlen(k_exts[ei]);
            if (p + elen <= line_end && link_starts_with_icase(s + p, k_exts[ei])) {
                size_t after = p + elen;
                if (after == line_end || is_token_delim((unsigned char)s[after])) {
                    return after;
                }
            }
        }
    }
    return 0;
}

static void add_prompt_ref_if_valid(mcode_app *app, cui_ctx *ui, const char *workdir,
                                    const char *raw_candidate, const char *label_opt,
                                    chat_ref_image *out, int *count, int max_count) {
    if (!raw_candidate || !raw_candidate[0] || *count >= max_count) return;
    if (!mcode_image_has_supported_ext(raw_candidate)) return;
    char resolved[MCODE_PATH_MAX] = "";
    if (!mcode_ui_resolve_local_path(raw_candidate, workdir, false, resolved, sizeof resolved)) return;
    if (has_resolved_in_refs(out, *count, resolved)) return;
    if (!mcode_ui_image_get(app, ui, resolved)) return;

    chat_ref_image *rf = &out[(*count)++];
    snprintf(rf->resolved, sizeof rf->resolved, "%s", resolved);
    snprintf(rf->label, sizeof rf->label, "%s", (label_opt && label_opt[0]) ? label_opt : "");
}

static int extract_prompt_images(mcode_app *app, cui_ctx *ui, const char *workdir,
                                 const char *s, chat_ref_image *out, int max_count) {
    int count = 0;
    if (!app || !ui || !s || !s[0] || max_count <= 0) return 0;
    size_t len = strlen(s);
    size_t code_ticks = 0;
    size_t i = 0;

    while (i < len && count < max_count) {
        if (s[i] == '`') {
            size_t run = 0;
            while (i + run < len && s[i + run] == '`') ++run;
            if (code_ticks == 0) {
                if (has_matching_backticks(s, len, i + run, run)) code_ticks = run;
            } else if (run == code_ticks) {
                code_ticks = 0;
            }
            i += run;
            continue;
        }
        if (code_ticks > 0) {
            ++i;
            continue;
        }

        /* Markdown ![alt](dest) or [label](dest) */
        if ((s[i] == '!' && i + 1 < len && s[i + 1] == '[') || s[i] == '[') {
            size_t ob = (s[i] == '!') ? i + 1 : i;
            char label[160] = "";
            char dest[MCODE_PATH_MAX] = "";
            size_t end = 0;
            if (parse_md_bracket_link(s, len, ob, label, sizeof label, dest, sizeof dest, &end)) {
                add_prompt_ref_if_valid(app, ui, workdir, dest, label, out, &count, max_count);
                i = end;
                continue;
            }
        }

        if (!is_token_boundary_before(s, i)) {
            ++i;
            continue;
        }

        /* @path references (from composer attach or typed manually) */
        if (s[i] == '@' && i + 1 < len && !isspace((unsigned char)s[i + 1])) {
            size_t start = i + 1;
            size_t end = start;
            if (s[start] == '"' || s[start] == '\'') {
                char q = s[start++];
                end = start;
                while (end < len && s[end] != q && s[end] != '\n' && s[end] != '\r') ++end;
                char tok[MCODE_PATH_MAX];
                size_t tlen = end - start;
                if (tlen >= sizeof tok) tlen = sizeof tok - 1;
                memcpy(tok, s + start, tlen);
                tok[tlen] = '\0';
                add_prompt_ref_if_valid(app, ui, workdir, tok, NULL, out, &count, max_count);
                i = (end < len && s[end] == q) ? end + 1 : end;
                continue;
            }
            while (end < len && !is_token_delim((unsigned char)s[end])) ++end;
            char tok[MCODE_PATH_MAX];
            size_t tlen = end - start;
            if (tlen >= sizeof tok) tlen = sizeof tok - 1;
            memcpy(tok, s + start, tlen);
            tok[tlen] = '\0';
            trim_trailing_punct(tok);
            int before = count;
            add_prompt_ref_if_valid(app, ui, workdir, tok, NULL, out, &count, max_count);
            if (count == before) {
                size_t spaced_end = find_spaced_image_end(s, len, start);
                if (spaced_end > end) {
                    tlen = spaced_end - start;
                    if (tlen >= sizeof tok) tlen = sizeof tok - 1;
                    memcpy(tok, s + start, tlen);
                    tok[tlen] = '\0';
                    add_prompt_ref_if_valid(app, ui, workdir, tok, NULL, out, &count, max_count);
                    if (count > before) end = spaced_end;
                }
            }
            i = end;
            continue;
        }

        /* file:// URI or bare absolute / ~/ image paths */
        bool is_file_uri = link_starts_with_icase(s + i, "file://");
        bool is_home_path = (s[i] == '~' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '\\'));
        bool is_abs_path = (s[i] == '/' && i + 1 < len && !isspace((unsigned char)s[i + 1]));
        if (is_file_uri || is_home_path || is_abs_path) {
            size_t start = i;
            size_t end = start;
            while (end < len && !is_token_delim((unsigned char)s[end])) ++end;
            char tok[MCODE_PATH_MAX];
            size_t tlen = end - start;
            if (tlen >= sizeof tok) tlen = sizeof tok - 1;
            memcpy(tok, s + start, tlen);
            tok[tlen] = '\0';
            trim_trailing_punct(tok);
            int before = count;
            add_prompt_ref_if_valid(app, ui, workdir, tok, NULL, out, &count, max_count);
            if (count == before && !is_file_uri) {
                size_t spaced_end = find_spaced_image_end(s, len, start);
                if (spaced_end > end) {
                    tlen = spaced_end - start;
                    if (tlen >= sizeof tok) tlen = sizeof tok - 1;
                    memcpy(tok, s + start, tlen);
                    tok[tlen] = '\0';
                    add_prompt_ref_if_valid(app, ui, workdir, tok, NULL, out, &count, max_count);
                    if (count > before) end = spaced_end;
                }
            }
            i = end;
            continue;
        }

        ++i;
    }

    return count;
}

static void draw_markdown_slice(mcode_app *app, cui_ctx *ui, const mcode_thread *thread,
                                const char *id, const char *txt, size_t len,
                                float fallback_md_w, bool streaming) {
    if (!txt || len == 0) return;
    cui_id md_id = cui_make_id(ui, id);
    bool has_md_rect = cui_get_rect(ui, md_id).w > 0.0f;
    cui_size md_width = (!has_md_rect && fallback_md_w > 0.0f)
        ? CUI_PX(fallback_md_w)
        : CUI_FILL;
    cui_markdown_result mr = cui_markdown(ui, id, txt,
                                          .length = (int)len,
                                          .width = md_width,
                                          .streaming = streaming);
    if (!has_md_rect && fallback_md_w > 0.0f) {
        cui_request_redraw(ui);
    }
    if (mr.link && mr.link[0]) {
        open_markdown_link(app, ui, thread, mr.link);
    }
}

static bool trim_markdown_slice(const char *txt, size_t start, size_t end,
                                size_t *out_start, size_t *out_end) {
    while (start < end && (txt[start] == '\r' || txt[start] == '\n')) ++start;
    while (end > start && (txt[end - 1] == '\r' || txt[end - 1] == '\n')) --end;
    bool has_non_ws = false;
    for (size_t p = start; p < end; ++p) {
        if (!isspace((unsigned char)txt[p])) {
            has_non_ws = true;
            break;
        }
    }
    if (!has_non_ws) return false;
    *out_start = start;
    *out_end = end;
    return true;
}

static void draw_assistant_markdown_block(mcode_app *app, cui_ctx *ui, const mcode_thread *thread,
                                          const char *bid, const char *txt, size_t len,
                                          float fallback_md_w, bool streaming) {
    if (!txt || len == 0) return;
    const char *workdir = (thread && thread->workdir[0]) ? thread->workdir : NULL;

    chat_md_image inline_imgs[MCODE_CHAT_MAX_INLINE_IMAGES];
    chat_ref_image ref_imgs[MCODE_CHAT_MAX_REF_IMAGES];
    int inline_count = 0;
    int ref_count = 0;
    scan_markdown_images(app, ui, workdir, txt, len,
                         inline_imgs, &inline_count, ref_imgs, &ref_count);

    float col_w = (fallback_md_w > 0.0f) ? fallback_md_w : 640.0f;
    float img_max_w = col_w - 16.0f;
    if (img_max_w > 460.0f) img_max_w = 460.0f;
    if (img_max_w < 160.0f) img_max_w = 160.0f;

    if (inline_count == 0 && ref_count == 0) {
        draw_markdown_slice(app, ui, thread, bid, txt, len, fallback_md_w, streaming);
        return;
    }

    cui_column(ui, .width = CUI_FILL, .gap = 8) {
        if (inline_count == 0) {
            draw_markdown_slice(app, ui, thread, bid, txt, len, fallback_md_w, streaming);
        } else {
            size_t cursor = 0;
            for (int ii = 0; ii < inline_count; ++ii) {
                size_t seg_s = 0, seg_e = 0;
                if (trim_markdown_slice(txt, cursor, inline_imgs[ii].start, &seg_s, &seg_e)) {
                    char seg_id[64];
                    snprintf(seg_id, sizeof seg_id, "%s-md-%d", bid, ii);
                    draw_markdown_slice(app, ui, thread, seg_id,
                                        txt + seg_s, seg_e - seg_s, fallback_md_w, false);
                }
                char img_id[64];
                snprintf(img_id, sizeof img_id, "%s-img-%d", bid, ii);
                mcode_ui_draw_image_preview_card(app, ui, img_id,
                                                 inline_imgs[ii].resolved,
                                                 inline_imgs[ii].alt,
                                                 img_max_w, 280.0f, false);
                cursor = inline_imgs[ii].end;
            }
            size_t tail_s = 0, tail_e = 0;
            if (trim_markdown_slice(txt, cursor, len, &tail_s, &tail_e)) {
                char seg_id[64];
                snprintf(seg_id, sizeof seg_id, "%s-md-tail", bid);
                draw_markdown_slice(app, ui, thread, seg_id,
                                    txt + tail_s, tail_e - tail_s, fallback_md_w, streaming);
            }
        }

        if (ref_count > 0) {
            cui_row(ui, .width = CUI_FILL, .gap = 8, .wrap = true, .align = CUI_ALIGN_START) {
                for (int ri = 0; ri < ref_count; ++ri) {
                    char ref_id[64];
                    snprintf(ref_id, sizeof ref_id, "%s-refimg-%d", bid, ri);
                    float thumb_w = (ref_count == 1) ? (img_max_w < 320.0f ? img_max_w : 320.0f) : 180.0f;
                    float thumb_h = (ref_count == 1) ? 200.0f : 124.0f;
                    mcode_ui_draw_image_preview_card(app, ui, ref_id,
                                                     ref_imgs[ri].resolved,
                                                     ref_imgs[ri].label,
                                                     thumb_w, thumb_h, ref_count > 1);
                }
            }
        }
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
    case TOOL_KIND_IMAGE:
        if (strstr(b->tool_name, "view") || strstr(b->tool_name, "View")) {
            verb = running ? "Viewing" : "Viewed";
            fallback = "image";
        } else {
            verb = running ? "Generating image" : "Generated image";
            fallback = "";
            detail[0] = '\0';
        }
        break;
    case TOOL_KIND_AGENT:
        verb = running ? "Running subagent" : "Subagent";
        fallback = "task";
        break;
    case TOOL_KIND_WAIT:
        verb = running ? "Waiting" : "Waited";
        fallback = "";
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
    int n_cmd = 0, n_edit = 0, n_read = 0, n_search = 0, n_web = 0, n_image = 0, n_agent = 0, n_other = 0;
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
        case TOOL_KIND_IMAGE:  ++n_image; break;
        case TOOL_KIND_AGENT:  ++n_agent; break;
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
    if (n_image > 0)  append_clause(out, cap, &pos, "processed %d image%s", n_image);
    if (n_agent > 0)  append_clause(out, cap, &pos, "%d subagent%s", n_agent);
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
                "with inline diff reviews and an integrated shell.");

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

static void draw_user_message(mcode_app *app, cui_ctx *ui, const char *thread_id,
                              const char *workdir, const char *text, bool steered) {
    if (!text) text = "";
    cui_push_id(ui, (thread_id && thread_id[0]) ? thread_id : "thread");
    cui_push_id(ui, "user-msg");

    chat_ref_image prompt_imgs[MCODE_CHAT_MAX_REF_IMAGES];
    int prompt_img_count = extract_prompt_images(app, ui, workdir, text,
                                                 prompt_imgs, MCODE_CHAT_MAX_REF_IMAGES);

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    bool has_accent = app_theme_has_accent(ui);
    cui_color surface = cui_color_of(ui, CUI_COLOR_SURFACE);
    cui_color bg;
    cui_color border_col;
    if (!has_accent) {
        cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
        bg = cui_mix(surface, fg, dark ? 0.08f : 0.055f);
        border_col = steered
            ? cui_alpha(cui_color_of(ui, CUI_COLOR_INFO), dark ? 0.45f : 0.35f)
            : cui_color_of(ui, dark ? CUI_COLOR_BORDER : CUI_COLOR_BORDER_SUBTLE);
    } else {
        cui_color acc = cui_color_of(ui, CUI_COLOR_ACCENT);
        bg = cui_mix(surface, acc, dark ? (steered ? 0.18f : 0.14f) : (steered ? 0.12f : 0.09f));
        border_col = cui_alpha(acc, dark ? (steered ? 0.50f : 0.30f) : (steered ? 0.40f : 0.22f));
    }

    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END, .pad_top = 10) {
        cui_column(ui, .max_width = 480, .pad_x = 13, .pad_y = 8, .gap = 6,
                   .bg = bg, .border = 1.0f, .border_color = border_col,
                   .radius_tl = 18, .radius_tr = 18, .radius_br = 6, .radius_bl = 18) {
            if (steered) {
                cui_color tag_col = has_accent
                    ? cui_color_of(ui, CUI_COLOR_ACCENT)
                    : cui_color_of(ui, CUI_COLOR_INFO);
                cui_row(ui, .gap = 4, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_ARROW_RIGHT, 11, tag_col);
                    cui_text(ui, "Steered", .font = CUI_FONT_MEDIUM, .size = 11.0f,
                             .color = tag_col, .nowrap = true);
                }
            }
            if (prompt_img_count > 0) {
                cui_row(ui, .gap = 6, .wrap = true, .align = CUI_ALIGN_START) {
                    float max_w = (prompt_img_count == 1) ? 220.0f : 124.0f;
                    float max_h = (prompt_img_count == 1) ? 140.0f : 88.0f;
                    for (int pi = 0; pi < prompt_img_count; ++pi) {
                        char pid[32];
                        snprintf(pid, sizeof pid, "uimg-%d", pi);
                        mcode_ui_draw_image_preview_card(app, ui, pid,
                                                         prompt_imgs[pi].resolved,
                                                         prompt_imgs[pi].label,
                                                         max_w, max_h, true);
                    }
                }
            }
            cui_text(ui, text, .color = CUI_COLOR_FOREGROUND,
                     .line_height = 1.45f, .selectable = true);
        }
    }

    cui_pop_id(ui);
    cui_pop_id(ui);
}

static void draw_turn(mcode_app *app, cui_ctx *ui, mcode_thread *thread, int turn_idx,
                      float fallback_md_w) {
    mcode_turn *turn = &thread->turns[turn_idx];
    bool is_last_turn = (turn_idx == thread->turn_count - 1);
    cui_push_id_int(ui, turn_idx);

    cui_column(ui, .width = CUI_FILL, .gap = 10) {
        /* User prompt bubble */
        draw_user_message(app, ui, thread->id, thread->workdir,
                          mcode_str_cstr(&turn->prompt), turn->steered);

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
                    draw_assistant_markdown_block(app, ui, thread, bid, txt, b->text.len,
                                                  fallback_md_w, streaming);
                }
            } else if (b->kind == MCODE_BLOCK_IMAGE) {
                char resolved[MCODE_PATH_MAX] = "";
                const char *workdir = (thread && thread->workdir[0]) ? thread->workdir : NULL;
                const char *img_path = b->image_path;
                if (img_path[0] &&
                    mcode_ui_resolve_local_path(img_path, workdir, false, resolved, sizeof resolved)) {
                    img_path = resolved;
                }
                float col_w = (fallback_md_w > 0.0f) ? fallback_md_w : 640.0f;
                float img_max_w = col_w - 16.0f;
                if (img_max_w > 540.0f) img_max_w = 540.0f;
                if (img_max_w < 160.0f) img_max_w = 160.0f;
                const char *caption = mcode_str_cstr(&b->text);
                if (!mcode_ui_draw_image_preview_card(app, ui, bid, img_path, caption,
                                                      img_max_w, 380.0f, false)) {
                    cui_row(ui, .gap = 8, .pad_x = 12, .pad_y = 8, .radius = 8,
                            .border = 1.0f, .border_color = CUI_COLOR_BORDER,
                            .bg = CUI_COLOR_SURFACE_MUTED, .align = CUI_ALIGN_CENTER) {
                        cui_icon(ui, CUI_ICON_IMAGE, 14, CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, b->image_path[0] ? b->image_path : "Image unavailable",
                                 .size = 12.5f, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            } else if (b->kind == MCODE_BLOCK_PLAN) {
                const char *ptxt = mcode_str_cstr(&b->text);
                if (ptxt[0]) {
                    last_text = ptxt;
                    bool streaming = turn->running && (bi == turn->block_count - 1);
                    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
                    cui_color info_c = cui_color_of(ui, CUI_COLOR_INFO);
                    cui_column(ui, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 10,
                               .border = 1.0f,
                               .border_color = cui_alpha(info_c, dark ? 0.42f : 0.35f),
                               .bg = cui_alpha(info_c, dark ? 0.07f : 0.045f)) {
                        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                            cui_icon(ui, CUI_ICON_COMPASS, 15, info_c);
                            cui_text(ui, "Proposed Plan", .font = CUI_FONT_MEDIUM, .size = 13.0f,
                                     .color = CUI_COLOR_FOREGROUND, .nowrap = true);
                            cui_spacer(ui);
                            cui_push_id(ui, bid);
                            cui_copy_button(ui, "copy-plan", ptxt);
                            cui_pop_id(ui);
                        }
                        cui_separator(ui);
                        float inner_w = (fallback_md_w > 32.0f) ? (fallback_md_w - 30.0f) : 0.0f;
                        draw_assistant_markdown_block(app, ui, thread, bid, ptxt, b->text.len,
                                                      inner_w, streaming);
                    }
                }
            } else if (b->kind == MCODE_BLOCK_NOTICE) {
                const char *ntxt = mcode_str_cstr(&b->text);
                if (ntxt[0]) {
                    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
                    if (b->notice_tone == CUI_TONE_WARNING || b->notice_tone == CUI_TONE_INFO) {
                        cui_color tc = cui_color_of(ui, b->notice_tone == CUI_TONE_WARNING
                                                        ? CUI_COLOR_WARNING : CUI_COLOR_INFO);
                        const char *ic = (b->notice_tone == CUI_TONE_WARNING)
                            ? CUI_ICON_TRIANGLE_ALERT : CUI_ICON_INFO;
                        cui_row(ui, .width = CUI_FILL, .pad_x = 11, .pad_y = 7, .gap = 8,
                                .radius = 8, .border = 1.0f,
                                .border_color = cui_alpha(tc, dark ? 0.40f : 0.32f),
                                .bg = cui_alpha(tc, dark ? 0.08f : 0.055f),
                                .align = CUI_ALIGN_CENTER) {
                            cui_icon(ui, ic, 14, tc);
                            cui_text(ui, ntxt, .size = 12.5f, .color = CUI_COLOR_FOREGROUND,
                                     .width = CUI_FILL, .selectable = true);
                        }
                    } else {
                        cui_row(ui, .width = CUI_FILL, .gap = 10, .pad_y = 2,
                                .align = CUI_ALIGN_CENTER) {
                            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1),
                                    .bg = CUI_COLOR_BORDER_SUBTLE) {}
                            cui_text(ui, ntxt, .font = CUI_FONT_MEDIUM, .size = 11.5f,
                                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1),
                                    .bg = CUI_COLOR_BORDER_SUBTLE) {}
                        }
                    }
                }
            } else if (b->kind == MCODE_BLOCK_ERROR) {
                cui_alert(ui, "Agent error", .description = mcode_str_cstr(&b->text),
                          .tone = CUI_TONE_DANGER);
            } else if (b->kind == MCODE_BLOCK_STEER) {
                const char *stxt = mcode_str_cstr(&b->text);
                if (stxt[0]) {
                    cui_push_id(ui, bid);
                    draw_user_message(app, ui, thread->id, thread->workdir, stxt, true);
                    cui_pop_id(ui);
                }
            }
        }

        if (turn->plan_step_count > 0 && turn->plan_steps) {
            cui_plan_step steps[MCODE_MAX_PLAN_STEPS];
            int nsteps = turn->plan_step_count < MCODE_MAX_PLAN_STEPS
                ? turn->plan_step_count : MCODE_MAX_PLAN_STEPS;
            for (int pi = 0; pi < nsteps; ++pi) {
                steps[pi].title = turn->plan_steps[pi].title;
                steps[pi].detail = turn->plan_steps[pi].detail[0] ? turn->plan_steps[pi].detail : NULL;
                steps[pi].status = turn->plan_steps[pi].status;
            }
            char pid[48];
            snprintf(pid, sizeof pid, "turn-plan-%d", turn_idx);
            (void)cui_agent_plan(ui, pid, steps, nsteps,
                                 .title = turn->plan_title[0] ? turn->plan_title : "Plan");
        }

        if (is_last_turn && turn->running && thread->pending_question.active &&
            thread->pending_question.count > 0) {
            mcode_pending_question *pq = &thread->pending_question;
            bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
            cui_color acc = cui_color_of(ui, app_theme_has_accent(ui) ? CUI_COLOR_ACCENT : CUI_COLOR_INFO);
            cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
            cui_vec2 mp = cui_mouse_pos(ui);
            bool submit_now = false;
            bool dismiss_now = false;
            bool any_answered = false;

            cui_column(ui, .id = "question-card", .width = CUI_FILL, .pad = 14, .gap = 12,
                       .radius = 10, .border = 1.0f,
                       .border_color = cui_alpha(acc, dark ? 0.50f : 0.38f),
                       .bg = cui_alpha(acc, dark ? 0.075f : 0.045f)) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_CIRCLE_HELP, 16, acc);
                    cui_text(ui, pq->count == 1 ? "Question from agent" : "Questions from agent",
                             .font = CUI_FONT_MEDIUM, .size = 13.5f,
                             .color = CUI_COLOR_FOREGROUND);
                }

                for (int qi = 0; qi < pq->count; ++qi) {
                    mcode_question_item *item = &pq->items[qi];
                    cui_push_id_int(ui, qi);
                    cui_column(ui, .width = CUI_FILL, .gap = 6) {
                        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER, .wrap = true) {
                            if (item->header[0]) {
                                cui_badge(ui, item->header, .tone = CUI_TONE_INFO, .size = CUI_SIZE_SM);
                            }
                            if (item->multi_select) {
                                cui_text(ui, "Select one or more", .size = 11.5f,
                                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                            }
                        }
                        cui_text(ui, item->question, .font = CUI_FONT_MEDIUM, .size = 13.0f,
                                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL);

                        if (item->option_count > 0) {
                            cui_column(ui, .width = CUI_FILL, .gap = 5) {
                                for (int oi = 0; oi < item->option_count; ++oi) {
                                    bool sel = (item->selected_mask & (1u << oi)) != 0;
                                    cui_push_id_int(ui, oi);
                                    cui_id oid = cui_make_id(ui, "qopt");
                                    cui_rect or_ = cui_get_rect(ui, oid);
                                    bool hov = or_.w > 0.0f &&
                                               mp.x >= or_.x && mp.x <= or_.x + or_.w &&
                                               mp.y >= or_.y && mp.y <= or_.y + or_.h;
                                    cui_color obg = sel
                                        ? cui_alpha(acc, dark ? 0.18f : 0.12f)
                                        : (hov ? cui_alpha(fg, dark ? 0.07f : 0.045f)
                                               : cui_color_of(ui, CUI_COLOR_SURFACE));
                                    cui_color obdr = sel
                                        ? cui_alpha(acc, dark ? 0.65f : 0.55f)
                                        : cui_color_of(ui, CUI_COLOR_BORDER);
                                    bool oclick = cui_card_begin(ui, &(cui_card_opts){
                                        .id = "qopt",
                                        .dir = CUI_ROW,
                                        .align = CUI_ALIGN_START,
                                        .width = CUI_FILL,
                                        .pad_x = 10,
                                        .pad_y = 8,
                                        .gap = 9,
                                        .hoverable = true,
                                        .border = 1.0f,
                                        .border_color = obdr,
                                        .radius = 8,
                                        .bg = obg,
                                    });
                                    cui_box(ui, .pad_top = 2) {
                                        const char *ind = item->multi_select
                                            ? (sel ? CUI_ICON_SQUARE_CHECK : CUI_ICON_SQUARE)
                                            : (sel ? CUI_ICON_CIRCLE_DOT : CUI_ICON_CIRCLE);
                                        cui_icon(ui, ind, 14, sel ? acc : CUI_COLOR_TEXT_MUTED);
                                    }
                                    cui_column(ui, .width = CUI_FILL, .gap = 2) {
                                        cui_text(ui, item->options[oi].label,
                                                 .font = CUI_FONT_MEDIUM, .size = 12.5f,
                                                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL);
                                        if (item->options[oi].description[0]) {
                                            cui_text(ui, item->options[oi].description,
                                                     .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED,
                                                     .width = CUI_FILL);
                                        }
                                    }
                                    cui_card_end(ui);
                                    cui_pop_id(ui);

                                    if (oclick) {
                                        if (item->multi_select) {
                                            item->selected_mask ^= (1u << oi);
                                        } else {
                                            item->selected_mask = (1u << oi);
                                        }
                                    }
                                }
                            }
                        }

                        if (item->allow_other || item->option_count == 0) {
                            int inf = cui_input(ui, "q-other", item->other_text, sizeof item->other_text,
                                                .placeholder = item->option_count > 0
                                                    ? "Or type a custom answer..."
                                                    : "Type your answer...",
                                                .password = item->is_secret,
                                                .size = CUI_SIZE_SM,
                                                .width = CUI_FILL);
                            if ((inf & CUI_SUBMITTED) && pq->count == 1) {
                                submit_now = true;
                            }
                        }

                        const char *ot = item->other_text;
                        while (*ot == ' ' || *ot == '\t') ++ot;
                        if (item->selected_mask != 0 || ot[0] != '\0') {
                            any_answered = true;
                        }
                    }
                    cui_pop_id(ui);
                }

                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    if (cui_button(ui, "Submit Answer", .variant = CUI_BUTTON_PRIMARY,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_CHECK,
                                   .disabled = !any_answered)) {
                        submit_now = true;
                    }
                    if (cui_button(ui, "Dismiss", .variant = CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_X)) {
                        dismiss_now = true;
                    }
                }
            }

            if (submit_now && any_answered) {
                mcode_agent_respond_question(thread->active_run, thread, true);
            } else if (dismiss_now) {
                mcode_agent_respond_question(thread->active_run, thread, false);
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
                    pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%lld in / %lld out",
                                    pos > 0 ? " · " : "",
                                    (long long)turn->input_tokens,
                                    (long long)turn->output_tokens);
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
    } k_modes[4] = {
        {
            MCODE_PERM_PLAN,
            CUI_ICON_COMPASS,
            "Plan",
            "Inspect read-only and propose a plan before editing.",
        },
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
    int cur_idx = 2;
    for (int i = 0; i < 4; ++i) {
        if (k_modes[i].mode == cur) { cur_idx = i; break; }
    }

    cui_vec2 mp = cui_mouse_pos(ui);
    cui_id pid = cui_make_id(ui, "perm-mode-btn");
    cui_rect pr = cui_get_rect(ui, pid);
    bool hovered = !busy && pr.w > 0.0f &&
                   mp.x >= pr.x && mp.x <= pr.x + pr.w &&
                   mp.y >= pr.y && mp.y <= pr.y + pr.h;
    bool open = cui_popover_is_open(ui, "perm-mode-pop");
    bool is_plan = (cur == MCODE_PERM_PLAN);

    cui_color bg = is_plan
        ? cui_alpha(cui_color_of(ui, CUI_COLOR_INFO), (open || hovered) ? 0.22f : 0.14f)
        : ((open || hovered)
            ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.08f)
            : CUI_RGBA(0x00000001u));
    cui_color fg = busy ? CUI_COLOR_TEXT_MUTED
                        : (is_plan ? CUI_COLOR_INFO : CUI_COLOR_TEXT_SECONDARY);

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
            for (int i = 0; i < 4; ++i) {
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

static void draw_composer_options_strip(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    bool show_missing = !app->cli_status.available[thread->agent];
    bool show_session = (thread->session_id[0] != '\0');
    if (!show_missing && !show_session) return;

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
    }
}

static bool composer_draft_is_blank(const char *s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        ++s;
    }
    return true;
}

static void format_single_line_preview(const char *src, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!src || !src[0]) return;
    size_t w = 0;
    bool prev_space = true;
    for (const char *p = src; *p && w + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space) {
                out[w++] = ' ';
                prev_space = true;
            }
        } else {
            out[w++] = (char)c;
            prev_space = false;
        }
    }
    while (w > 0 && out[w - 1] == ' ') --w;
    out[w] = '\0';
}

static bool draw_composer_action_pill(cui_ctx *ui, const char *id, const char *icon,
                                      const char *label, bool compact, bool primary,
                                      bool enabled, const char *tooltip) {
    cui_id cid = cui_make_id(ui, id);
    cui_rect r = cui_get_rect(ui, cid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = enabled && r.w > 0.0f &&
               mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    bool has_accent = app_theme_has_accent(ui);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color acc = has_accent ? cui_color_of(ui, CUI_COLOR_ACCENT) : fg;

    cui_color bg;
    cui_color bdr;
    cui_color txt_col;
    if (!enabled) {
        bg = CUI_RGBA(0x00000001u);
        bdr = cui_alpha(fg, dark ? 0.10f : 0.10f);
        txt_col = CUI_COLOR_TEXT_MUTED;
    } else if (primary) {
        bg = cui_alpha(acc, hov ? (dark ? 0.28f : 0.18f) : (dark ? 0.18f : 0.12f));
        bdr = cui_alpha(acc, dark ? 0.42f : 0.30f);
        txt_col = has_accent ? CUI_COLOR_ACCENT : CUI_COLOR_FOREGROUND;
    } else {
        bg = hov ? cui_alpha(fg, dark ? 0.11f : 0.08f) : cui_alpha(fg, dark ? 0.05f : 0.035f);
        bdr = cui_alpha(fg, hov ? (dark ? 0.22f : 0.18f) : (dark ? 0.13f : 0.12f));
        txt_col = hov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_SECONDARY;
    }

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = id,
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = compact ? CUI_PX(28) : CUI_FIT,
        .height = CUI_PX(28),
        .pad_x = compact ? 1 : 9,
        .pad_y = 1,
        .gap = compact ? 0 : 5,
        .hoverable = enabled,
        .border = 1.0f,
        .border_color = bdr,
        .radius = 8,
        .bg = bg,
    });
    if (compact) {
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
            if (icon && icon[0]) {
                cui_icon(ui, icon, 13.0f, txt_col);
            }
        }
    } else {
        if (icon && icon[0]) {
            cui_icon(ui, icon, 13.0f, txt_col);
        }
        if (label && label[0]) {
            cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 12.0f,
                     .color = txt_col, .nowrap = true);
        }
    }
    cui_card_end(ui);
    if (tooltip && tooltip[0]) {
        cui_tooltip(ui, tooltip);
    }
    return enabled && clicked;
}

static void draw_queued_item_row(cui_ctx *ui, const mcode_queued_msg *qm, int idx, bool busy,
                                 int *act_steer, int *act_up, int *act_edit, int *act_remove) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_push_id(ui, qm->id);

    cui_id rid = cui_make_id(ui, "qrow");
    cui_rect rr = cui_get_rect(ui, rid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = rr.w > 0.0f && mp.x >= rr.x && mp.x <= rr.x + rr.w &&
               mp.y >= rr.y && mp.y <= rr.y + rr.h;

    cui_color row_bg = hov
        ? cui_alpha(fg, dark ? 0.065f : 0.045f)
        : cui_alpha(fg, dark ? 0.03f : 0.02f);

    char num_lbl[16];
    snprintf(num_lbl, sizeof num_lbl, "%d", idx + 1);
    char preview[256];
    format_single_line_preview(mcode_str_cstr(&qm->text), preview, sizeof preview);

    cui_row(ui, .id = "qrow", .width = CUI_FILL, .pad_x = 8, .pad_y = 5, .gap = 6,
            .align = CUI_ALIGN_CENTER, .radius = 8, .bg = row_bg) {
        cui_box(ui, .width = CUI_PX(18), .height = CUI_PX(18), .radius = 5,
                .bg = cui_alpha(fg, dark ? 0.08f : 0.07f),
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, num_lbl, .font = CUI_FONT_MONO, .size = 10.5f,
                     .color = CUI_COLOR_TEXT_MUTED, .tabular = true, .nowrap = true);
        }
        cui_text(ui, preview[0] ? preview : "(empty)", .size = 12.5f,
                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL, .nowrap = true);
        cui_tooltip(ui, mcode_str_cstr(&qm->text));

        cui_row(ui, .gap = 2, .align = CUI_ALIGN_CENTER) {
            if (idx > 0) {
                if (cui_icon_button(ui, CUI_ICON_CHEVRON_UP,
                                    .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                    .width = CUI_PX(24),
                                    .tooltip = "Move up")) {
                    *act_up = idx;
                }
            }
            if (cui_icon_button(ui, busy ? CUI_ICON_ARROW_RIGHT : CUI_ICON_ARROW_UP,
                                .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .width = CUI_PX(24),
                                .tooltip = busy ? "Steer now" : "Send now")) {
                *act_steer = idx;
            }
            if (cui_icon_button(ui, CUI_ICON_PENCIL,
                                .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .width = CUI_PX(24),
                                .tooltip = "Edit in composer")) {
                *act_edit = idx;
            }
            if (cui_icon_button(ui, CUI_ICON_X,
                                .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                .width = CUI_PX(24),
                                .tooltip = "Remove from queue")) {
                *act_remove = idx;
            }
        }
    }

    cui_pop_id(ui);
}

static void draw_message_queue_dock(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                    bool busy, cui_id composer_fid) {
    bool has_steer = thread->steer_pending && thread->pending_steer.len > 0;
    bool has_queue = thread->queued_count > 0;
    if (!has_steer && !has_queue) return;

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color surface = cui_color_of(ui, CUI_COLOR_SURFACE);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color dock_bg = dark ? cui_mix(surface, fg, 0.03f) : cui_color_of(ui, CUI_COLOR_SURFACE_MUTED);
    cui_color dock_bdr = cui_color_of(ui, CUI_COLOR_BORDER);

    bool act_cancel_steer = false;
    bool act_send_next = false;
    bool act_clear = false;
    int act_steer = -1;
    int act_up = -1;
    int act_edit = -1;
    int act_remove = -1;

    cui_column(ui, .id = "msg-queue-dock", .width = CUI_FILL,
               .pad_x = 10, .pad_y = 8, .gap = 6,
               .radius = 12, .border = 1.0f, .border_color = dock_bdr, .bg = dock_bg) {
        if (has_steer) {
            char spreview[256];
            format_single_line_preview(mcode_str_cstr(&thread->pending_steer),
                                       spreview, sizeof spreview);
            cui_row(ui, .width = CUI_FILL, .gap = 7, .align = CUI_ALIGN_CENTER) {
                cui_spinner(ui, 12);
                cui_badge(ui, "Steering", .tone = CUI_TONE_ACCENT, .size = CUI_SIZE_SM);
                cui_text(ui, spreview, .size = 12.5f, .color = CUI_COLOR_FOREGROUND,
                         .width = CUI_FILL, .nowrap = true);
                cui_tooltip(ui, mcode_str_cstr(&thread->pending_steer));
                if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM,
                                    .variant = CUI_BUTTON_GHOST, .width = CUI_PX(24),
                                    .tooltip = "Cancel steer")) {
                    act_cancel_steer = true;
                }
            }
            if (has_queue) {
                cui_separator(ui);
            }
        }

        if (has_queue) {
            char hdr[64];
            if (!busy && thread->queue_paused) {
                snprintf(hdr, sizeof hdr, "Queued (%d) · Paused", thread->queued_count);
            } else {
                snprintf(hdr, sizeof hdr, "Queued (%d)", thread->queued_count);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_LIST, 12.5f, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, hdr, .font = CUI_FONT_MEDIUM, .size = 12.0f,
                         .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                cui_spacer(ui);
                if (!busy) {
                    if (cui_button(ui, "Send next", .variant = CUI_BUTTON_SECONDARY,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_PLAY)) {
                        act_send_next = true;
                    }
                }
                if (thread->queued_count > 1) {
                    if (cui_button(ui, "Clear", .variant = CUI_BUTTON_GHOST,
                                   .size = CUI_SIZE_SM)) {
                        act_clear = true;
                    }
                }
            }

            if (thread->queued_count > 3) {
                cui_scroll(ui, .id = "msg-queue-list", .width = CUI_FILL,
                           .height = CUI_PX(136), .gap = 4) {
                    for (int i = 0; i < thread->queued_count; ++i) {
                        draw_queued_item_row(ui, &thread->queued_msgs[i], i, busy,
                                             &act_steer, &act_up, &act_edit, &act_remove);
                    }
                }
            } else {
                cui_column(ui, .width = CUI_FILL, .gap = 4) {
                    for (int i = 0; i < thread->queued_count; ++i) {
                        draw_queued_item_row(ui, &thread->queued_msgs[i], i, busy,
                                             &act_steer, &act_up, &act_edit, &act_remove);
                    }
                }
            }
        }
    }

    if (act_cancel_steer) {
        mcode_ui_cancel_pending_steer(app, thread);
    }
    if (act_send_next) {
        mcode_ui_send_next_queued(app, ui, thread);
    } else if (act_clear) {
        mcode_thread_queue_clear(thread);
        thread->queue_paused = false;
        mcode_ui_mark_dirty(app);
    } else if (act_steer >= 0 && act_steer < thread->queued_count) {
        mcode_ui_steer_queued_item(app, ui, thread, act_steer);
    } else if (act_up > 0 && act_up < thread->queued_count) {
        mcode_thread_queue_move(thread, act_up, act_up - 1);
        mcode_ui_mark_dirty(app);
    } else if (act_edit >= 0 && act_edit < thread->queued_count) {
        char *pulled = strdup(mcode_str_cstr(&thread->queued_msgs[act_edit].text));
        if (pulled) {
            if (!composer_draft_is_blank(thread->composer_draft)) {
                /* Swap current draft into this queue slot so typed text is never lost. */
                mcode_str_set(&thread->queued_msgs[act_edit].text, thread->composer_draft);
            } else {
                mcode_thread_queue_remove(thread, act_edit);
                if (thread->queued_count == 0) thread->queue_paused = false;
            }
            snprintf(thread->composer_draft, sizeof thread->composer_draft, "%s", pulled);
            free(pulled);
            mcode_ui_mark_dirty(app);
            cui_set_focus(ui, composer_fid);
        }
    } else if (act_remove >= 0 && act_remove < thread->queued_count) {
        mcode_thread_queue_remove(thread, act_remove);
        if (thread->queued_count == 0) thread->queue_paused = false;
        mcode_ui_mark_dirty(app);
    }
}

static void append_attached_path_to_draft(mcode_thread *thread, const char *fpath) {
    if (!thread || !fpath || !fpath[0]) return;
    size_t cur_len = strlen(thread->composer_draft);
    const char *rel = fpath;
    size_t wd_len = strlen(thread->workdir);
    if (wd_len > 0 && strncmp(fpath, thread->workdir, wd_len) == 0 &&
        (fpath[wd_len] == '/' || fpath[wd_len] == '\\')) {
        rel = fpath + wd_len + 1;
    }
    bool has_space = (strchr(rel, ' ') != NULL || strchr(rel, '\t') != NULL);
    bool need_sep = (cur_len > 0 && !isspace((unsigned char)thread->composer_draft[cur_len - 1]));
    if (has_space) {
        snprintf(thread->composer_draft + cur_len,
                 sizeof thread->composer_draft - cur_len,
                 "%s@\"%s\" ", need_sep ? " " : "", rel);
    } else {
        snprintf(thread->composer_draft + cur_len,
                 sizeof thread->composer_draft - cur_len,
                 "%s@%s ", need_sep ? " " : "", rel);
    }
}

static bool composer_blocked_by_modal(const mcode_app *app) {
    if (!app) return false;
    return app->project_dialog_open ||
           app->rename_project_open ||
           app->new_thread_open ||
           app->git_dialog_open ||
           app->discard_dialog_open ||
           app->actions_dialog_open ||
           app->image_lightbox_open ||
           (app->chrome && (app->chrome->settings_open || app->chrome->palette_open));
}

static void draw_thread_composer(mcode_app *app, cui_ctx *ui, mcode_thread *thread, bool centered) {
    bool busy = (thread->active_run != NULL);
    const app_settings *settings = app_chrome_settings(app->chrome);
    bool steer_default = (settings && settings->mcode.busy_submit_mode == APP_BUSY_SUBMIT_STEER);
    bool draft_blank = composer_draft_is_blank(thread->composer_draft);

    cui_id composer_id = cui_make_id(ui, "thread-composer");
    cui_id composer_fid = composer_id ^ 0xF1E1Du;

    if (!centered) {
        draw_composer_options_strip(app, ui, thread);
    }

    draw_message_queue_dock(app, ui, thread, busy, composer_fid);
    /* Refresh blank/busy status in case queue dock actions mutated draft or started a turn */
    busy = (thread->active_run != NULL);
    draft_blank = composer_draft_is_blank(thread->composer_draft);

    chat_ref_image draft_imgs[6];
    int draft_img_count = extract_prompt_images(app, ui, thread->workdir,
                                                thread->composer_draft, draft_imgs, 6);
    if (draft_img_count > 0) {
        cui_row(ui, .width = CUI_FILL, .gap = 6, .wrap = true, .align = CUI_ALIGN_START) {
            for (int di = 0; di < draft_img_count; ++di) {
                char did[32];
                snprintf(did, sizeof did, "draft-img-%d", di);
                mcode_ui_draw_image_preview_card(app, ui, did,
                                                 draft_imgs[di].resolved,
                                                 draft_imgs[di].label,
                                                 96.0f, 64.0f, true);
            }
        }
    }

    bool want_queue = false;
    bool want_steer = false;
    if (cui_is_focused(ui, composer_fid)) {
        int mods = cui_key_mods(ui);
        if (busy && !draft_blank && cui_key_pressed(ui, CUI_KEY_ENTER) && !(mods & CUI_MOD_SHIFT)) {
            if (mods & (CUI_MOD_PRIMARY | CUI_MOD_ALT)) {
                cui_key_consume(ui, CUI_KEY_ENTER);
                if (steer_default) want_queue = true;
                else want_steer = true;
            } else if (!(mods & (CUI_MOD_CTRL | CUI_MOD_SUPER))) {
                cui_key_consume(ui, CUI_KEY_ENTER);
                if (steer_default) want_steer = true;
                else want_queue = true;
            }
        } else if (draft_blank && thread->queued_count > 0 &&
                   cui_key_pressed(ui, CUI_KEY_UP) && mods == 0) {
            cui_key_consume(ui, CUI_KEY_UP);
            int last = thread->queued_count - 1;
            snprintf(thread->composer_draft, sizeof thread->composer_draft, "%s",
                     mcode_str_cstr(&thread->queued_msgs[last].text));
            mcode_thread_queue_remove(thread, last);
            if (thread->queued_count == 0) thread->queue_paused = false;
            mcode_ui_mark_dirty(app);
            draft_blank = composer_draft_is_blank(thread->composer_draft);
        }
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
#ifdef __APPLE__
    const char *alt_key = "Cmd+Enter";
#else
    const char *alt_key = "Ctrl+Enter";
#endif
    char busy_placeholder[160];
    if (steer_default) {
        snprintf(busy_placeholder, sizeof busy_placeholder,
                 "Steer %s (Enter) or queue follow-up (%s)...",
                 mcode_agent_short_label(thread->agent), alt_key);
    } else {
        snprintf(busy_placeholder, sizeof busy_placeholder,
                 "Queue follow-up (Enter) or steer %s (%s)...",
                 mcode_agent_short_label(thread->agent), alt_key);
    }
    const char *placeholder = busy
        ? busy_placeholder
        : (thread->turn_count == 0
               ? k_first_placeholders[thread->agent]
               : k_followup_placeholders[thread->agent]);

    int ev = 0;
    float toolbar_left = 10.0f + cui_theme_get(ui)->control_sm + 6.0f;
    cui_placement pop_place = centered ? CUI_PLACE_BOTTOM_START : CUI_PLACE_TOP_START;
    float comp_w = cui_get_rect(ui, composer_id).w;
    bool compact_actions = (comp_w > 0.0f && comp_w < 500.0f);
    bool live_steer = busy && mcode_agent_supports_live_steer(thread->active_run);

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
            cui_spacer(ui);
            mcode_ui_draw_composer_usage_ring(
                app, ui, thread,
                centered ? CUI_PLACE_BOTTOM_END : CUI_PLACE_TOP_END);

            if (busy) {
                bool can_submit = !composer_draft_is_blank(thread->composer_draft);
                char qtip[128];
                snprintf(qtip, sizeof qtip, "Queue for next turn (%s)",
                         steer_default ? alt_key : "Enter");
                char stip[160];
                snprintf(stip, sizeof stip, "%s (%s)",
                         live_steer ? "Steer active turn live" : "Redirect active turn now",
                         steer_default ? "Enter" : alt_key);
                if (draw_composer_action_pill(ui, "cmp-queue-btn", CUI_ICON_LIST, "Queue",
                                              compact_actions, !steer_default, can_submit, qtip)) {
                    want_queue = true;
                }
                if (draw_composer_action_pill(ui, "cmp-steer-btn", CUI_ICON_ARROW_RIGHT, "Steer",
                                              compact_actions, steer_default, can_submit, stip)) {
                    want_steer = true;
                }
            }
        }
    }

    if (app->composer_want_focus && !composer_blocked_by_modal(app)) {
        cui_set_focus(ui, composer_fid);
        app->composer_want_focus = false;
    }

    if (centered) {
        draw_composer_options_strip(app, ui, thread);
    }

    if (ev & CUI_COMPOSER_ATTACH) {
        char fpath[MCODE_PATH_MAX] = "";
        if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                .kind = CUI_FILE_OPEN,
                .title = "Attach File or Image to Prompt",
            }, fpath, sizeof fpath)) {
            append_attached_path_to_draft(thread, fpath);
        }
    }
    if (want_steer && !composer_draft_is_blank(thread->composer_draft)) {
        char *copy = strdup(thread->composer_draft);
        thread->composer_draft[0] = '\0';
        if (copy) {
            mcode_ui_steer_message(app, ui, thread, copy);
            free(copy);
        }
    } else if (want_queue && !composer_draft_is_blank(thread->composer_draft)) {
        char *copy = strdup(thread->composer_draft);
        thread->composer_draft[0] = '\0';
        if (copy) {
            mcode_ui_queue_message(app, ui, thread, copy);
            free(copy);
        }
    } else if ((ev & CUI_COMPOSER_SEND) && !composer_draft_is_blank(thread->composer_draft)) {
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

#define MCODE_CHAT_SCROLL_PAD 16.0f
#define MCODE_CHAT_COL_MAX_W  860.0f

void mcode_ui_keep_thread_scrolls_alive(mcode_app *app, cui_ctx *ui) {
    if (!app || !ui) return;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (!t->scroll_ui_ready || !t->id[0]) continue;
        cui_push_id(ui, "thread-transcript");
        cui_push_id(ui, t->id);
        cui_id scroll_id = cui_make_id(ui, "chat-transcript");
        cui_id content_id = cui_make_id(ui, "chat-transcript-content");
        (void)cui_state(ui, scroll_id, 0);
        (void)cui_state(ui, content_id, 0);
        cui_pop_id(ui);
        cui_pop_id(ui);
    }
}

void mcode_ui_draw_chat(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!thread) {
        app->last_chat_scroll_thread_id[0] = '\0';
        draw_empty_workspace(app, ui);
        return;
    }

    /* Accept drag-and-dropped files/images onto the chat view directly into the composer. */
    if (!composer_blocked_by_modal(app)) {
        int ndrop = cui_dropped_file_count(ui);
        if (ndrop > 0) {
            for (int di = 0; di < ndrop; ++di) {
                const char *dp = cui_dropped_file(ui, di);
                if (dp && dp[0]) {
                    append_attached_path_to_draft(thread, dp);
                }
            }
            cui_dropped_files_claim(ui);
            app->composer_want_focus = true;
        }
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        if (thread->turn_count == 0) {
            app->last_chat_scroll_thread_id[0] = '\0';
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
            cui_push_id(ui, "thread-transcript");
            cui_push_id(ui, thread->id);

            cui_id scroll_id = cui_make_id(ui, "chat-transcript");
            cui_id content_id = cui_make_id(ui, "chat-transcript-content");
            cui_rect view_r = cui_get_rect(ui, scroll_id);
            cui_rect content_r = cui_get_rect(ui, content_id);
            bool has_layout = (view_r.h > 0.0f && content_r.h > 0.0f);

            uint64_t cur_frame = cui_frame_count(ui);
            bool reentering = (strcmp(app->last_chat_scroll_thread_id, thread->id) != 0 ||
                               app->last_chat_scroll_frame + 1 != cur_frame);
            if (view_r.w > 2.0f * MCODE_CHAT_SCROLL_PAD) {
                if (app->last_chat_view_w > 0.0f &&
                    fabsf(view_r.w - app->last_chat_view_w) > 1.0f) {
                    cui_request_redraw(ui);
                }
                app->last_chat_view_w = view_r.w;
            }
            float ref_view_w = (view_r.w > 2.0f * MCODE_CHAT_SCROLL_PAD)
                ? view_r.w
                : app->last_chat_view_w;
            float fallback_md_w = 0.0f;
            if (ref_view_w > 2.0f * MCODE_CHAT_SCROLL_PAD) {
                fallback_md_w = ref_view_w - 2.0f * MCODE_CHAT_SCROLL_PAD;
                if (fallback_md_w > MCODE_CHAT_COL_MAX_W) fallback_md_w = MCODE_CHAT_COL_MAX_W;
                if (fallback_md_w < 120.0f) fallback_md_w = 0.0f;
            }

            /* Detect manual wheel or scrollbar interaction before cui_scroll consumes input */
            cui_vec2 mp = cui_mouse_pos(ui);
            cui_vec2 wheel = cui_scroll_delta(ui);
            bool mouse_in_view = has_layout &&
                mp.x >= view_r.x && mp.x <= view_r.x + view_r.w &&
                mp.y >= view_r.y && mp.y <= view_r.y + view_r.h;
            bool mouse_on_bar = mouse_in_view &&
                mp.x >= view_r.x + view_r.w - 14.0f &&
                (cui_mouse_clicked(ui, CUI_MOUSE_LEFT) || cui_mouse_down(ui, CUI_MOUSE_LEFT));
            bool user_scrolled = !reentering &&
                ((mouse_in_view && wheel.y != 0.0f) || mouse_on_bar);
            if (user_scrolled) {
                thread->scroll_restore_pending = false;
                thread->scroll_restore_frames = 0;
                thread->scroll_restore_settled = 0;
            }

            /* Restore persisted scroll state on first render of a thread (e.g. after app launch),
             * or re-pin to bottom if the viewport resized while viewing a file or another thread. */
            if (!user_scrolled && thread->scroll_to_bottom_frames == 0 && thread->has_scroll_y) {
                bool need_init_restore = (!thread->scroll_ui_ready || !has_layout) &&
                    (thread->scroll_at_bottom || thread->scroll_y > 0.5f);
                bool need_bottom_resync = reentering && thread->scroll_at_bottom;
                if (need_init_restore || need_bottom_resync) {
                    thread->scroll_restore_pending = true;
                    thread->scroll_restore_frames = 180;
                    thread->scroll_restore_settled = 0;
                }
            }

            /* Scrollable transcript */
            cui_scroll(ui, .id = "chat-transcript", .width = CUI_FILL, .height = CUI_FILL,
                       .pad = MCODE_CHAT_SCROLL_PAD, .gap = 16, .fade_edges = true) {
                cui_row(ui, .id = "chat-transcript-content", .width = CUI_FILL,
                        .justify = CUI_JUSTIFY_CENTER) {
                    cui_column(ui, .width = CUI_FILL, .max_width = MCODE_CHAT_COL_MAX_W, .gap = 18) {
                        for (int i = 0; i < thread->turn_count; ++i) {
                            draw_turn(app, ui, thread, i, fallback_md_w);
                        }
                    }
                }
            }
            thread->scroll_ui_ready = true;

            if (thread->scroll_to_bottom_frames > 0) {
                thread->scroll_restore_pending = false;
                thread->scroll_restore_frames = 0;
                thread->scroll_restore_settled = 0;
                thread->has_scroll_y = true;
                thread->scroll_at_bottom = true;
                cui_scroll_to(ui, "chat-transcript", 1e9f);
                thread->scroll_to_bottom_frames--;
                if (thread->scroll_to_bottom_frames > 0) {
                    cui_request_redraw(ui);
                } else {
                    thread->scroll_restore_pending = true;
                    thread->scroll_restore_frames = 180;
                    thread->scroll_restore_settled = 0;
                    cui_request_redraw(ui);
                }
            } else if (thread->scroll_restore_pending) {
                float target = thread->scroll_at_bottom ? 1e9f : thread->scroll_y;
                if (target < 0.0f) target = 0.0f;
                cui_scroll_to(ui, "chat-transcript", target);
                if (has_layout) {
                    float measured_y = (view_r.y + MCODE_CHAT_SCROLL_PAD) - content_r.y;
                    if (measured_y < 0.0f) measured_y = 0.0f;
                    float max_y = (content_r.h + 2.0f * MCODE_CHAT_SCROLL_PAD) - view_r.h;
                    if (max_y < 0.0f) max_y = 0.0f;
                    float clamped_target = thread->scroll_at_bottom
                        ? max_y
                        : (target > max_y ? max_y : target);
                    thread->scroll_restore_settled++;
                    if (thread->scroll_restore_settled >= 2 &&
                        fabsf(measured_y - clamped_target) <= 1.5f) {
                        bool at_bottom = (max_y > 0.5f && clamped_target >= max_y - 2.0f);
                        bool changed = (fabsf(clamped_target - thread->scroll_y) >= 1.0f ||
                                        at_bottom != thread->scroll_at_bottom);
                        thread->scroll_y = clamped_target;
                        thread->scroll_at_bottom = at_bottom;
                        thread->has_scroll_y = true;
                        thread->scroll_restore_pending = false;
                        thread->scroll_restore_frames = 0;
                        thread->scroll_restore_settled = 0;
                        if (changed && !thread->is_draft) {
                            mcode_ui_mark_dirty(app);
                        }
                    } else if (--thread->scroll_restore_frames <= 0) {
                        thread->scroll_restore_pending = false;
                        thread->scroll_restore_frames = 0;
                        thread->scroll_restore_settled = 0;
                    } else {
                        cui_request_redraw(ui);
                    }
                } else {
                    thread->scroll_restore_settled = 0;
                    if (--thread->scroll_restore_frames <= 0) {
                        thread->scroll_restore_pending = false;
                        thread->scroll_restore_frames = 0;
                    } else {
                        cui_request_redraw(ui);
                    }
                }
            } else if (has_layout) {
                float measured_y = (view_r.y + MCODE_CHAT_SCROLL_PAD) - content_r.y;
                if (measured_y < 0.0f) measured_y = 0.0f;
                float max_y = (content_r.h + 2.0f * MCODE_CHAT_SCROLL_PAD) - view_r.h;
                if (max_y < 0.0f) max_y = 0.0f;
                if (measured_y > max_y) measured_y = max_y;
                bool at_bottom = (max_y > 0.5f && measured_y >= max_y - 2.0f);
                if (user_scrolled && wheel.y > 0.0f) {
                    at_bottom = false;
                }
                bool changed = (!thread->has_scroll_y ||
                                fabsf(measured_y - thread->scroll_y) >= 1.0f ||
                                at_bottom != thread->scroll_at_bottom);
                thread->scroll_y = measured_y;
                thread->scroll_at_bottom = at_bottom;
                thread->has_scroll_y = true;
                if (changed && !thread->is_draft) {
                    mcode_ui_mark_dirty(app);
                }
            }

            snprintf(app->last_chat_scroll_thread_id, sizeof app->last_chat_scroll_thread_id,
                     "%s", thread->id);
            app->last_chat_scroll_frame = cur_frame;

            cui_pop_id(ui);
            cui_pop_id(ui);

            /* Composer dock */
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad_x = 16, .pad_y = 12) {
                cui_column(ui, .width = CUI_FILL, .max_width = 860, .gap = 8) {
                    draw_thread_composer(app, ui, thread, false);
                }
            }
        }
    }
}
