#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <cui/cui.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_KEYBINDING_COMMAND_PALETTE "command_palette"

enum {
    WINDOW_WIDTH_MIN = 360,
    WINDOW_WIDTH_MAX = 8192,
    WINDOW_HEIGHT_MIN = 280,
    WINDOW_HEIGHT_MAX = 8192,
    APP_CONFLICT_NONE = -1,
    APP_CONFLICT_SETTINGS = -2,
    APP_MAX_KEYBINDINGS = 64,
    APP_KEYBINDING_ID_MAX = 64,
    APP_KEYBINDING_LABEL_MAX = 96,
    APP_CLI_PATH_MAX = 512,
    APP_MODEL_NAME_MAX = 96,
};

#define FONT_SCALE_MIN 0.85f
#define FONT_SCALE_MAX 1.35f

typedef enum {
    APP_AGENT_CLAUDE = 0,
    APP_AGENT_CODEX,
    APP_AGENT_AGY,
    APP_AGENT_COUNT
} app_agent_kind;

typedef enum {
    APP_THEME_SYSTEM = 0,
    APP_THEME_LIGHT,
    APP_THEME_DARK,
    APP_THEME_COUNT
} app_theme_mode;

typedef struct {
    char id[APP_KEYBINDING_ID_MAX];
    char label[APP_KEYBINDING_LABEL_MAX];
    cui_key_combo combo;
    cui_key_combo default_combo;
} app_keybinding;

typedef struct {
    char id[APP_KEYBINDING_ID_MAX];
    cui_key_combo combo;
} app_deferred_keybinding;

typedef struct {
    /* Mcode agent & workflow preferences. */
    struct {
        app_agent_kind default_agent;
        bool auto_approve_permissions;
        char claude_bin[APP_CLI_PATH_MAX];
        char codex_bin[APP_CLI_PATH_MAX];
        char agy_bin[APP_CLI_PATH_MAX];
        char default_claude_model[APP_MODEL_NAME_MAX];
        char default_codex_model[APP_MODEL_NAME_MAX];
        char default_agy_model[APP_MODEL_NAME_MAX];
    } mcode;

    /* Persisted appearance preferences. */
    struct {
        app_theme_mode theme_mode;
        cui_accent accent;
        float font_scale;
        bool reduced_motion;
    } appearance;

    /* Persisted window geometry and fullscreen state. */
    struct {
        bool has_pos;
        int x;
        int y;
        int width;
        int height;
        bool fullscreen;
    } window;

    /* Persisted update preferences. */
    struct {
        bool check_on_startup;
        int64_t last_checked;
    } updates;

    /* Configurable action keybindings (built-in + app-registered). */
    app_keybinding keybindings[APP_MAX_KEYBINDINGS];
    int keybinding_count;

    /* Internal: keybindings loaded from settings.json before actions register. */
    app_deferred_keybinding deferred_keybindings[APP_MAX_KEYBINDINGS];
    int deferred_keybinding_count;
    bool defer_unknown_keybindings;
} app_settings;

/* A NULL path uses the app's per-user settings.json. */
bool app_settings_load(app_settings *settings, const char *path);
bool app_settings_save(const app_settings *settings, const char *path);
bool app_settings_dir(char *out, size_t capacity, const char *path, bool create);

/* Register a keybinding (reusing any deferred combo loaded from settings.json).
 * Returns its index in settings->keybindings, or -1 on invalid input / full table. */
int app_settings_register_keybinding(app_settings *settings, const char *id,
                                     const char *label, cui_key_combo default_combo);
int app_settings_find_keybinding(const app_settings *settings, const char *id);
void app_settings_reset_keybindings(app_settings *settings);

/* Keybinding dispatch and conflict detection. */
bool app_keybinding_pressed(cui_ctx *ui, const app_settings *settings, int index);
/* Returns another conflicting keybinding index, APP_CONFLICT_SETTINGS if it
 * matches a reserved built-in shortcut, or APP_CONFLICT_NONE. */
int app_keybinding_conflict(const app_settings *settings, int index);

#endif
