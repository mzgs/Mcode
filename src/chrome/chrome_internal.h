#ifndef CHROME_INTERNAL_H
#define CHROME_INTERNAL_H

#include "app_chrome.h"
#include "updater.h"

extern const char *const chrome_theme_icons[APP_THEME_COUNT];
extern const char *const chrome_section_labels[SETTINGS_SECTION_COUNT];
extern const char *const chrome_section_icons[SETTINGS_SECTION_COUNT];

bool app_contains_ignore_case(const char *text, const char *query);

app_settings chrome_default_settings(void);
void chrome_reset_settings(app_chrome *chrome);
void chrome_apply_appearance(app_chrome *chrome, cui_ctx *ui);
void chrome_reload_appearance(app_chrome *chrome, cui_ctx *ui);
bool chrome_save_settings(app_chrome *chrome, cui_ctx *ui);
void chrome_open_settings_folder(app_chrome *chrome, cui_ctx *ui);
void chrome_open_log_file(app_chrome *chrome, cui_ctx *ui);
void chrome_format_debug_info(const app_chrome *chrome, cui_ctx *ui, char *out, size_t cap);
void chrome_reset_window(app_chrome *chrome, cui_ctx *ui);
void chrome_toggle_fullscreen(void);
cui_key_combo chrome_action_combo(const app_chrome *chrome, const app_registered_action *action);
cui_key_combo chrome_palette_combo(const app_chrome *chrome);
int chrome_keybinding_conflict(const app_chrome *chrome, int index);
bool chrome_trigger_action_index(app_chrome *chrome, cui_ctx *ui, int index);

void chrome_check_for_updates(app_chrome *chrome, bool silent);
void chrome_start_update(app_chrome *chrome, cui_ctx *ui);
void chrome_updater_snapshot(app_chrome *chrome, app_update_snapshot *out);

void chrome_draw_command_palette(app_chrome *chrome, cui_ctx *ui);
void chrome_draw_settings_dialog(app_chrome *chrome, cui_ctx *ui);

/* Exposed for unit tests: collect & stably coalesce palette commands for chrome. */
typedef struct {
    cui_command commands[APP_MAX_PALETTE_COMMANDS];
    int count;
    char text_pool[8192];
    size_t text_used;
} chrome_palette_snapshot;
void chrome_collect_palette_snapshot(app_chrome *chrome, cui_ctx *ui, chrome_palette_snapshot *out);

#endif
