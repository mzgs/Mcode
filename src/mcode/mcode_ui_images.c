#include "mcode_ui_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32) && !defined(S_ISREG)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

enum {
    MCODE_IMAGE_MAX_BYTES = 64 * 1024 * 1024,
};

static const double MCODE_IMAGE_STAT_INTERVAL_SEC = 1.0;

static bool starts_with_icase(const char *s, const char *prefix) {
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
    if (starts_with_icase(s, "mailto:") || starts_with_icase(s, "tel:") ||
        starts_with_icase(s, "data:")) {
        return true;
    }
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

static const char *path_basename(const char *path) {
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = (bslash && (!slash || bslash > slash)) ? bslash : slash;
    return (sep && sep[1]) ? sep + 1 : path;
}

bool mcode_image_has_supported_ext(const char *path) {
    if (!path || !path[0]) return false;
    size_t len = 0;
    while (path[len] && path[len] != '?' && path[len] != '#') ++len;
    while (len > 0 && isspace((unsigned char)path[len - 1])) --len;
    if (len < 4) return false;

    const char *dot = NULL;
    for (size_t i = len; i > 0; --i) {
        char ch = path[i - 1];
        if (ch == '/' || ch == '\\') break;
        if (ch == '.') {
            dot = path + (i - 1);
            break;
        }
    }
    if (!dot) return false;
    size_t ext_len = (size_t)((path + len) - (dot + 1));
    if (ext_len == 3) {
        return starts_with_icase(dot + 1, "png") || starts_with_icase(dot + 1, "jpg");
    }
    if (ext_len == 4) {
        return starts_with_icase(dot + 1, "jpeg");
    }
    return false;
}

bool mcode_ui_resolve_local_path(const char *raw_link, const char *workdir,
                                 bool strip_line_suffix,
                                 char *out_path, size_t out_cap) {
    if (out_path && out_cap > 0) out_path[0] = '\0';
    if (!raw_link || !out_path || out_cap == 0) return false;

    while (*raw_link == ' ' || *raw_link == '\t') ++raw_link;
    if (!raw_link[0]) return false;

    char trimmed[MCODE_PATH_MAX];
    size_t rlen = strlen(raw_link);
    while (rlen > 0 && isspace((unsigned char)raw_link[rlen - 1])) --rlen;
    if (rlen >= 2 && raw_link[0] == '<' && raw_link[rlen - 1] == '>') {
        ++raw_link;
        rlen -= 2;
        while (rlen > 0 && isspace((unsigned char)raw_link[0])) { ++raw_link; --rlen; }
        while (rlen > 0 && isspace((unsigned char)raw_link[rlen - 1])) --rlen;
    }
    if (rlen == 0) return false;
    if (rlen >= sizeof trimmed) rlen = sizeof trimmed - 1;
    memcpy(trimmed, raw_link, rlen);
    trimmed[rlen] = '\0';

    bool is_file_uri = starts_with_icase(trimmed, "file://");
    if (!is_file_uri && has_non_file_uri_scheme(trimmed)) {
        return false;
    }

    char path[MCODE_PATH_MAX] = "";
    if (is_file_uri) {
        const char *p = trimmed + 7;
        if (starts_with_icase(p, "localhost/")) p += 9;
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
        snprintf(path, sizeof path, "%s", trimmed);
    }

    if (!path[0]) return false;

    /* Expand leading ~/ or resolve relative paths against workdir before stripping suffixes. */
    size_t suffix_base = 0;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\\' || path[1] == '\0')) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
        if (home && home[0]) {
            snprintf(out_path, out_cap, "%s%s", home, path + 1);
            suffix_base = strlen(home);
        } else {
            snprintf(out_path, out_cap, "%s", path);
        }
    } else {
        bool is_abs = (path[0] == '/' || path[0] == '\\' ||
                       (isalpha((unsigned char)path[0]) && path[1] == ':' &&
                        (path[2] == '/' || path[2] == '\\')));
        if (!is_abs && workdir && workdir[0]) {
            size_t wlen = strlen(workdir);
            while (wlen > 1 && (workdir[wlen - 1] == '/' || workdir[wlen - 1] == '\\')) {
                --wlen;
            }
            const char *rel = path;
            if (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
            snprintf(out_path, out_cap, "%.*s/%s", (int)wlen, workdir, rel);
            suffix_base = wlen + 1;
        } else {
            snprintf(out_path, out_cap, "%s", path);
        }
    }

    size_t out_len = strlen(out_path);
    if (suffix_base > out_len) suffix_base = out_len;

    /* Strip #fragment, ?query, and optionally :line[:col] when literal path does not exist. */
    if (!path_exists_on_disk(out_path)) {
        char *hash = strchr(out_path + suffix_base, '#');
        if (hash) *hash = '\0';
        if (!path_exists_on_disk(out_path)) {
            char *q = strchr(out_path + suffix_base, '?');
            if (q) *q = '\0';
        }
        if (strip_line_suffix && !path_exists_on_disk(out_path)) {
            strip_line_col_suffix(out_path);
        }
    }

    return path_exists_on_disk(out_path);
}

static bool stat_image_file(const char *path, int64_t *out_size,
                            int64_t *out_msec, int64_t *out_mnsec) {
    if (out_size) *out_size = 0;
    if (out_msec) *out_msec = 0;
    if (out_mnsec) *out_mnsec = 0;
    if (!path || !path[0]) return false;

    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    if (st.st_size <= 0 || st.st_size > MCODE_IMAGE_MAX_BYTES) return false;

    if (out_size) *out_size = (int64_t)st.st_size;
#if defined(__APPLE__)
    if (out_msec) *out_msec = (int64_t)st.st_mtimespec.tv_sec;
    if (out_mnsec) *out_mnsec = (int64_t)st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    if (out_msec) *out_msec = (int64_t)st.st_mtime;
    if (out_mnsec) *out_mnsec = 0;
#else
    if (out_msec) *out_msec = (int64_t)st.st_mtim.tv_sec;
    if (out_mnsec) *out_mnsec = (int64_t)st.st_mtim.tv_nsec;
#endif
    return true;
}

static void cache_entry_load(mcode_image_cache_entry *e, cui_ctx *ui, const char *abs_path,
                             int64_t fsz, int64_t msec, int64_t mnsec, double now) {
    if (e->tex) {
        cui_image_destroy(ui, e->tex);
        e->tex = 0;
    }
    e->width = 0;
    e->height = 0;
    e->file_size = fsz;
    e->mtime_sec = msec;
    e->mtime_nsec = mnsec;
    e->checked_mono = now;

    int w = 0, h = 0;
    cui_texture tex = cui_image_load(ui, abs_path, &w, &h);
    if (tex && w > 0 && h > 0) {
        e->tex = tex;
        e->width = w;
        e->height = h;
        e->failed = false;
    } else {
        if (tex) cui_image_destroy(ui, tex);
        e->failed = true;
    }
}

const mcode_image_cache_entry *mcode_ui_image_get(mcode_app *app, cui_ctx *ui,
                                                  const char *abs_path) {
    if (!app || !ui || !abs_path || !abs_path[0]) return NULL;
    if (!mcode_image_has_supported_ext(abs_path)) return NULL;

    mcode_image_cache *cache = &app->image_cache;
    uint64_t frame = cui_frame_count(ui);
    double now = mcode_now_mono();

    int found = -1;
    int empty = -1;
    int lru = 0;
    uint64_t lru_frame = UINT64_MAX;

    for (int i = 0; i < MCODE_IMAGE_CACHE_MAX; ++i) {
        mcode_image_cache_entry *e = &cache->entries[i];
        if (!e->occupied) {
            if (empty < 0) empty = i;
            continue;
        }
        if (strcmp(e->path, abs_path) == 0) {
            found = i;
            break;
        }
        if (e->last_used_frame < lru_frame) {
            lru_frame = e->last_used_frame;
            lru = i;
        }
    }

    if (found >= 0) {
        mcode_image_cache_entry *e = &cache->entries[found];
        e->last_used_frame = frame;
        if (now - e->checked_mono >= MCODE_IMAGE_STAT_INTERVAL_SEC) {
            int64_t fsz = 0, msec = 0, mnsec = 0;
            if (!stat_image_file(abs_path, &fsz, &msec, &mnsec)) {
                if (e->tex) {
                    cui_image_destroy(ui, e->tex);
                    e->tex = 0;
                }
                e->width = 0;
                e->height = 0;
                e->file_size = 0;
                e->mtime_sec = 0;
                e->mtime_nsec = 0;
                e->failed = true;
                e->checked_mono = now;
            } else if (fsz != e->file_size || msec != e->mtime_sec || mnsec != e->mtime_nsec) {
                cache_entry_load(e, ui, abs_path, fsz, msec, mnsec, now);
            } else {
                e->checked_mono = now;
            }
        }
        return (!e->failed && e->tex != 0 && e->width > 0 && e->height > 0) ? e : NULL;
    }

    int slot = (empty >= 0) ? empty : lru;
    mcode_image_cache_entry *e = &cache->entries[slot];
    if (e->occupied && e->tex) {
        cui_image_destroy(ui, e->tex);
    }
    memset(e, 0, sizeof *e);
    e->occupied = true;
    e->last_used_frame = frame;
    snprintf(e->path, sizeof e->path, "%s", abs_path);

    int64_t fsz = 0, msec = 0, mnsec = 0;
    if (!stat_image_file(abs_path, &fsz, &msec, &mnsec)) {
        e->failed = true;
        e->checked_mono = now;
        return NULL;
    }

    cache_entry_load(e, ui, abs_path, fsz, msec, mnsec, now);
    return (!e->failed && e->tex != 0 && e->width > 0 && e->height > 0) ? e : NULL;
}

void mcode_ui_open_image_lightbox(mcode_app *app, const char *abs_path, const char *title) {
    if (!app || !abs_path || !abs_path[0]) return;
    snprintf(app->image_lightbox_path, sizeof app->image_lightbox_path, "%s", abs_path);
    const char *base = path_basename(abs_path);
    const char *lbl = (title && title[0]) ? title : base;
    if (!lbl || !lbl[0]) lbl = "Image Preview";
    size_t llen = strlen(lbl);
    if (llen > 72) {
        snprintf(app->image_lightbox_title, sizeof app->image_lightbox_title, "%.69s...", lbl);
    } else {
        snprintf(app->image_lightbox_title, sizeof app->image_lightbox_title, "%s", lbl);
    }
    memset(&app->image_lightbox_view, 0, sizeof app->image_lightbox_view);
    app->image_lightbox_open = true;
}

bool mcode_ui_draw_image_preview_card(mcode_app *app, cui_ctx *ui, const char *id,
                                      const char *abs_path, const char *caption,
                                      float max_w, float max_h, bool compact) {
    const mcode_image_cache_entry *img = mcode_ui_image_get(app, ui, abs_path);
    if (!img) return false;

    if (max_w < 32.0f) max_w = 32.0f;
    if (max_h < 32.0f) max_h = 32.0f;

    float iw = (float)img->width;
    float ih = (float)img->height;
    float scale = 1.0f;
    if (iw > max_w) scale = max_w / iw;
    if (ih * scale > max_h) scale = max_h / ih;
    float disp_w = floorf(iw * scale);
    float disp_h = floorf(ih * scale);
    if (disp_w < 1.0f) disp_w = 1.0f;
    if (disp_h < 1.0f) disp_h = 1.0f;

    const char *base = path_basename(abs_path);
    const char *label = (caption && caption[0]) ? caption : base;
    if (!label || !label[0]) label = "Image";

    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color surface = cui_color_of(ui, CUI_COLOR_SURFACE);

    cui_id cid = cui_make_id(ui, id ? id : "img-preview");
    cui_rect r = cui_get_rect(ui, cid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hovered = r.w > 0.0f && mp.x >= r.x && mp.x <= r.x + r.w &&
                   mp.y >= r.y && mp.y <= r.y + r.h;

    cui_color card_bg = dark
        ? cui_mix(surface, fg, hovered ? 0.06f : 0.035f)
        : cui_mix(surface, fg, hovered ? 0.04f : 0.02f);
    cui_color card_bdr = hovered
        ? cui_color_of(ui, CUI_COLOR_BORDER_STRONG)
        : cui_color_of(ui, CUI_COLOR_BORDER);

    float pad = compact ? 5.0f : 6.0f;
    float footer_w = disp_w;
    if (compact) {
        if (footer_w < 84.0f) footer_w = 84.0f;
        if (footer_w > max_w) footer_w = max_w;
    } else {
        if (footer_w < 180.0f) footer_w = 180.0f;
        if (footer_w > max_w) footer_w = max_w;
    }

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = id ? id : "img-preview",
        .dir = CUI_COLUMN,
        .align = CUI_ALIGN_CENTER,
        .width = CUI_PX(footer_w + 2.0f * pad),
        .pad = pad,
        .gap = compact ? 4.0f : 6.0f,
        .hoverable = true,
        .border = 1.0f,
        .border_color = card_bdr,
        .radius = compact ? 8.0f : 10.0f,
        .bg = card_bg,
    });

    cui_image(ui, img->tex,
              .width = CUI_PX(disp_w),
              .height = CUI_PX(disp_h),
              .radius = compact ? 5.0f : 7.0f);

    cui_row(ui, .width = CUI_PX(footer_w), .pad_x = 2.0f, .gap = 6.0f,
            .align = CUI_ALIGN_CENTER, .clip = true) {
        cui_text(ui, label,
                 .font = compact ? CUI_FONT_BODY : CUI_FONT_MEDIUM,
                 .size = compact ? 11.0f : 12.0f,
                 .color = CUI_COLOR_TEXT_SECONDARY,
                 .width = CUI_FILL,
                 .nowrap = true);
        if (!compact) {
            char dims[32];
            snprintf(dims, sizeof dims, "%d×%d", img->width, img->height);
            cui_text(ui, dims,
                     .font = CUI_FONT_MONO,
                     .size = 11.0f,
                     .color = CUI_COLOR_TEXT_MUTED,
                     .tabular = true,
                     .nowrap = true);
            cui_icon(ui, CUI_ICON_MAXIMIZE_2, 12.0f,
                     hovered ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED);
        }
    }

    cui_card_end(ui);

    char tip[320];
    snprintf(tip, sizeof tip, "Click to inspect · %s (%d×%d)",
             base[0] ? base : label, img->width, img->height);
    cui_tooltip(ui, tip);

    if (clicked) {
        mcode_ui_open_image_lightbox(app, abs_path, label);
    }
    return true;
}
