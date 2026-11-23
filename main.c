#include <cui/cui_app.h>
#include <cui/cui.h>
#include "app_chrome.h"

typedef struct {
    app_chrome chrome;
    /* Add persistent application state here. */
} app_state;

static void on_save(cui_ctx *ui, void *user) {
    app_state *app = user;
    app_chrome_notify(&app->chrome, ui, "Save triggered.", CUI_TONE_SUCCESS);
}

static void on_find(cui_ctx *ui, void *user) {
    app_state *app = user;
    app_chrome_notify(&app->chrome, ui, "Find triggered.", CUI_TONE_INFO);
}

static void frame(cui_ctx *ui, void *user) {
    app_state *app = user;
    app_chrome_begin_frame(&app->chrome, ui);

    /* Add your UI here. */

    app_chrome_end_frame(&app->chrome, ui);
}

int main(int argc, char **argv) {
    static app_state app;
    static const app_action_desc actions[] = {
        {
            .id = "save",
            .title = "Save",
            .icon = CUI_ICON_SAVE,
            .menu = APP_MENU_FILE,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_S },
            .on_trigger = on_save,
        },
        {
            .id = "find",
            .title = "Find",
            .icon = CUI_ICON_SEARCH,
            .menu = APP_MENU_EDIT,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_F },
            .on_trigger = on_find,
        },
    };

    int exit_code = 0;
    if (!app_chrome_init(&app.chrome, argc, argv, &exit_code))
        return exit_code;
    app_chrome_register_actions(&app.chrome, actions,
                                sizeof actions / sizeof actions[0], &app);

    cui_app_desc desc = app_chrome_app_desc(&app.chrome);
    int status = cui_app_run(&desc, frame, &app);
    app_chrome_shutdown(&app.chrome);
    return status;
}
