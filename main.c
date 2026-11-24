#include <cui/cui_app.h>
#include <cui/cui.h>
#include "app_chrome.h"
#include "mcode_ui.h"

typedef struct {
    app_chrome chrome;
    mcode_app mcode;
} app_state;

static void frame(cui_ctx *ui, void *user) {
    app_state *app = user;
    app_chrome_begin_frame(&app->chrome, ui);
    mcode_app_frame(&app->mcode, ui);
    app_chrome_end_frame(&app->chrome, ui);
}

int main(int argc, char **argv) {
    static app_state app;
    static const app_action_desc actions[] = {
        {
            .id = "new_thread",
            .title = "New Thread",
            .icon = CUI_ICON_PLUS,
            .group = "Threads",
            .menu = APP_MENU_FILE,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_N },
            .on_trigger = mcode_app_action_new_thread,
        },
        {
            .id = "add_project",
            .title = "Add or Create Project...",
            .icon = CUI_ICON_FOLDER_OPEN,
            .group = "Projects",
            .menu = APP_MENU_FILE,
            .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_O },
            .on_trigger = mcode_app_action_add_project,
        },
        {
            .id = "open_folder",
            .title = "Open Folder in Finder",
            .icon = CUI_ICON_FOLDER_OPEN,
            .group = "Projects",
            .menu = APP_MENU_FILE,
            .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_F },
            .on_trigger = mcode_app_action_open_folder,
        },
        {
            .id = "project_actions",
            .title = "Project Quick Actions...",
            .icon = CUI_ICON_SLIDERS_HORIZONTAL,
            .group = "Projects",
            .menu = APP_MENU_FILE,
            .on_trigger = mcode_app_action_project_actions,
        },
        {
            .id = "commit",
            .title = "Commit Changes...",
            .icon = CUI_ICON_CHECK,
            .group = "Git",
            .menu = APP_MENU_EDIT,
            .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_C },
            .on_trigger = mcode_app_action_commit,
        },
        {
            .id = "create_pr",
            .title = "Push & Create Pull Request...",
            .icon = CUI_ICON_ARROW_UP_RIGHT,
            .group = "Git",
            .menu = APP_MENU_EDIT,
            .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P },
            .on_trigger = mcode_app_action_create_pr,
        },
        {
            .id = "refresh_diff",
            .title = "Refresh Diff",
            .icon = CUI_ICON_REFRESH_CW,
            .group = "Git",
            .menu = APP_MENU_VIEW,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_R },
            .on_trigger = mcode_app_action_refresh_diff,
        },
        {
            .id = "toggle_sidebar",
            .title = "Toggle Sidebar",
            .icon = CUI_ICON_PANEL_LEFT,
            .group = "View",
            .menu = APP_MENU_VIEW,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_B },
            .checked = true,
            .on_trigger = mcode_app_action_toggle_sidebar,
        },
        {
            .id = "toggle_diff",
            .title = "Toggle Diff Panel",
            .icon = CUI_ICON_LAYERS,
            .group = "View",
            .menu = APP_MENU_VIEW,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_D },
            .checked = true,
            .on_trigger = mcode_app_action_toggle_diff,
        },
        {
            .id = "toggle_terminal",
            .title = "Toggle Integrated Terminal",
            .icon = CUI_ICON_SQUARE_TERMINAL,
            .group = "View",
            .menu = APP_MENU_VIEW,
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_J },
            .on_trigger = mcode_app_action_toggle_terminal,
        },
    };

    int exit_code = 0;
    if (!app_chrome_init(&app.chrome, argc, argv, &exit_code))
        return exit_code;

    mcode_app_init(&app.mcode, &app.chrome);
    app_chrome_register_actions(&app.chrome, actions,
                                sizeof actions / sizeof actions[0], &app.mcode);
    app_chrome_set_palette_provider(&app.chrome, mcode_app_palette_provider, &app.mcode);

    cui_app_desc desc = app_chrome_app_desc(&app.chrome);
    int status = cui_app_run(&desc, frame, &app);
    mcode_app_shutdown(&app.mcode);
    app_chrome_shutdown(&app.chrome);
    return status;
}
