#ifndef APP_CHROME_H
#define APP_CHROME_H

#include <cui/cui_app.h>
#include <cui/cui.h>
#include <stdbool.h>
#include "app_config.h"
#include "app_log.h"
#include "settings.h"

enum {
    SETTINGS_SECTION_GENERAL = 0,
    SETTINGS_SECTION_APPEARANCE,
    SETTINGS_SECTION_KEYBINDINGS,
    SETTINGS_SECTION_ABOUT,
    SETTINGS_SECTION_COUNT
};

enum {
    APP_MAX_ACTIONS = 60,
    APP_MAX_PALETTE_COMMANDS = 128,
    APP_ACTION_GROUP_MAX = 64,
    APP_ACTION_ICON_MAX = 16,
};

typedef enum {
    APP_MENU_NONE = 0,
    APP_MENU_FILE,
    APP_MENU_EDIT,
    APP_MENU_VIEW,
} app_menu_group;

typedef struct {
    const char *settings_path;
    bool software_renderer;
} app_cli_opts;

typedef void (*app_action_fn)(cui_ctx *ui, void *user);

typedef struct {
    const char *id;              /* Stable unique ID for lookup & keybinding persistence (e.g. "save"). */
    const char *title;           /* Display label in Command Palette, Keybindings, and menus (required). */
    const char *icon;            /* Optional CUI_ICON_* for Command Palette and Windows/Linux menus. */
    const char *group;           /* Optional Command Palette group (defaults to "Actions"). */
    app_menu_group menu;         /* Optional top-level menu placement (default APP_MENU_NONE). */
    cui_key_combo default_combo; /* Default keyboard shortcut, or {0} for unbound. */
    bool hide_from_palette;      /* Omit from Command Palette when true. */
    bool hide_from_keybindings;  /* Omit from Settings -> Keybindings & persistence when true. */
    bool disabled;               /* Initial disabled state (toggleable via app_chrome_set_action_enabled). */
    bool checked;                /* Initial menu checkmark state (toggleable via app_chrome_set_action_checked). */
    app_action_fn on_trigger;    /* Callback invoked when triggered via shortcut, menu, or palette. */
    void *user;                  /* Per-action user pointer (overrides default_user when non-NULL). */
} app_action_desc;

typedef struct {
    char id[APP_KEYBINDING_ID_MAX];
    char title[APP_KEYBINDING_LABEL_MAX];
    char icon[APP_ACTION_ICON_MAX];
    char group[APP_ACTION_GROUP_MAX];
    app_menu_group menu;
    cui_key_combo default_combo;
    int keybinding_index;        /* Index into app_settings.keybindings, or -1 if not keybindable. */
    bool hide_from_palette;
    bool enabled;
    bool checked;
    app_action_fn on_trigger;
    void *user;
} app_registered_action;

typedef struct {
    const char *title;           /* Command title shown & searched in the palette (required). */
    const char *icon;            /* Optional CUI_ICON_*. */
    const char *group;           /* Optional group heading (defaults to "Actions"). */
    const char *shortcut;        /* Optional shortcut hint label. */
    app_action_fn on_trigger;    /* Callback invoked when chosen. */
    void *user;                  /* Optional user pointer (defaults to provider user when NULL). */
} app_palette_command;

typedef struct app_palette_builder app_palette_builder;
typedef void (*app_palette_provider)(app_palette_builder *builder, void *user);

struct app_updater;

typedef struct {
    /* Settings & CLI options */
    app_settings settings;
    app_cli_opts cli;
    char env_settings_path[4096];

    /* Chrome UI state */
    bool settings_open;
    bool palette_open;
    int settings_section;
    char keybinding_filter[128];

    /* Registered app actions & dynamic Command Palette provider */
    app_registered_action actions[APP_MAX_ACTIONS];
    int action_count;
    int command_palette_kb;
    app_palette_provider palette_provider;
    void *palette_provider_user;

    /* Update checker state */
    struct app_updater *updater;
    bool update_startup_checked;
    bool update_auto_checked;
    bool update_check_pending;
    bool update_check_silent;
    bool update_notified;
    bool update_relaunching;
    bool update_relaunch_failed;
    int update_toast_handle;
    int last_update_state;

    /* Internal lifecycle & persistence tracking */
    bool settings_save_failed;
    bool settings_dirty;
    bool window_dirty;
    double window_dirty_since;
    bool window_fullscreen_pending;
    double window_fullscreen_since;
    bool window_reset_pending;
    double window_reset_since;
    bool window_reset_settling;
    double window_reset_settle_since;
    bool observed_window_has_pos;
    int observed_window_x;
    int observed_window_y;
    cui_ctx *window_state_ui;
    void *native_window;
    const char *window_backend;
    cui_ctx *appearance_ui;
    bool appearance_force_motion;
    bool has_cached_system_dark;
    int cached_system_dark;
    double system_theme_checked_at;
    bool env_theme_override;
    bool env_accent_override;
    bool env_screenshot_mode;
#ifdef __APPLE__
    int pending_action;
    cui_key pending_edit_key;
    cui_key injected_edit_key;
#endif
} app_chrome;

/* Parse CLI, load settings, and initialize chrome. Returns false when the app should exit with *exit_code. */
bool app_chrome_init(app_chrome *chrome, int argc, char **argv, int *exit_code);
cui_app_desc app_chrome_app_desc(const app_chrome *chrome);

/* Register app actions (automatically wired into shortcuts, Keybindings, Command Palette, and menus). */
bool app_chrome_register_action(app_chrome *chrome, const app_action_desc *desc);
int app_chrome_register_actions(app_chrome *chrome, const app_action_desc *descs,
                                size_t count, void *default_user);
bool app_chrome_trigger_action(app_chrome *chrome, cui_ctx *ui, const char *id);
bool app_chrome_set_action_enabled(app_chrome *chrome, const char *id, bool enabled);
bool app_chrome_set_action_checked(app_chrome *chrome, const char *id, bool checked);

/* Optional provider called while the Command Palette is open to inject dynamic commands. */
void app_chrome_set_palette_provider(app_chrome *chrome,
                                     app_palette_provider provider, void *user);
bool app_palette_add(app_palette_builder *builder, const app_palette_command *cmd);

/* Call at the start and end of frame() around your application UI. */
void app_chrome_begin_frame(app_chrome *chrome, cui_ctx *ui);
void app_chrome_end_frame(app_chrome *chrome, cui_ctx *ui);
void app_chrome_shutdown(app_chrome *chrome);

void app_chrome_notify(app_chrome *chrome, cui_ctx *ui, const char *message, cui_tone tone);
const app_settings *app_chrome_settings(const app_chrome *chrome);

#endif
