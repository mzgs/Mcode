#include "mcode_ui_internal.h"
#include "mcode_files.h"
#include "mcode_git.h"
#include "platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool rect_contains(cui_rect r, cui_vec2 p) {
    return r.w > 0.0f && r.h > 0.0f &&
           p.x >= r.x && p.x <= r.x + r.w &&
           p.y >= r.y && p.y <= r.y + r.h;
}

static cui_color right_sidebar_bg(cui_ctx *ui) {
    return (cui_get_theme_mode(ui) == CUI_THEME_DARK)
        ? CUI_RGBA(0x000000ffu)
        : cui_color_of(ui, CUI_COLOR_SURFACE_MUTED);
}

static cui_color right_hover_bg(cui_ctx *ui, float strength) {
    cui_color base = right_sidebar_bg(ui);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    return cui_mix(base, fg, strength);
}

static const char *path_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void split_path_dir_base(const char *path, char *dir_out, size_t dir_cap,
                                const char **base_out) {
    if (dir_out && dir_cap) dir_out[0] = '\0';
    if (base_out) *base_out = path ? path : "";
    if (!path) return;
    const char *slash = strrchr(path, '/');
    if (slash) {
        if (base_out) *base_out = slash + 1;
        if (dir_out && dir_cap > 0) {
            size_t n = (size_t)(slash - path);
            if (n >= dir_cap) n = dir_cap - 1;
            memcpy(dir_out, path, n);
            dir_out[n] = '\0';
        }
    }
}

static const char *display_git_status_letter(const char *st, bool untracked) {
    if (untracked || (st && st[0] == '?')) return "U";
    if (!st || !st[0]) return "M";
    return st;
}

static cui_color color_for_git_status_char(cui_ctx *ui, char c) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    switch (c) {
    case 'M': return dark ? CUI_RGBA(0x5b9cf6ffu) : CUI_RGBA(0x2563ebffu);
    case 'A':
    case 'U':
    case '?': return dark ? CUI_RGBA(0x4ec970ffu) : CUI_RGBA(0x16a34affu);
    case 'D': return dark ? CUI_RGBA(0xf16c6cffu) : CUI_RGBA(0xdc2626ffu);
    case 'R': return dark ? CUI_RGBA(0x38bdf8ffu) : CUI_RGBA(0x0284c7ffu);
    default:  return cui_color_of(ui, CUI_COLOR_TEXT_MUTED);
    }
}

static bool ext_eq(const char *ext, const char *want) {
    if (!ext || !want) return false;
    while (*ext && *want) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*want)) return false;
        ++ext;
        ++want;
    }
    return *ext == '\0' && *want == '\0';
}

/* Crisp VS Code-inspired file type icon using pure CUI primitives. */
static void draw_file_type_icon(cui_ctx *ui, const char *filename, bool muted) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    const char *base = path_basename(filename);
    const char *ext = strrchr(base, '.');
    if (ext && ext[1]) ext++;
    else ext = NULL;

    const char *glyph = NULL;
    const char *icon = CUI_ICON_FILE_TEXT;
    cui_color col = cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);
    float glyph_size = 11.5f;

    if (strcmp(base, "CMakeLists.txt") == 0 || ext_eq(ext, "cmake")) {
        icon = CUI_ICON_LAYERS;
        col = dark ? CUI_RGBA(0x4ea8f5ffu) : CUI_RGBA(0x2563ebffu);
    } else if (strcmp(base, "Makefile") == 0 || ext_eq(ext, "mk")) {
        icon = CUI_ICON_SETTINGS;
        col = dark ? CUI_RGBA(0xe5a54bffu) : CUI_RGBA(0xb45309ffu);
    } else if (ext_eq(ext, "c")) {
        glyph = "C";
        col = dark ? CUI_RGBA(0x3b9effffu) : CUI_RGBA(0x1d6fd6ffu);
    } else if (ext_eq(ext, "h") || ext_eq(ext, "hpp") || ext_eq(ext, "hh")) {
        glyph = "h";
        col = dark ? CUI_RGBA(0x6ea8feffu) : CUI_RGBA(0x4f46e5ffu);
    } else if (ext_eq(ext, "cpp") || ext_eq(ext, "cc") || ext_eq(ext, "cxx") || ext_eq(ext, "mm")) {
        glyph = "C+";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0x3b9effffu) : CUI_RGBA(0x1d6fd6ffu);
    } else if (ext_eq(ext, "m")) {
        glyph = "m";
        col = dark ? CUI_RGBA(0x38bdf8ffu) : CUI_RGBA(0x0284c7ffu);
    } else if (ext_eq(ext, "js") || ext_eq(ext, "mjs") || ext_eq(ext, "cjs") || ext_eq(ext, "jsx")) {
        glyph = "JS";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0xf5d042ffu) : CUI_RGBA(0xca8a04ffu);
    } else if (ext_eq(ext, "ts") || ext_eq(ext, "tsx") || ext_eq(ext, "mts")) {
        glyph = "TS";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0x4b9ceeffu) : CUI_RGBA(0x2563ebffu);
    } else if (ext_eq(ext, "py")) {
        glyph = "PY";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0x52c472ffu) : CUI_RGBA(0x16a34affu);
    } else if (ext_eq(ext, "rs")) {
        glyph = "RS";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0xef8555ffu) : CUI_RGBA(0xc2410cffu);
    } else if (ext_eq(ext, "go")) {
        glyph = "GO";
        glyph_size = 9.5f;
        col = dark ? CUI_RGBA(0x38bdf8ffu) : CUI_RGBA(0x0284c7ffu);
    } else if (ext_eq(ext, "json")) {
        glyph = "{}";
        glyph_size = 10.0f;
        col = dark ? CUI_RGBA(0xeab308ffu) : CUI_RGBA(0xa16207ffu);
    } else if (ext_eq(ext, "md") || ext_eq(ext, "markdown")) {
        icon = CUI_ICON_FILE_TEXT;
        col = dark ? CUI_RGBA(0x60a5faffu) : CUI_RGBA(0x2563ebffu);
    } else if (ext_eq(ext, "sh") || ext_eq(ext, "zsh") || ext_eq(ext, "bash")) {
        icon = CUI_ICON_SQUARE_TERMINAL;
        col = dark ? CUI_RGBA(0x4ade80ffu) : CUI_RGBA(0x16a34affu);
    } else if (ext_eq(ext, "png") || ext_eq(ext, "jpg") || ext_eq(ext, "jpeg") ||
               ext_eq(ext, "gif") || ext_eq(ext, "svg") || ext_eq(ext, "icns") ||
               ext_eq(ext, "ico") || ext_eq(ext, "webp")) {
        icon = CUI_ICON_IMAGE;
        col = dark ? CUI_RGBA(0xc084fcffu) : CUI_RGBA(0x9333eaffu);
    } else if (ext_eq(ext, "a") || ext_eq(ext, "o") || ext_eq(ext, "so") ||
               ext_eq(ext, "dylib") || ext_eq(ext, "dll") || ext_eq(ext, "lib")) {
        icon = CUI_ICON_PACKAGE;
        col = dark ? CUI_RGBA(0x84cc16ffu) : CUI_RGBA(0x4d7c0fffu);
    } else if (ext_eq(ext, "yaml") || ext_eq(ext, "yml") || ext_eq(ext, "toml") ||
               ext_eq(ext, "plist") || ext_eq(ext, "xml") || ext_eq(ext, "in") ||
               ext_eq(ext, "rc") || ext_eq(ext, "desktop")) {
        icon = CUI_ICON_SETTINGS;
        col = dark ? CUI_RGBA(0xa78bfaffu) : CUI_RGBA(0x7c3aedffu);
    } else if (ext_eq(ext, "html") || ext_eq(ext, "htm") || ext_eq(ext, "css")) {
        icon = CUI_ICON_CODE;
        col = dark ? CUI_RGBA(0xfb923cffu) : CUI_RGBA(0xea580cffu);
    } else if (ext_eq(ext, "txt") || ext_eq(ext, "log")) {
        icon = CUI_ICON_FILE_TEXT;
        col = cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);
    } else if (base[0] == '.') {
        icon = CUI_ICON_SETTINGS;
        col = cui_color_of(ui, CUI_COLOR_TEXT_MUTED);
    } else {
        icon = CUI_ICON_FILE;
        col = cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);
    }

    if (muted) {
        col = cui_alpha(col, dark ? 0.58f : 0.62f);
    }

    cui_box(ui, .width = CUI_PX(16), .height = CUI_PX(16),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (glyph) {
            cui_text(ui, glyph, .font = CUI_FONT_MONO, .size = glyph_size,
                     .color = col, .nowrap = true);
        } else {
            cui_icon(ui, icon, 13.5f, col);
        }
    }
}

static cui_color folder_icon_color(cui_ctx *ui, const char *name, bool git_ignored) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color col;
    if (strcmp(name, "build") == 0 || strcmp(name, "dist") == 0 || strcmp(name, "out") == 0) {
        col = dark ? CUI_RGBA(0xe8a8a8ffu) : CUI_RGBA(0xb45353ffu);
    } else if (strcmp(name, "src") == 0) {
        col = dark ? CUI_RGBA(0x60a5faffu) : CUI_RGBA(0x2563ebffu);
    } else if (strcmp(name, "tests") == 0 || strcmp(name, "Testing") == 0 || strcmp(name, "test") == 0) {
        col = dark ? CUI_RGBA(0x34d399ffu) : CUI_RGBA(0x059669ffu);
    } else if (strcmp(name, "generated") == 0) {
        col = dark ? CUI_RGBA(0xf87171ffu) : CUI_RGBA(0xdc2626ffu);
    } else if (strcmp(name, "resources") == 0 || strcmp(name, "assets") == 0) {
        col = dark ? CUI_RGBA(0xfbbf24ffu) : CUI_RGBA(0xd97706ffu);
    } else if (strcmp(name, "scripts") == 0) {
        col = dark ? CUI_RGBA(0xa78bfaffu) : CUI_RGBA(0x7c3aedffu);
    } else {
        col = dark ? CUI_RGBA(0x8e99a8ffu) : CUI_RGBA(0x64748bffu);
    }
    if (git_ignored) {
        col = cui_alpha(col, dark ? 0.60f : 0.58f);
    }
    return col;
}

static void count_git_groups(const mcode_app *app, int *out_staged, int *out_unstaged) {
    int staged = 0, unstaged = 0;
    if (app) {
        for (int i = 0; i < app->git_file_count; ++i) {
            if (app->git_files[i].staged) ++staged;
            else ++unstaged;
        }
    }
    if (out_staged) *out_staged = staged;
    if (out_unstaged) *out_unstaged = unstaged;
}

static bool draw_commit_menu_item(cui_ctx *ui, const char *id, const char *icon,
                                  const char *label, bool disabled) {
    cui_id cid = cui_make_id(ui, id);
    bool hov = !disabled && rect_contains(cui_get_rect(ui, cid), cui_mouse_pos(ui));
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color bg = hov ? cui_alpha(fg, 0.08f) : CUI_RGBA(0x00000001u);
    cui_color txt = disabled ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_FOREGROUND;
    cui_color ic = disabled ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_TEXT_SECONDARY;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = id,
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_FILL,
        .pad_x = 9,
        .pad_y = 6,
        .gap = 8,
        .hoverable = !disabled,
        .border = -1.0f,
        .radius = 6.0f,
        .bg = bg,
    });
    if (icon && icon[0]) {
        cui_icon(ui, icon, 13.5f, ic);
    }
    cui_text(ui, label, .size = 12.5f, .color = txt, .width = CUI_FILL, .nowrap = true);
    cui_card_end(ui);
    return clicked && !disabled;
}

static float context_menu_width(void) {
#if defined(__APPLE__) || defined(_WIN32)
    return 168.0f;
#else
    return 184.0f;
#endif
}

static bool draw_context_menu_item(cui_ctx *ui, const char *id, const char *icon,
                                   const char *label, bool disabled) {
    cui_id cid = cui_make_id(ui, id);
    bool hov = !disabled && rect_contains(cui_get_rect(ui, cid), cui_mouse_pos(ui));
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color bg = hov ? cui_alpha(fg, 0.08f) : CUI_RGBA(0x00000001u);
    cui_color txt = disabled ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_FOREGROUND;
    cui_color ic = disabled ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_TEXT_SECONDARY;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = id,
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_FILL,
        .pad_x = 8,
        .pad_y = 5,
        .gap = 7,
        .hoverable = !disabled,
        .border = -1.0f,
        .radius = 5.0f,
        .bg = bg,
    });
    if (icon && icon[0]) {
        cui_icon(ui, icon, 13.0f, ic);
    }
    cui_text(ui, label, .size = 12.0f, .color = txt, .width = CUI_FILL, .nowrap = true);
    cui_card_end(ui);
    return clicked && !disabled;
}

static void draw_context_menu_sep(cui_ctx *ui) {
    cui_box(ui, .width = CUI_FILL, .pad_x = 4, .pad_y = 2) {
        cui_separator(ui);
    }
}

static void reveal_in_file_manager(mcode_app *app, cui_ctx *ui, const char *full_path) {
    if (!full_path || !full_path[0]) return;
    if (!app_reveal_in_file_manager(full_path) && ui) {
        app_chrome_notify(app->chrome, ui, "Could not show item in file manager.", CUI_TONE_DANGER);
    }
}

static void open_file_context_menu(mcode_app *app, cui_ctx *ui, mcode_file_ctx_kind kind,
                                   int index, bool is_dir, bool is_expanded,
                                   bool is_staged, bool is_deleted,
                                   const char *workdir, const char *rel_path) {
    if (!app || !ui) return;
    cui_vec2 mp = cui_mouse_pos(ui);
    app->file_ctx_open = true;
    app->file_ctx_opened_this_frame = true;
    app->file_ctx_kind = kind;
    app->file_ctx_x = mp.x;
    app->file_ctx_y = mp.y;
    app->file_ctx_index = index;
    app->file_ctx_is_dir = is_dir;
    app->file_ctx_is_expanded = is_expanded;
    app->file_ctx_is_staged = is_staged;
    app->file_ctx_is_deleted = is_deleted;
    snprintf(app->file_ctx_workdir, sizeof app->file_ctx_workdir, "%s",
             workdir ? workdir : "");
    snprintf(app->file_ctx_rel_path, sizeof app->file_ctx_rel_path, "%s",
             rel_path ? rel_path : "");
}

static void promote_project_for_workdir(mcode_app *app, const char *workdir) {
    if (!app) return;
    if (workdir && workdir[0]) {
        mcode_project *p = mcode_store_find_project_by_path(&app->store, workdir);
        if (p) {
            char pid[MCODE_ID_MAX];
            snprintf(pid, sizeof pid, "%s", p->id);
            mcode_ui_promote_project(app, pid);
            return;
        }
    }
    mcode_thread *t = mcode_store_selected_thread(&app->store);
    if (t && t->project_id[0]) {
        mcode_ui_promote_project(app, t->project_id);
    } else if (app->store.selected_project_id[0]) {
        mcode_ui_promote_project(app, app->store.selected_project_id);
    }
}

static void perform_sidebar_commit(mcode_app *app, cui_ctx *ui, const char *workdir,
                                   bool also_push) {
    if (!app || !workdir || !workdir[0]) return;
    if (!app->sidebar_commit_msg[0]) {
        if (ui) app_chrome_notify(app->chrome, ui, "Enter a commit message first.", CUI_TONE_WARNING);
        return;
    }

    int staged_count = 0, unstaged_count = 0;
    count_git_groups(app, &staged_count, &unstaged_count);
    if (staged_count == 0 && unstaged_count == 0) {
        if (ui) app_chrome_notify(app->chrome, ui, "No changes to commit.", CUI_TONE_INFO);
        return;
    }

    char err[256] = "";
    bool ok = (staged_count > 0)
        ? mcode_git_commit_staged(workdir, app->sidebar_commit_msg, err, sizeof err)
        : mcode_git_commit_all(workdir, app->sidebar_commit_msg, err, sizeof err);
    if (ok && also_push) {
        ok = mcode_git_push(workdir, err, sizeof err);
    }
    if (ok) {
        app->sidebar_commit_msg[0] = '\0';
        promote_project_for_workdir(app, workdir);
        mcode_ui_refresh_workspace_git(app);
        if (app->inspector_open && app->inspector_mode == MCODE_INSPECTOR_DIFF &&
            app->git_file_count == 0) {
            mcode_ui_close_inspector(app);
        }
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              also_push ? "Committed and pushed." : "Changes committed.",
                              CUI_TONE_SUCCESS);
        }
    } else {
        if (ui) {
            app_chrome_notify(app->chrome, ui,
                              err[0] ? err : (also_push ? "Commit & push failed." : "Commit failed."),
                              CUI_TONE_DANGER);
        }
    }
}

static void draw_diff_line_stats(cui_ctx *ui, int added, int removed) {
    char add_buf[24];
    char rem_buf[24];
    snprintf(add_buf, sizeof add_buf, "+%d", added);
    snprintf(rem_buf, sizeof rem_buf, "-%d", removed);
    cui_row(ui, .gap = 4, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, add_buf, .font = CUI_FONT_MONO, .size = 11.0f,
                 .color = color_for_git_status_char(ui, 'A'), .tabular = true, .nowrap = true);
        cui_text(ui, rem_buf, .font = CUI_FONT_MONO, .size = 11.0f,
                 .color = color_for_git_status_char(ui, 'D'), .tabular = true, .nowrap = true);
    }
}

static void draw_right_sidebar_top_header(mcode_app *app, cui_ctx *ui) {
    cui_row(ui, .width = CUI_FILL, .pad_left = 12, .pad_right = 8,
            .pad_top = 6, .pad_bottom = 0, .gap = 6, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, "Changes", .font = CUI_FONT_MEDIUM, .size = 13.0f,
                 .color = CUI_COLOR_FOREGROUND, .nowrap = true);
        if (app->git_file_count > 0 &&
            (app->git_total_added > 0 || app->git_total_removed > 0)) {
            draw_diff_line_stats(ui, app->git_total_added, app->git_total_removed);
        }
        cui_spacer(ui);

        if (cui_icon_button(ui, CUI_ICON_REFRESH_CW,
                            .variant = CUI_BUTTON_GHOST,
                            .size = CUI_SIZE_SM,
                            .width = CUI_PX(26),
                            .color = CUI_COLOR_TEXT_SECONDARY,
                            .tooltip = "Refresh Changes & Files",
                            .shortcut = "Cmd R")) {
            mcode_ui_refresh_workspace_git(app);
        }
    }
}

static void draw_source_control_commit_box(mcode_app *app, cui_ctx *ui,
                                           mcode_thread *thread, mcode_project *proj,
                                           const char *workdir) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    const char *branch = (thread && thread->branch[0]) ? thread->branch
                       : (proj && proj->default_branch[0]) ? proj->default_branch
                       : "HEAD";
    int staged_count = 0, unstaged_count = 0;
    count_git_groups(app, &staged_count, &unstaged_count);
    int change_count = app->git_file_count;

    static char placeholder[160];
#ifdef __APPLE__
    snprintf(placeholder, sizeof placeholder, "Message (Cmd+Enter to commit on \"%s\")", branch);
#else
    snprintf(placeholder, sizeof placeholder, "Message (Ctrl+Enter to commit on \"%s\")", branch);
#endif

    cui_column(ui, .width = CUI_FILL, .pad_left = 10, .pad_right = 10,
               .pad_top = 2, .pad_bottom = 6, .gap = 7) {
        cui_color input_bg = dark ? CUI_RGBA(0x0e0e11ffu) : cui_color_of(ui, CUI_COLOR_SURFACE);
        cui_color input_bdr = dark ? CUI_RGBA(0x24242cffu) : cui_color_of(ui, CUI_COLOR_BORDER);

        int flags = cui_input(ui, "sc-commit-msg", app->sidebar_commit_msg,
                              sizeof app->sidebar_commit_msg,
                              .placeholder = placeholder,
                              .size = CUI_SIZE_SM,
                              .width = CUI_FILL,
                              .bg = input_bg,
                              .border_color = input_bdr,
                              .radius = 6.0f);

        bool has_changes = (change_count > 0);
        bool can_commit = (app->sidebar_commit_msg[0] != '\0' && has_changes);
        if (can_commit && ((flags & CUI_SUBMITTED) ||
                           ((flags & CUI_FOCUSED) &&
                            cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_ENTER)))) {
            perform_sidebar_commit(app, ui, workdir, false);
        }

        /* Full-width Commit split button (matching VS Code) */
        cui_color active_bg = (cui_get_accent(ui) == CUI_ACCENT_NEUTRAL)
            ? (dark ? CUI_RGBA(0x3a63b8ffu) : CUI_RGBA(0x2563ebffu))
            : cui_color_of(ui, CUI_COLOR_ACCENT);
        cui_color accent_bg = has_changes
            ? active_bg
            : (dark ? CUI_RGBA(0x1f335cffu) : CUI_RGBA(0x93b4f5ffu));
        cui_color btn_fg = has_changes
            ? CUI_RGBA(0xffffffffu)
            : cui_alpha(CUI_RGBA(0xffffffffu), 0.72f);

        cui_row(ui, .width = CUI_FILL, .gap = 0, .radius = 6.0f, .clip = true,
                .align = CUI_ALIGN_STRETCH) {
            if (cui_button(ui, "Commit",
                           .variant = CUI_BUTTON_PRIMARY,
                           .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_CHECK,
                           .disabled = !has_changes,
                           .full_width = true,
                           .bg = accent_bg,
                           .color = btn_fg,
                           .radius = -1.0f)) {
                perform_sidebar_commit(app, ui, workdir, false);
            }

            cui_box(ui, .width = CUI_PX(1), .height = CUI_FILL,
                    .bg = cui_alpha(CUI_RGBA(0xffffffffu), 0.22f)) {}

            if (cui_icon_button(ui, CUI_ICON_CHEVRON_DOWN,
                                .variant = CUI_BUTTON_PRIMARY,
                                .size = CUI_SIZE_SM,
                                .width = CUI_PX(28),
                                .bg = accent_bg,
                                .color = btn_fg,
                                .radius = -1.0f,
                                .tooltip = "More Commit Actions")) {
                cui_popover_toggle(ui, "sc-commit-more-pop");
            }

            bool close_pop = false;
            cui_popover(ui, "sc-commit-more-pop", .placement = CUI_PLACE_BOTTOM_END,
                        .width = 216, .pad = 5, .gap = 2) {
                cui_column(ui, .width = CUI_FILL, .gap = 2) {
                    if (draw_commit_menu_item(ui, "sc-act-cpush", CUI_ICON_UPLOAD,
                                              "Commit & Push", !can_commit)) {
                        perform_sidebar_commit(app, ui, workdir, true);
                        close_pop = true;
                    }
                    if (draw_commit_menu_item(ui, "sc-act-push", CUI_ICON_ARROW_UP_RIGHT,
                                              "Push", false)) {
                        char err[256] = "";
                        if (mcode_git_push(workdir, err, sizeof err)) {
                            app_chrome_notify(app->chrome, ui, "Branch pushed.", CUI_TONE_SUCCESS);
                        } else {
                            app_chrome_notify(app->chrome, ui,
                                              err[0] ? err : "Push failed.", CUI_TONE_DANGER);
                        }
                        close_pop = true;
                    }
                    if (draw_commit_menu_item(ui, "sc-act-pr", CUI_ICON_GIT_BRANCH,
                                              "Push & Create PR...", !thread)) {
                        mcode_app_action_create_pr(ui, app);
                        close_pop = true;
                    }
                    cui_separator(ui);
                    if (draw_commit_menu_item(ui, "sc-act-stage-all", CUI_ICON_PLUS,
                                              "Stage All Changes", unstaged_count == 0)) {
                        char err[256] = "";
                        if (mcode_git_stage_all(workdir, err, sizeof err)) {
                            promote_project_for_workdir(app, workdir);
                            mcode_ui_refresh_workspace_git(app);
                        } else if (err[0]) {
                            app_chrome_notify(app->chrome, ui, err, CUI_TONE_DANGER);
                        }
                        close_pop = true;
                    }
                    if (draw_commit_menu_item(ui, "sc-act-unstage-all", CUI_ICON_MINUS,
                                              "Unstage All Changes", staged_count == 0)) {
                        char err[256] = "";
                        if (mcode_git_unstage_all(workdir, err, sizeof err)) {
                            promote_project_for_workdir(app, workdir);
                            mcode_ui_refresh_workspace_git(app);
                        } else if (err[0]) {
                            app_chrome_notify(app->chrome, ui, err, CUI_TONE_DANGER);
                        }
                        close_pop = true;
                    }
                    if (draw_commit_menu_item(ui, "sc-act-dlg", CUI_ICON_SQUARE_PEN,
                                              "Commit Dialog...", !thread)) {
                        mcode_app_action_commit(ui, app);
                        close_pop = true;
                    }
                }
            }
            if (close_pop) {
                cui_popover_close(ui, "sc-commit-more-pop");
            }
        }
    }
}

static void draw_change_group_header(mcode_app *app, cui_ctx *ui, const char *workdir,
                                     const char *id_prefix, const char *title,
                                     int count, bool *expanded, bool is_staged_group,
                                     bool persist_toggle) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    cui_push_id(ui, id_prefix);
    cui_vec2 mp = cui_mouse_pos(ui);
    cui_id hid = cui_make_id(ui, "hdr");
    cui_id aid = cui_make_id(ui, "act");
    bool hdr_hov = rect_contains(cui_get_rect(ui, hid), mp);
    bool act_hov = rect_contains(cui_get_rect(ui, aid), mp);

    cui_row(ui, .width = CUI_FILL, .pad_left = 6, .pad_right = 8, .gap = 2,
            .align = CUI_ALIGN_CENTER) {
        bool clicked = cui_card_begin(ui, &(cui_card_opts){
            .id = "hdr",
            .dir = CUI_ROW,
            .align = CUI_ALIGN_CENTER,
            .width = CUI_FILL,
            .pad_x = 4,
            .pad_y = 4,
            .gap = 5,
            .hoverable = true,
            .border = -1.0f,
            .radius = 5.0f,
            .bg = hdr_hov ? right_hover_bg(ui, dark ? 0.06f : 0.045f)
                          : CUI_RGBA(0x00000001u),
        });
        cui_icon(ui, *expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                 12.0f, cui_alpha(fg, hdr_hov ? 0.78f : 0.55f));
        cui_text(ui, title, .font = CUI_FONT_MEDIUM, .size = 12.5f,
                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL, .nowrap = true);
        cui_card_end(ui);

        if (clicked) {
            *expanded = !(*expanded);
            if (persist_toggle) mcode_ui_mark_dirty(app);
        }

        if ((hdr_hov || act_hov) && count > 0 && workdir && workdir[0]) {
            bool act_clicked = cui_card_begin(ui, &(cui_card_opts){
                .id = "act",
                .width = CUI_PX(22),
                .height = CUI_PX(22),
                .pad = 1,
                .hoverable = true,
                .border = -1.0f,
                .radius = 5.0f,
                .bg = act_hov ? right_hover_bg(ui, dark ? 0.12f : 0.08f)
                              : CUI_RGBA(0x00000001u),
            });
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, is_staged_group ? CUI_ICON_MINUS : CUI_ICON_PLUS, 12.0f,
                         act_hov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
            }
            cui_card_end(ui);
            cui_tooltip(ui, is_staged_group ? "Unstage All Changes" : "Stage All Changes");
            if (act_clicked) {
                char err[256] = "";
                bool ok = is_staged_group
                    ? mcode_git_unstage_all(workdir, err, sizeof err)
                    : mcode_git_stage_all(workdir, err, sizeof err);
                if (ok) {
                    promote_project_for_workdir(app, workdir);
                    mcode_ui_refresh_workspace_git(app);
                } else if (err[0]) {
                    app_chrome_notify(app->chrome, ui, err, CUI_TONE_DANGER);
                }
            }
        }

        /* VS Code-style circular/pill count badge */
        char cnt_buf[16];
        snprintf(cnt_buf, sizeof cnt_buf, "%d", count);
        cui_box(ui, .pad_x = 6, .pad_y = 1, .radius = 9.0f,
                .bg = cui_alpha(fg, dark ? 0.14f : 0.10f),
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, cnt_buf, .font = CUI_FONT_MEDIUM, .size = 10.5f,
                     .color = cui_alpha(fg, 0.85f), .tabular = true, .nowrap = true);
        }
    }
    cui_pop_id(ui);
}

/* Returns true if the list was mutated (e.g. stage/unstage triggered a refresh). */
static bool draw_change_file_rows(mcode_app *app, cui_ctx *ui, const char *workdir,
                                  bool want_staged) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    bool mutated = false;

    for (int i = 0; i < app->git_file_count; ++i) {
        const mcode_diff_file *f = &app->git_files[i];
        if (f->staged != want_staged) continue;

        bool selected = (app->inspector_open &&
                         !app->inspector_showing_chat &&
                         app->inspector_mode == MCODE_INSPECTOR_DIFF &&
                         i == app->git_selected_idx);

        char dir_part[384];
        const char *base_name = "";
        split_path_dir_base(f->path, dir_part, sizeof dir_part, &base_name);

        cui_push_id(ui, want_staged ? "stg" : "chg");
        cui_push_id_int(ui, i);
        cui_vec2 mp = cui_mouse_pos(ui);
        cui_id row_id = cui_make_id(ui, "row");
        cui_id btn_id = cui_make_id(ui, "stgbtn");
        bool row_hov = rect_contains(cui_get_rect(ui, row_id), mp);
        bool btn_hov = rect_contains(cui_get_rect(ui, btn_id), mp);
        bool any_hov = row_hov || btn_hov;

        cui_color row_bg = selected
            ? (dark ? CUI_RGBA(0x1b2434ffu) : CUI_RGBA(0xe2ecfcffu))
            : (any_hov ? right_hover_bg(ui, dark ? 0.065f : 0.045f)
                       : CUI_RGBA(0x00000001u));

        cui_row(ui, .width = CUI_FILL, .gap = 2, .align = CUI_ALIGN_CENTER) {
            bool clicked = cui_card_begin(ui, &(cui_card_opts){
                .id = "row",
                .dir = CUI_ROW,
                .align = CUI_ALIGN_CENTER,
                .width = CUI_FILL,
                .pad_x = 8,
                .pad_y = 3,
                .gap = 6,
                .hoverable = true,
                .border = -1.0f,
                .radius = 5.0f,
                .bg = row_bg,
            });

            draw_file_type_icon(ui, base_name, false);

            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER, .clip = true) {
                cui_text(ui, base_name,
                         .font = selected ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                         .size = 12.5f,
                         .color = selected ? CUI_COLOR_FOREGROUND
                                           : cui_mix(cui_color_of(ui, CUI_COLOR_FOREGROUND),
                                                     cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY),
                                                     0.20f),
                         .nowrap = true);
                if (dir_part[0]) {
                    cui_text(ui, dir_part, .size = 11.5f,
                             .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                }
            }

            const char *st_lbl = display_git_status_letter(f->status, f->untracked);
            cui_text(ui, st_lbl, .font = CUI_FONT_MEDIUM, .size = 11.5f,
                     .color = color_for_git_status_char(ui, st_lbl[0]),
                     .nowrap = true);

            cui_card_end(ui);

            bool rclick = cui_last_hovered(ui) && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT);
            if (!app->file_ctx_open) {
                char tip[640];
                const char *kind_lbl = want_staged ? "Staged" : (f->untracked ? "Untracked" : "Unstaged");
                if (f->added > 0 || f->removed > 0) {
                    snprintf(tip, sizeof tip, "%s  (%s · +%d -%d)", f->path, kind_lbl, f->added, f->removed);
                } else {
                    snprintf(tip, sizeof tip, "%s  (%s)", f->path, kind_lbl);
                }
                cui_tooltip(ui, tip);
            }

            /* Context menu */
            char path_copy[512];
            snprintf(path_copy, sizeof path_copy, "%s", f->path);
            bool is_deleted = (strcmp(f->status, "D") == 0);

            if (rclick) {
                open_file_context_menu(app, ui, MCODE_FILE_CTX_CHANGE,
                                       i, false, false, want_staged, is_deleted,
                                       workdir, path_copy);
            }

            if (any_hov && workdir && workdir[0]) {
                bool sclk = cui_card_begin(ui, &(cui_card_opts){
                    .id = "stgbtn",
                    .width = CUI_PX(20),
                    .height = CUI_PX(20),
                    .pad = 1,
                    .hoverable = true,
                    .border = -1.0f,
                    .radius = 4.0f,
                    .bg = btn_hov ? right_hover_bg(ui, dark ? 0.14f : 0.10f)
                                  : CUI_RGBA(0x00000001u),
                });
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_icon(ui, want_staged ? CUI_ICON_MINUS : CUI_ICON_PLUS, 11.5f,
                             btn_hov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
                }
                cui_card_end(ui);
                cui_tooltip(ui, want_staged ? "Unstage Changes" : "Stage Changes");
                if (sclk && !mutated) {
                    char err[256] = "";
                    bool ok = want_staged
                        ? mcode_git_unstage_file(workdir, path_copy, err, sizeof err)
                        : mcode_git_stage_file(workdir, path_copy, err, sizeof err);
                    if (ok) {
                        promote_project_for_workdir(app, workdir);
                        mcode_ui_refresh_workspace_git(app);
                        mutated = true;
                    } else if (err[0]) {
                        app_chrome_notify(app->chrome, ui, err, CUI_TONE_DANGER);
                    }
                }
            }

            if (clicked && !mutated) {
                mcode_ui_open_diff_file(app, i);
            }
        }

        cui_pop_id(ui);
        cui_pop_id(ui);
        if (mutated) break;
    }
    return mutated;
}

static void draw_source_control_lists(mcode_app *app, cui_ctx *ui,
                                      mcode_project *proj, const char *workdir) {
    cui_color bg = right_sidebar_bg(ui);

    if (proj && !proj->is_git_repo) {
        cui_column(ui, .width = CUI_FILL, .pad_x = 14, .pad_y = 12, .gap = 8,
                   .align = CUI_ALIGN_START) {
            cui_text(ui, "Not a Git repository.", .size = 12.0f,
                     .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, "Initialize Git Repo", .variant = CUI_BUTTON_SECONDARY,
                           .size = CUI_SIZE_SM, .icon = CUI_ICON_GIT_BRANCH)) {
                mcode_ui_init_project_git(app, ui, proj);
            }
        }
        return;
    }

    int staged_count = 0, unstaged_count = 0;
    count_git_groups(app, &staged_count, &unstaged_count);

    cui_scroll(ui, .id = "sc-changes-scroll", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 0, .fade_edges = true, .fade_color = bg) {
        cui_column(ui, .width = CUI_FILL, .pad_left = 4, .pad_right = 10,
                   .pad_top = 0, .pad_bottom = 6, .gap = 1) {
            bool mutated = false;
            if (staged_count > 0) {
                draw_change_group_header(app, ui, workdir, "sc-grp-staged",
                                         "Staged Changes", staged_count,
                                         &app->staged_expanded, true, false);
                if (app->staged_expanded) {
                    mutated = draw_change_file_rows(app, ui, workdir, true);
                }
                cui_gap(ui, 3);
            }

            if (!mutated) {
                /* When Staged Changes is also visible, show the Unstaged "Changes" subgroup header too */
                if (staged_count > 0) {
                    draw_change_group_header(app, ui, workdir, "sc-grp-unstaged",
                                             "Changes", unstaged_count,
                                             &app->store.changes_expanded, false, true);
                }
                if (app->store.changes_expanded) {
                    if (unstaged_count > 0) {
                        (void)draw_change_file_rows(app, ui, workdir, false);
                    } else if (staged_count == 0) {
                        cui_column(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 8) {
                            cui_text(ui, "Working tree clean", .size = 12.0f,
                                     .color = CUI_COLOR_TEXT_MUTED);
                        }
                    }
                }
            }
        }
    }
}

static void draw_files_header(mcode_app *app, cui_ctx *ui, const char *project_name,
                              const char *workdir) {
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    cui_vec2 mp = cui_mouse_pos(ui);
    cui_id hid = cui_make_id(ui, "exp-proj-hdr");
    cui_id cid = cui_make_id(ui, "exp-collapse");
    cui_id rid = cui_make_id(ui, "exp-refresh");
    bool hdr_hov = rect_contains(cui_get_rect(ui, hid), mp);
    bool col_hov = rect_contains(cui_get_rect(ui, cid), mp);
    bool ref_hov = rect_contains(cui_get_rect(ui, rid), mp);
    bool any_hov = hdr_hov || col_hov || ref_hov;

    cui_row(ui, .width = CUI_FILL, .pad_left = 6, .pad_right = 8, .pad_top = 2, .pad_bottom = 2,
            .gap = 2, .align = CUI_ALIGN_CENTER) {
        bool clicked = cui_card_begin(ui, &(cui_card_opts){
            .id = "exp-proj-hdr",
            .dir = CUI_ROW,
            .align = CUI_ALIGN_CENTER,
            .width = CUI_FILL,
            .pad_x = 4,
            .pad_y = 4,
            .gap = 5,
            .hoverable = true,
            .border = -1.0f,
            .radius = 5.0f,
            .bg = hdr_hov ? right_hover_bg(ui, dark ? 0.06f : 0.045f)
                          : CUI_RGBA(0x00000001u),
        });
        cui_icon(ui, app->store.files_expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                 12.0f, cui_alpha(fg, hdr_hov ? 0.78f : 0.55f));
        cui_text(ui, (project_name && project_name[0]) ? project_name : "Files",
                 .font = CUI_FONT_MEDIUM, .size = 12.5f,
                 .color = CUI_COLOR_FOREGROUND, .width = CUI_FILL, .nowrap = true);
        cui_card_end(ui);

        if (cui_last_hovered(ui) && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) {
            open_file_context_menu(app, ui, MCODE_FILE_CTX_FILES_HDR,
                                   -1, true, app->store.files_expanded, false, false,
                                   workdir, "");
        }

        if (clicked) {
            app->store.files_expanded = !app->store.files_expanded;
            mcode_ui_mark_dirty(app);
        }

        if (any_hov) {
            bool cclk = cui_card_begin(ui, &(cui_card_opts){
                .id = "exp-collapse",
                .width = CUI_PX(22),
                .height = CUI_PX(22),
                .pad = 1,
                .hoverable = true,
                .border = -1.0f,
                .radius = 5.0f,
                .bg = col_hov ? right_hover_bg(ui, dark ? 0.12f : 0.08f)
                              : CUI_RGBA(0x00000001u),
            });
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, CUI_ICON_MINIMIZE_2, 11.5f,
                         col_hov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
            }
            cui_card_end(ui);
            cui_tooltip(ui, "Collapse Folders in Explorer");
            if (cclk) {
                mcode_file_tree_collapse_all(&app->file_tree);
            }

            bool rclk = cui_card_begin(ui, &(cui_card_opts){
                .id = "exp-refresh",
                .width = CUI_PX(22),
                .height = CUI_PX(22),
                .pad = 1,
                .hoverable = true,
                .border = -1.0f,
                .radius = 5.0f,
                .bg = ref_hov ? right_hover_bg(ui, dark ? 0.12f : 0.08f)
                              : CUI_RGBA(0x00000001u),
            });
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, CUI_ICON_REFRESH_CW, 11.5f,
                         ref_hov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
            }
            cui_card_end(ui);
            cui_tooltip(ui, "Refresh Explorer");
            if (rclk) {
                mcode_ui_refresh_workspace_git(app);
            }
        }
    }
}

static int find_diff_index_for_path(const mcode_app *app, const char *rel_path) {
    if (!app || !rel_path) return -1;
    int staged_fallback = -1;
    for (int i = 0; i < app->git_file_count; ++i) {
        if (strcmp(app->git_files[i].path, rel_path) == 0) {
            if (!app->git_files[i].staged) return i;
            if (staged_fallback < 0) staged_fallback = i;
        }
    }
    return staged_fallback;
}

static void draw_file_tree_nodes(mcode_app *app, cui_ctx *ui,
                                 const char *workdir, int first_idx, int depth) {
    if (!app || depth > 48) return;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    for (int idx = first_idx; idx >= 0 && idx < app->file_tree.count; ) {
        mcode_fs_node node = app->file_tree.nodes[idx];
        int next_sib = node.next_sibling;

        bool selected = (!node.is_dir &&
                         app->inspector_open &&
                         !app->inspector_showing_chat &&
                         app->inspector_mode == MCODE_INSPECTOR_FILE &&
                         strcmp(app->preview_rel_path, node.rel_path) == 0);

        cui_push_id(ui, node.rel_path);
        cui_id row_id = cui_make_id(ui, "fs-row");
        bool hov = rect_contains(cui_get_rect(ui, row_id), cui_mouse_pos(ui));

        cui_color row_bg = selected
            ? (dark ? CUI_RGBA(0x1b2434ffu) : CUI_RGBA(0xe2ecfcffu))
            : (hov ? right_hover_bg(ui, dark ? 0.065f : 0.045f)
                   : CUI_RGBA(0x00000001u));

        bool clicked = cui_card_begin(ui, &(cui_card_opts){
            .id = "fs-row",
            .dir = CUI_ROW,
            .align = CUI_ALIGN_CENTER,
            .width = CUI_FILL,
            .pad_x = 8,
            .pad_y = 3,
            .gap = 5,
            .hoverable = true,
            .border = -1.0f,
            .radius = 5.0f,
            .bg = row_bg,
        });

        if (node.depth > 0) {
            cui_color guide = cui_alpha(fg, dark ? 0.08f : 0.07f);
            cui_row(ui, .gap = 0, .height = CUI_PX(16), .align = CUI_ALIGN_STRETCH) {
                for (int d = 0; d < node.depth; ++d) {
                    cui_box(ui, .width = CUI_PX(14), .height = CUI_FILL,
                            .pad_left = 5) {
                        cui_box(ui, .width = CUI_PX(1), .height = CUI_FILL, .bg = guide) {}
                    }
                }
            }
        }

        if (node.is_dir) {
            cui_box(ui, .width = CUI_PX(12), .height = CUI_PX(14),
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, node.expanded ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                         11.0f, cui_alpha(fg, node.git_ignored ? 0.30f : (hov ? 0.70f : 0.45f)));
            }
            cui_box(ui, .width = CUI_PX(16), .height = CUI_PX(16),
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, node.expanded ? CUI_ICON_FOLDER_OPEN : CUI_ICON_FOLDER,
                         13.5f, folder_icon_color(ui, node.name, node.git_ignored));
            }
        } else {
            cui_box(ui, .width = CUI_PX(12), .height = CUI_PX(14)) {}
            draw_file_type_icon(ui, node.name, node.git_ignored);
        }

        cui_color name_col;
        if (node.git_ignored) {
            name_col = cui_alpha(cui_color_of(ui, CUI_COLOR_TEXT_MUTED), dark ? 0.72f : 0.78f);
        } else if (selected) {
            name_col = CUI_COLOR_FOREGROUND;
        } else if (!node.is_dir && node.git_status != 0) {
            name_col = color_for_git_status_char(ui, node.git_status);
        } else {
            name_col = cui_mix(fg, cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY), 0.22f);
        }

        cui_text(ui, node.name,
                 .font = selected ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                 .size = 12.5f,
                 .color = name_col,
                 .width = CUI_FILL,
                 .nowrap = true);

        if (!node.is_dir && node.git_status != 0 && !node.git_ignored) {
            char st[2] = { node.git_status, '\0' };
            cui_text(ui, st, .font = CUI_FONT_MEDIUM, .size = 11.0f,
                     .color = color_for_git_status_char(ui, node.git_status),
                     .nowrap = true);
        } else if (node.is_dir && node.has_changed_descendant && !node.git_ignored) {
            cui_box(ui, .width = CUI_PX(5), .height = CUI_PX(5), .radius = 2.5f,
                    .bg = color_for_git_status_char(ui, 'M')) {}
        }

        cui_card_end(ui);

        bool rclick = cui_last_hovered(ui) && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT);
        if (!app->file_ctx_open) {
            cui_tooltip(ui, node.rel_path);
        }
        if (rclick) {
            open_file_context_menu(app, ui, MCODE_FILE_CTX_TREE,
                                   idx, node.is_dir, node.expanded, false, false,
                                   workdir, node.rel_path);
        }

        if (clicked) {
            if (node.is_dir) {
                mcode_file_tree_toggle(&app->file_tree, idx);
            } else {
                mcode_ui_open_file_preview(app, ui, workdir, node.rel_path);
            }
        }

        cui_pop_id(ui);

        if (idx < app->file_tree.count &&
            app->file_tree.nodes[idx].is_dir &&
            app->file_tree.nodes[idx].expanded &&
            app->file_tree.nodes[idx].first_child >= 0) {
            draw_file_tree_nodes(app, ui, workdir,
                                 app->file_tree.nodes[idx].first_child, depth + 1);
        }

        idx = next_sib;
    }
}

static void draw_files_tree_body(mcode_app *app, cui_ctx *ui, const char *workdir) {
    cui_color bg = right_sidebar_bg(ui);
    if (app->file_tree.first_root < 0) {
        cui_column(ui, .width = CUI_FILL, .pad_x = 16, .pad_y = 10) {
            cui_text(ui, "Empty folder", .size = 12.0f, .color = CUI_COLOR_TEXT_MUTED);
        }
        return;
    }

    cui_scroll(ui, .id = "exp-files-scroll", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 0, .fade_edges = true, .fade_color = bg) {
        cui_column(ui, .width = CUI_FILL, .pad_left = 4, .pad_right = 10,
                   .pad_top = 2, .pad_bottom = 10, .gap = 1) {
            draw_file_tree_nodes(app, ui, workdir, app->file_tree.first_root, 0);
        }
    }
}

/* ---- Right Sidebar (VS Code-style Changes + Project Files Tree) ---- */

void mcode_ui_draw_diff(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    mcode_project *proj = thread ? mcode_store_find_project(&app->store, thread->project_id) : NULL;
    if (!proj && app->store.selected_project_id[0]) {
        proj = mcode_store_find_project(&app->store, app->store.selected_project_id);
    }
    if (!proj && app->store.project_count > 0) {
        proj = &app->store.projects[0];
    }
    char proj_id[MCODE_ID_MAX] = "";
    if (proj) snprintf(proj_id, sizeof proj_id, "%s", proj->id);
    char workdir[MCODE_PATH_MAX] = "";
    if (thread && thread->workdir[0]) {
        snprintf(workdir, sizeof workdir, "%s", thread->workdir);
    } else if (proj && proj->path[0]) {
        snprintf(workdir, sizeof workdir, "%s", proj->path);
    }
    char proj_name[MCODE_NAME_MAX];
    snprintf(proj_name, sizeof proj_name, "%s", proj ? proj->name : "Project");

    cui_color bg = right_sidebar_bg(ui);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = bg, .gap = 0) {
        if (!workdir[0]) {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                       .pad = 20, .gap = 8) {
                cui_icon(ui, CUI_ICON_FOLDER_OPEN, 20, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, "No project open", .font = CUI_FONT_MEDIUM, .size = 12.5f,
                         .color = CUI_COLOR_TEXT_SECONDARY);
            }
        } else {
            /* Keep both Git changes and file tree synced with the active project */
            if (strcmp(app->git_workdir, workdir) != 0 ||
                strcmp(app->file_tree.root_path, workdir) != 0) {
                mcode_ui_refresh_workspace_git(app);
            }

            draw_right_sidebar_top_header(app, ui);

            int staged_count = 0, unstaged_count = 0;
            count_git_groups(app, &staged_count, &unstaged_count);
            bool any_changes_expanded = app->store.changes_expanded ||
                                        (staged_count > 0 && app->staged_expanded);
            bool files_open = app->store.files_expanded;

            if (any_changes_expanded && files_open) {
                /* Both sections open: draggable vertical split just like VS Code */
                cui_split(ui, "mcode-right-vsplit", &app->store.right_split_ratio,
                          .vertical = true, .min_first = 140, .min_second = 120,
                          .width = CUI_FILL, .height = CUI_FILL) {
                    /* Upper pane: Commit + Staged/Unstaged Changes */
                    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = bg, .gap = 0) {
                        if (!proj || proj->is_git_repo) {
                            draw_source_control_commit_box(app, ui, thread, proj, workdir);
                        }
                        if (staged_count == 0) {
                            draw_change_group_header(app, ui, workdir, "sc-grp-changes",
                                                     "Changes", unstaged_count,
                                                     &app->store.changes_expanded, false, true);
                        }
                        proj = proj_id[0] ? mcode_store_find_project(&app->store, proj_id) : NULL;
                        draw_source_control_lists(app, ui, proj, workdir);
                    }
                    cui_split_next(ui);
                    /* Lower pane: Project Files Tree */
                    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = bg, .gap = 0) {
                        cui_separator(ui);
                        draw_files_header(app, ui, proj_name, workdir);
                        draw_files_tree_body(app, ui, workdir);
                    }
                }
            } else if (any_changes_expanded && !files_open) {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = bg, .gap = 0) {
                    if (!proj || proj->is_git_repo) {
                        draw_source_control_commit_box(app, ui, thread, proj, workdir);
                    }
                    if (staged_count == 0) {
                        draw_change_group_header(app, ui, workdir, "sc-grp-changes",
                                                 "Changes", unstaged_count,
                                                 &app->store.changes_expanded, false, true);
                    }
                    proj = proj_id[0] ? mcode_store_find_project(&app->store, proj_id) : NULL;
                    draw_source_control_lists(app, ui, proj, workdir);
                }
                cui_separator(ui);
                draw_files_header(app, ui, proj_name, workdir);
            } else if (!any_changes_expanded && files_open) {
                if (!proj || proj->is_git_repo) {
                    draw_source_control_commit_box(app, ui, thread, proj, workdir);
                }
                if (staged_count > 0) {
                    draw_change_group_header(app, ui, workdir, "sc-grp-staged",
                                             "Staged Changes", staged_count,
                                             &app->staged_expanded, true, false);
                }
                draw_change_group_header(app, ui, workdir, "sc-grp-changes",
                                         "Changes", unstaged_count,
                                         &app->store.changes_expanded, false, true);
                cui_separator(ui);
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = bg, .gap = 0) {
                    draw_files_header(app, ui, proj_name, workdir);
                    draw_files_tree_body(app, ui, workdir);
                }
            } else {
                if (!proj || proj->is_git_repo) {
                    draw_source_control_commit_box(app, ui, thread, proj, workdir);
                }
                if (staged_count > 0) {
                    draw_change_group_header(app, ui, workdir, "sc-grp-staged",
                                             "Staged Changes", staged_count,
                                             &app->staged_expanded, true, false);
                }
                draw_change_group_header(app, ui, workdir, "sc-grp-changes",
                                         "Changes", unstaged_count,
                                         &app->store.changes_expanded, false, true);
                cui_separator(ui);
                draw_files_header(app, ui, proj_name, workdir);
            }
        }
    }
}

void mcode_ui_draw_file_context_menu(mcode_app *app, cui_ctx *ui) {
    if (!app || !ui || !app->file_ctx_open) return;

    bool just_opened = app->file_ctx_opened_this_frame;
    app->file_ctx_opened_this_frame = false;

    cui_id menu_id = cui_make_id(ui, "mcode-file-ctx-menu");
    cui_rect menu_rect = cui_get_rect(ui, menu_id);
    cui_vec2 mp = cui_mouse_pos(ui);

    if (!just_opened) {
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
            app->file_ctx_open = false;
            return;
        }
        if ((cui_mouse_clicked(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) &&
            !rect_contains(menu_rect, mp)) {
            app->file_ctx_open = false;
            return;
        }
    }

    const char *workdir = app->file_ctx_workdir;
    const char *rel_path = app->file_ctx_rel_path;
    bool has_workdir = (workdir[0] != '\0');
    int tree_diff_idx = -1;
    if (app->file_ctx_kind == MCODE_FILE_CTX_TREE && !app->file_ctx_is_dir) {
        tree_diff_idx = find_diff_index_for_path(app, rel_path);
    }

    int item_count = 0;
    int sep_count = 0;
    if (app->file_ctx_kind == MCODE_FILE_CTX_TREE) {
        item_count = app->file_ctx_is_dir ? 1 : (tree_diff_idx >= 0 ? 2 : 1);
        if (has_workdir) {
            item_count += 2;
            sep_count = 1;
        }
    } else if (app->file_ctx_kind == MCODE_FILE_CTX_CHANGE) {
        item_count = 1 + (!app->file_ctx_is_deleted && has_workdir ? 1 : 0);
        if (has_workdir) {
            sep_count = 1;
            item_count += 1 + (!app->file_ctx_is_deleted ? 2 : 0);
        }
    } else if (app->file_ctx_kind == MCODE_FILE_CTX_FILES_HDR) {
        item_count = 2 + (has_workdir ? 1 : 0);
        sep_count = has_workdir ? 1 : 0;
    }

    float w = context_menu_width();
    float est_h = (!just_opened && menu_rect.h > 0.0f)
        ? menu_rect.h
        : ((float)item_count * 26.0f + (float)sep_count * 5.0f + 10.0f);

    cui_rect root_r = cui_get_rect(ui, cui_make_id(ui, "mcode-root-stack"));
    cui_vec2 vp = cui_app_size(NULL);
    float origin_x = root_r.w > 0.0f ? root_r.x : 0.0f;
    float origin_y = root_r.h > 0.0f ? root_r.y : 0.0f;
    float avail_w = root_r.w > 0.0f ? root_r.w : vp.x;
    float avail_h = root_r.h > 0.0f ? root_r.h : vp.y;

    float x = app->file_ctx_x - origin_x;
    float y = app->file_ctx_y - origin_y + 2.0f;
    if (avail_w > 0.0f && x + w > avail_w - 8.0f) x = avail_w - 8.0f - w;
    if (x < 8.0f) x = 8.0f;
    if (avail_h > 0.0f && y + est_h > avail_h - 8.0f) {
        y = (app->file_ctx_y - origin_y) - est_h - 2.0f;
    }
    if (y < 8.0f) y = 8.0f;

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color bdr = dark ? cui_color_of(ui, CUI_COLOR_BORDER)
                         : cui_alpha(CUI_RGBA(0x000000ffu), 0.10f);
    bool close_now = false;

    cui_box(ui, .pad_left = x, .pad_top = y) {
        cui_box(ui, .id = "mcode-file-ctx-menu", .dir = CUI_STACK,
                .width = CUI_PX(w),
                .bg = CUI_COLOR_SURFACE_RAISED,
                .border = 1.0f,
                .border_color = bdr,
                .radius = 10.0f,
                .elevation = CUI_ELEVATION_FLOATING) {
            (void)cui_canvas(ui, "file-ctx-bg", CUI_FILL, CUI_FILL, NULL, NULL, 0);

            cui_column(ui, .width = CUI_FILL, .pad = 4, .gap = 1) {
                if (app->file_ctx_kind == MCODE_FILE_CTX_TREE) {
                    if (app->file_ctx_is_dir) {
                        if (draw_context_menu_item(ui, "fs-toggle",
                                                   app->file_ctx_is_expanded ? CUI_ICON_CHEVRON_RIGHT
                                                                             : CUI_ICON_CHEVRON_DOWN,
                                                   app->file_ctx_is_expanded ? "Collapse Folder"
                                                                             : "Expand Folder",
                                                   false)) {
                            int idx = app->file_ctx_index;
                            if (idx >= 0 && idx < app->file_tree.count &&
                                strcmp(app->file_tree.nodes[idx].rel_path, rel_path) == 0) {
                                mcode_file_tree_toggle(&app->file_tree, idx);
                            }
                            close_now = true;
                        }
                    } else {
                        if (draw_context_menu_item(ui, "fs-preview", CUI_ICON_FILE_TEXT,
                                                   "Open Preview", false)) {
                            mcode_ui_open_file_preview(app, ui, workdir, rel_path);
                            close_now = true;
                        }
                        if (tree_diff_idx >= 0 &&
                            draw_context_menu_item(ui, "fs-diff", CUI_ICON_LAYERS,
                                                   "View Diff", false)) {
                            mcode_ui_open_diff_file(app, tree_diff_idx);
                            close_now = true;
                        }
                    }
                    if (has_workdir) {
                        draw_context_menu_sep(ui);
                        if (draw_context_menu_item(ui, "fs-reveal", CUI_ICON_FOLDER_OPEN,
                                                   app_reveal_in_file_manager_label(), false)) {
                            char full[MCODE_PATH_MAX + 512];
                            snprintf(full, sizeof full, "%s/%s", workdir, rel_path);
                            reveal_in_file_manager(app, ui, full);
                            close_now = true;
                        }
                        if (draw_context_menu_item(ui, "fs-ext", CUI_ICON_EXTERNAL_LINK,
                                                   "Open Externally", false)) {
                            char full[MCODE_PATH_MAX + 512];
                            snprintf(full, sizeof full, "%s/%s", workdir, rel_path);
                            cui_app_open_url(full);
                            close_now = true;
                        }
                    }
                } else if (app->file_ctx_kind == MCODE_FILE_CTX_CHANGE) {
                    if (draw_context_menu_item(ui, "chg-diff", CUI_ICON_LAYERS, "View Diff", false)) {
                        if (app->file_ctx_index >= 0 && app->file_ctx_index < app->git_file_count) {
                            mcode_ui_open_diff_file(app, app->file_ctx_index);
                        }
                        close_now = true;
                    }
                    if (!app->file_ctx_is_deleted && has_workdir) {
                        if (draw_context_menu_item(ui, "chg-open", CUI_ICON_FILE_TEXT, "Open File", false)) {
                            mcode_ui_open_file_preview(app, ui, workdir, rel_path);
                            close_now = true;
                        }
                    }
                    if (has_workdir) {
                        draw_context_menu_sep(ui);
                        if (draw_context_menu_item(ui, "chg-stage",
                                                   app->file_ctx_is_staged ? CUI_ICON_MINUS : CUI_ICON_PLUS,
                                                   app->file_ctx_is_staged ? "Unstage Changes" : "Stage Changes",
                                                   false)) {
                            char err[256] = "";
                            bool ok = app->file_ctx_is_staged
                                ? mcode_git_unstage_file(workdir, rel_path, err, sizeof err)
                                : mcode_git_stage_file(workdir, rel_path, err, sizeof err);
                            if (ok) {
                                promote_project_for_workdir(app, workdir);
                                mcode_ui_refresh_workspace_git(app);
                            } else if (err[0]) {
                                app_chrome_notify(app->chrome, ui, err, CUI_TONE_DANGER);
                            }
                            close_now = true;
                        }
                        if (!app->file_ctx_is_deleted) {
                            if (draw_context_menu_item(ui, "chg-reveal", CUI_ICON_FOLDER_OPEN,
                                                       app_reveal_in_file_manager_label(), false)) {
                                char full[MCODE_PATH_MAX + 512];
                                snprintf(full, sizeof full, "%s/%s", workdir, rel_path);
                                reveal_in_file_manager(app, ui, full);
                                close_now = true;
                            }
                            if (draw_context_menu_item(ui, "chg-ext", CUI_ICON_EXTERNAL_LINK,
                                                       "Open Externally", false)) {
                                char full[MCODE_PATH_MAX + 512];
                                snprintf(full, sizeof full, "%s/%s", workdir, rel_path);
                                cui_app_open_url(full);
                                close_now = true;
                            }
                        }
                    }
                } else if (app->file_ctx_kind == MCODE_FILE_CTX_FILES_HDR) {
                    if (has_workdir) {
                        if (draw_context_menu_item(ui, "exp-hdr-reveal", CUI_ICON_FOLDER_OPEN,
                                                   app_reveal_in_file_manager_label(), false)) {
                            reveal_in_file_manager(app, ui, workdir);
                            close_now = true;
                        }
                        draw_context_menu_sep(ui);
                    }
                    if (draw_context_menu_item(ui, "exp-hdr-col", CUI_ICON_MINIMIZE_2,
                                               "Collapse All", false)) {
                        mcode_file_tree_collapse_all(&app->file_tree);
                        close_now = true;
                    }
                    if (draw_context_menu_item(ui, "exp-hdr-ref", CUI_ICON_REFRESH_CW,
                                               "Refresh", false)) {
                        mcode_ui_refresh_workspace_git(app);
                        close_now = true;
                    }
                }
            }
        }
    }

    if (close_now) {
        app->file_ctx_open = false;
    }
}

/* ---- Center Workspace Main (Chat + On-Demand Diff / File Inspector) ---- */

static void format_file_size(int64_t bytes, char *out, size_t cap) {
    if (bytes < 1024) snprintf(out, cap, "%lld B", (long long)bytes);
    else if (bytes < 1024 * 1024) snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

void mcode_ui_draw_workspace_main(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!app->inspector_open) {
        mcode_ui_draw_chat(app, ui, thread);
        return;
    }

    /* Validate diff selection if currently in diff mode */
    const mcode_diff_file *cur_diff = NULL;
    if (app->inspector_mode == MCODE_INSPECTOR_DIFF) {
        if (app->git_selected_idx >= 0 && app->git_selected_idx < app->git_file_count) {
            cur_diff = &app->git_files[app->git_selected_idx];
        } else {
            mcode_ui_close_inspector(app);
            mcode_ui_draw_chat(app, ui, thread);
            return;
        }
    }

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    const char *active_rel = (app->inspector_mode == MCODE_INSPECTOR_DIFF && cur_diff)
        ? cur_diff->path : app->preview_rel_path;
    const char *active_base = path_basename(active_rel);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_BACKGROUND, .gap = 0) {
        /* Compact center tab bar: [Chat]  [file/diff ×]   <contextual actions> */
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(36),
                .pad_x = 10, .gap = 6, .align = CUI_ALIGN_CENTER) {
            /* Chat tab */
            cui_id ctab_id = cui_make_id(ui, "ws-tab-chat");
            bool ctab_hov = rect_contains(cui_get_rect(ui, ctab_id), cui_mouse_pos(ui));
            bool chat_active = app->inspector_showing_chat;
            bool chat_clicked = cui_card_begin(ui, &(cui_card_opts){
                .id = "ws-tab-chat",
                .dir = CUI_ROW,
                .align = CUI_ALIGN_CENTER,
                .pad_x = 9,
                .pad_y = 4,
                .gap = 5,
                .hoverable = true,
                .radius = 6.0f,
                .border = chat_active ? 1.0f : -1.0f,
                .border_color = dark ? CUI_RGBA(0x2c2c34ffu) : cui_alpha(fg, 0.12f),
                .bg = chat_active
                    ? (dark ? CUI_RGBA(0x18181dffu) : CUI_RGBA(0xffffffffu))
                    : (ctab_hov ? cui_alpha(fg, 0.06f) : CUI_RGBA(0x00000001u)),
            });
            cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 12.5f,
                     chat_active ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "Chat",
                     .font = chat_active ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                     .size = 12.0f,
                     .color = chat_active ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_SECONDARY,
                     .nowrap = true);
            cui_card_end(ui);
            if (chat_clicked) app->inspector_showing_chat = true;

            /* Active File / Diff tab */
            cui_id ftab_id = cui_make_id(ui, "ws-tab-file");
            bool ftab_hov = rect_contains(cui_get_rect(ui, ftab_id), cui_mouse_pos(ui));
            bool file_active = !app->inspector_showing_chat;
            cui_row(ui, .gap = 0, .radius = 6.0f, .align = CUI_ALIGN_CENTER,
                    .border = file_active ? 1.0f : 0.0f,
                    .border_color = dark ? CUI_RGBA(0x2c2c34ffu) : cui_alpha(fg, 0.12f),
                    .bg = file_active
                        ? (dark ? CUI_RGBA(0x18181dffu) : CUI_RGBA(0xffffffffu))
                        : (ftab_hov ? cui_alpha(fg, 0.06f) : CUI_RGBA(0x00000001u))) {
                bool ftab_clicked = cui_card_begin(ui, &(cui_card_opts){
                    .id = "ws-tab-file",
                    .dir = CUI_ROW,
                    .align = CUI_ALIGN_CENTER,
                    .pad_x = 7,
                    .pad_y = 4,
                    .gap = 5,
                    .hoverable = true,
                    .border = -1.0f,
                    .radius = 6.0f,
                    .bg = CUI_RGBA(0x00000001u),
                });
                draw_file_type_icon(ui, active_base, false);
                cui_text(ui, active_base,
                         .font = file_active ? CUI_FONT_MEDIUM : CUI_FONT_BODY,
                         .size = 12.0f,
                         .color = file_active ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_SECONDARY,
                         .nowrap = true);
                if (app->inspector_mode == MCODE_INSPECTOR_DIFF && cur_diff) {
                    const char *st_lbl = display_git_status_letter(cur_diff->status,
                                                                   cur_diff->untracked);
                    cui_text(ui, st_lbl, .font = CUI_FONT_MEDIUM, .size = 10.5f,
                             .color = color_for_git_status_char(ui, st_lbl[0]),
                             .nowrap = true);
                }
                cui_card_end(ui);
                cui_tooltip(ui, active_rel);
                if (ftab_clicked) app->inspector_showing_chat = false;

                cui_id xid = cui_make_id(ui, "ws-tab-close");
                bool xhov = rect_contains(cui_get_rect(ui, xid), cui_mouse_pos(ui));
                bool xclk = cui_card_begin(ui, &(cui_card_opts){
                    .id = "ws-tab-close",
                    .width = CUI_PX(20),
                    .height = CUI_PX(20),
                    .pad = 1,
                    .hoverable = true,
                    .border = -1.0f,
                    .radius = 4.0f,
                    .bg = xhov ? cui_alpha(fg, 0.12f) : CUI_RGBA(0x00000001u),
                });
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL,
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_icon(ui, CUI_ICON_X, 11.0f,
                             xhov ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
                }
                cui_card_end(ui);
                cui_tooltip(ui, "Close");
                if (xclk) {
                    mcode_ui_close_inspector(app);
                }
            }

            if (app->inspector_open && !app->inspector_showing_chat) {
                /* Relative path crumb */
                cui_row(ui, .width = CUI_FILL, .pad_left = 4, .align = CUI_ALIGN_CENTER,
                        .clip = true) {
                    cui_text(ui, active_rel, .font = CUI_FONT_MONO, .size = 11.0f,
                             .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                }

                if (app->inspector_mode == MCODE_INSPECTOR_DIFF && cur_diff) {
                    draw_diff_line_stats(ui, cur_diff->added, cur_diff->removed);

                    static const char *const modes[] = { "Split", "Inline" };
                    int mode = app->store.diff_inline_mode ? 1 : 0;
                    if (cui_segmented(ui, "ws-diff-mode", modes, 2, &mode, .size = CUI_SIZE_SM)) {
                        app->store.diff_inline_mode = (mode == 1);
                        mcode_ui_mark_dirty(app);
                    }

                    /* Change hunk navigation next to Split / Inline */
                    int total_ch = app->git_diff_change_count;
                    if (total_ch > 0) {
                        char hpos[24];
                        int cur_one = (app->git_diff_change_idx >= 0 && app->git_diff_change_idx < total_ch)
                            ? (app->git_diff_change_idx + 1) : 1;
                        snprintf(hpos, sizeof hpos, "%d/%d", cur_one, total_ch);
                        cui_text(ui, hpos, .font = CUI_FONT_MONO, .size = 11.0f,
                                 .color = CUI_COLOR_TEXT_MUTED, .tabular = true, .nowrap = true);
                    }
                    cui_row(ui, .gap = 0, .align = CUI_ALIGN_CENTER) {
                        bool can_prev = (total_ch > 0 && app->git_diff_change_idx > 0);
                        bool can_next = (total_ch > 0 && app->git_diff_change_idx < total_ch - 1);
                        if (cui_icon_button(ui, CUI_ICON_ARROW_UP, .size = CUI_SIZE_SM,
                                            .variant = CUI_BUTTON_GHOST, .width = CUI_PX(24),
                                            .disabled = !can_prev,
                                            .tooltip = "Previous Change", .shortcut = "Alt Up")) {
                            if (app->git_diff_change_idx > 0) app->git_diff_change_idx--;
                        }
                        if (cui_icon_button(ui, CUI_ICON_ARROW_DOWN, .size = CUI_SIZE_SM,
                                            .variant = CUI_BUTTON_GHOST, .width = CUI_PX(24),
                                            .disabled = !can_next,
                                            .tooltip = "Next Change", .shortcut = "Alt Down")) {
                            if (app->git_diff_change_idx < 0) app->git_diff_change_idx = 0;
                            else if (app->git_diff_change_idx < total_ch - 1) app->git_diff_change_idx++;
                        }
                    }

                    if (strcmp(cur_diff->status, "D") != 0 && app->git_workdir[0]) {
                        if (cui_icon_button(ui, CUI_ICON_FILE_TEXT, .size = CUI_SIZE_SM,
                                            .variant = CUI_BUTTON_GHOST,
                                            .tooltip = "View Full File")) {
                            mcode_ui_open_file_preview(app, ui, app->git_workdir, cur_diff->path);
                        }
                    }
                    if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                        .variant = CUI_BUTTON_GHOST,
                                        .tooltip = "Refresh Diff", .shortcut = "Cmd R")) {
                        mcode_ui_refresh_workspace_git(app);
                    }
                } else if (app->inspector_mode == MCODE_INSPECTOR_FILE) {
                    int diff_idx = find_diff_index_for_path(app, app->preview_rel_path);
                    if (diff_idx >= 0) {
                        if (cui_button(ui, "Diff", .variant = CUI_BUTTON_GHOST,
                                       .size = CUI_SIZE_SM, .icon = CUI_ICON_LAYERS)) {
                            mcode_ui_open_diff_file(app, diff_idx);
                        }
                    }
                    cui_copy_button(ui, "ws-copy-path", app->preview_rel_path);
                    cui_tooltip(ui, "Copy Relative Path");
                    if (app->preview_workdir[0]) {
                        if (cui_icon_button(ui, CUI_ICON_EXTERNAL_LINK, .size = CUI_SIZE_SM,
                                            .variant = CUI_BUTTON_GHOST,
                                            .tooltip = "Open Externally")) {
                            char full[MCODE_PATH_MAX + 512];
                            snprintf(full, sizeof full, "%s/%s",
                                     app->preview_workdir, app->preview_rel_path);
                            cui_app_open_url(full);
                        }
                    }
                }
            }
        }
        cui_separator(ui);

        /* Body */
        if (!app->inspector_open || app->inspector_showing_chat) {
            mcode_ui_draw_chat(app, ui, thread);
        } else if (app->inspector_mode == MCODE_INSPECTOR_DIFF && cur_diff) {
            if (strcmp(app->git_diff_loaded_path, cur_diff->path) != 0 ||
                app->git_diff_loaded_staged != cur_diff->staged) {
                mcode_ui_load_workspace_diff(app);
            }
            const char *old_t = app->git_diff_old_text ? app->git_diff_old_text : "";
            const char *new_t = app->git_diff_new_text ? app->git_diff_new_text : "";
            char old_hdr[560];
            char new_hdr[560];
            if (cur_diff->staged) {
                snprintf(old_hdr, sizeof old_hdr, "%s (HEAD)", cur_diff->path);
                snprintf(new_hdr, sizeof new_hdr, "%s (Index)", cur_diff->path);
            } else {
                snprintf(old_hdr, sizeof old_hdr, "%s (%s)", cur_diff->path,
                         cur_diff->untracked ? "Empty" : "Index");
                snprintf(new_hdr, sizeof new_hdr, "%s (Working Tree)", cur_diff->path);
            }

            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6) {
                int prev_changes = app->git_diff_change_count;
                cui_diff_result dr = cui_diff_view(
                    ui, "mcode-diff-view", old_t, new_t,
                    .mode = app->store.diff_inline_mode ? CUI_DIFF_INLINE : CUI_DIFF_SPLIT,
                    .language = mcode_lang_from_path(cur_diff->path),
                    .old_title = old_hdr,
                    .new_title = new_hdr,
                    .current = &app->git_diff_change_idx,
                    .ignore_eol = true,
                    .height = -1);
                app->git_diff_change_count = dr.changes;
                if (dr.changes != prev_changes) {
                    cui_request_redraw(ui);
                }
            }
        } else if (app->inspector_mode == MCODE_INSPECTOR_FILE) {
            if (app->preview_is_binary) {
                char sz_buf[32];
                format_file_size(app->preview_file_size, sz_buf, sizeof sz_buf);
                char desc[128];
                snprintf(desc, sizeof desc, "Binary file (%s) — preview not available.", sz_buf);
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                           .pad = 24, .gap = 10) {
                    cui_icon(ui, CUI_ICON_FILE, 28, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, active_base, .font = CUI_FONT_MEDIUM, .size = 14.0f);
                    cui_muted(ui, desc);
                    if (app->preview_workdir[0] &&
                        cui_button(ui, "Open Externally", .variant = CUI_BUTTON_SECONDARY,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_EXTERNAL_LINK)) {
                        char full[MCODE_PATH_MAX + 512];
                        snprintf(full, sizeof full, "%s/%s",
                                 app->preview_workdir, app->preview_rel_path);
                        cui_app_open_url(full);
                    }
                }
            } else {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6) {
                    if (app->preview_truncated) {
                        char sz_buf[32];
                        format_file_size(app->preview_file_size, sz_buf, sizeof sz_buf);
                        char msg[128];
                        snprintf(msg, sizeof msg,
                                 "Showing first 512 KB of %s.", sz_buf);
                        cui_alert(ui, msg, .tone = CUI_TONE_INFO);
                    }
                    static char s_empty_buf[2] = "";
                    char *buf = app->preview_text ? app->preview_text : s_empty_buf;
                    size_t cap = app->preview_text ? app->preview_buf_cap : sizeof s_empty_buf;
                    cui_push_id(ui, app->preview_rel_path);
                    (void)cui_code_editor(ui, "mcode-file-preview", buf, cap,
                                          .lang = mcode_lang_from_path(app->preview_rel_path),
                                          .read_only = true,
                                          .height = -1,
                                          .size = 12.5f);
                    cui_pop_id(ui);
                }
            }
        }
    }
}
