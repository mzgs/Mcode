#ifndef APP_MENU_H
#define APP_MENU_H

#include <stdbool.h>
#include "app_chrome.h"

typedef struct {
    bool *settings_open;
    int *settings_section;
    int about_section;
    bool *palette_open;
    void *user;
    void (*on_action)(int action_index, void *user);
    void (*on_edit_key)(cui_key key, void *user);
    void (*on_toggle_fullscreen)(void *user);
    void (*on_will_terminate)(void *user);
} app_menu_hooks;

typedef struct {
    int action_index;
    const char *title;
    app_menu_group menu;
    cui_key_combo combo;
    bool enabled;
    bool checked;
} app_menu_action;

typedef struct {
    cui_key_combo palette_combo;
    bool fullscreen;
    const app_menu_action *actions;
    int action_count;
} app_menu_state;

/* Call from frame() after CUI creates the menu; referenced state must outlive the app. */
void app_menu_install(const app_menu_hooks *hooks);
/* Sync dynamic menu items, shortcut labels, and fullscreen title on the native menu bar. */
void app_menu_sync(const app_menu_state *state);
/* Detach hooks and local event monitors before app_chrome is destroyed. */
void app_menu_shutdown(void);

#endif
