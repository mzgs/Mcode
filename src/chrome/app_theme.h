#ifndef APP_THEME_H
#define APP_THEME_H

#include "settings.h"
#include <cui/cui.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    app_theme_mode mode;
    const char *id;
    const char *label;
    const char *palette_title;
    const char *icon;
    bool is_dark;
    bool has_accent;

    /* Recessed chrome rails (sidebar, topbar, status bar, terminal toolbar) */
    cui_color chrome_bg;
    cui_color chrome_input_bg;

    /* Core CUI semantic tokens */
    cui_color background;
    cui_color surface;
    cui_color surface_raised;
    cui_color surface_muted;
    cui_color foreground;
    cui_color text_secondary;
    cui_color text_muted;
    cui_color border;
    cui_color border_subtle;
    cui_color border_strong;

    /* Default accent & controls (used when user accent is CUI_ACCENT_NEUTRAL) */
    cui_color accent;
    cui_color accent_strong;
    cui_color accent_subtle;
    cui_color accent_foreground;
    cui_color focus_ring;
    cui_color control_on;
    cui_color control_glyph;
    cui_color control_track;
    cui_color control_track_hover;
    cui_color control_thumb;
    cui_color control_thumb_on;

    /* Status, tones, overlay, and chart series */
    cui_color success;
    cui_color warning;
    cui_color danger;
    cui_color info;
    cui_color tone_sky;
    cui_color tone_teal;
    cui_color tone_green;
    cui_color tone_amber;
    cui_color tone_rose;
    cui_color tone_orchid;
    cui_color tone_violet;
    cui_color scrim;
    cui_color series[6];

    /* Integrated terminal palette */
    cui_color term_bg;
    cui_color term_fg;
    cui_color term_cursor;
    cui_color term_selection;
    uint32_t term_ansi[16];
} app_theme_spec;

/* Metadata and palette for any app_theme_mode (APP_THEME_SYSTEM returns metadata
 * with Dark Default preview colors as fallback; resolve System before applying). */
const app_theme_spec *app_theme_spec_get(app_theme_mode mode);

/* Preferred presentation order for the Settings -> Appearance theme gallery
 * and Command Palette. */
const app_theme_mode *app_theme_display_order(int *out_count);

/* Apply a concrete (non-System) theme palette onto `ui`'s live cui_theme,
 * preserving explicit non-neutral user accent overrides. */
void app_theme_apply(cui_ctx *ui, app_theme_mode resolved_mode, cui_accent accent);

/* Active resolved theme spec for `ui` (falls back to Dark Default / Light
 * matching cui_get_theme_mode(ui) if no theme has been applied to `ui` yet). */
const app_theme_spec *app_theme_current(cui_ctx *ui);

/* True when the active theme or explicit accent provides a chromatic accent
 * (as opposed to neutral monochrome on Dark Default / Light). */
bool app_theme_has_accent(cui_ctx *ui);

#endif
