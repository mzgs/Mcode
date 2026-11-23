#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "chrome_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define test_rmdir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

typedef struct {
    int save_calls;
    int find_calls;
    int custom_calls;
    void *last_user;
} test_state;

static void on_save(cui_ctx *ui, void *user) {
    (void)ui;
    test_state *st = user;
    st->save_calls++;
    st->last_user = user;
}

static void on_find(cui_ctx *ui, void *user) {
    (void)ui;
    test_state *st = user;
    st->find_calls++;
    st->last_user = user;
}

static void on_custom(cui_ctx *ui, void *user) {
    (void)ui;
    test_state *st = user;
    st->custom_calls++;
    st->last_user = user;
}

static void temp_settings_path(const char *tag, char *dir_out, size_t dir_cap,
                               char *file_out, size_t file_cap) {
    const char *dir = getenv("TMPDIR");
#ifdef _WIN32
    if (!dir || !dir[0]) dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(dir_out, dir_cap, "%s\\c_gui_actions_test_%s_%d", dir, tag, (int)getpid());
    (void)_mkdir(dir_out);
    snprintf(file_out, file_cap, "%s\\settings.json", dir_out);
#else
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(dir_out, dir_cap, "%s/c_gui_actions_test_%s_%d", dir, tag, (int)getpid());
    (void)mkdir(dir_out, 0700);
    snprintf(file_out, file_cap, "%s/settings.json", dir_out);
#endif
    remove(file_out);
}

static void cleanup_test_dir(const char *dir_path, const char *settings_path) {
    remove(settings_path);
    char log_path[1088];
    char log_bak[1088];
#ifdef _WIN32
    snprintf(log_path, sizeof log_path, "%s\\app.log", dir_path);
    snprintf(log_bak, sizeof log_bak, "%s\\app.log.1", dir_path);
#else
    snprintf(log_path, sizeof log_path, "%s/app.log", dir_path);
    snprintf(log_bak, sizeof log_bak, "%s/app.log.1", dir_path);
#endif
    remove(log_path);
    remove(log_bak);
    test_rmdir(dir_path);
}

static bool write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(content);
    bool ok = fwrite(content, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool read_text_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

static void init_test_chrome(app_chrome *chrome, const char *settings_path) {
    char arg0[] = "actions_tests";
    char arg1[] = "--settings";
    char arg2[1024];
    snprintf(arg2, sizeof arg2, "%s", settings_path);
    char *argv[] = { arg0, arg1, arg2 };
    int exit_code = 0;
    CHECK(app_chrome_init(chrome, 3, argv, &exit_code));
}

static void test_registration_and_trigger(void) {
    char dir[1024], path[1088];
    temp_settings_path("reg", dir, sizeof dir, path, sizeof path);

    app_chrome chrome;
    init_test_chrome(&chrome, path);

    test_state default_st = { 0 };
    test_state override_st = { 0 };

    /* Invalid registrations */
    CHECK(!app_chrome_register_action(&chrome, NULL));
    CHECK(!app_chrome_register_action(&chrome, &(app_action_desc){ .id = "empty_title" }));
    CHECK(!app_chrome_register_action(&chrome, &(app_action_desc){
        .id = APP_KEYBINDING_COMMAND_PALETTE,
        .title = "Reserved",
    }));

    const app_action_desc batch[] = {
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
            .user = &override_st,
        },
        {
            .title = "Palette Only",
            .group = "Tools",
            .on_trigger = on_custom,
        },
        {
            .id = "hidden_kb",
            .title = "No Keybinding Row",
            .hide_from_keybindings = true,
            .on_trigger = on_custom,
        },
    };
    CHECK(app_chrome_register_actions(&chrome, batch, sizeof batch / sizeof batch[0], &default_st) == 4);

    /* Duplicate ID rejected */
    CHECK(!app_chrome_register_action(&chrome, &(app_action_desc){
        .id = "save",
        .title = "Save Again",
    }));

    /* Keybindings table contains command_palette + save + find (not Palette Only or hidden_kb) */
    CHECK(chrome.settings.keybinding_count == 3);
    CHECK(app_settings_find_keybinding(&chrome.settings, APP_KEYBINDING_COMMAND_PALETTE) == 0);
    CHECK(app_settings_find_keybinding(&chrome.settings, "save") == 1);
    CHECK(app_settings_find_keybinding(&chrome.settings, "find") == 2);
    CHECK(app_settings_find_keybinding(&chrome.settings, "hidden_kb") == -1);

    /* Trigger & user pointer routing */
    CHECK(app_chrome_trigger_action(&chrome, NULL, "save"));
    CHECK(default_st.save_calls == 1);
    CHECK(default_st.last_user == &default_st);

    CHECK(app_chrome_trigger_action(&chrome, NULL, "find"));
    CHECK(override_st.find_calls == 1);
    CHECK(override_st.last_user == &override_st);

    CHECK(app_chrome_trigger_action(&chrome, NULL, "hidden_kb"));
    CHECK(default_st.custom_calls == 1);

    /* Disable / enable & checked */
    CHECK(app_chrome_set_action_enabled(&chrome, "save", false));
    CHECK(!app_chrome_trigger_action(&chrome, NULL, "save"));
    CHECK(default_st.save_calls == 1);
    CHECK(app_chrome_set_action_enabled(&chrome, "save", true));
    CHECK(app_chrome_trigger_action(&chrome, NULL, "save"));
    CHECK(default_st.save_calls == 2);

    CHECK(app_chrome_set_action_checked(&chrome, "find", true));
    CHECK(chrome.actions[1].checked);
    CHECK(!app_chrome_set_action_checked(&chrome, "nonexistent", true));
    CHECK(!app_chrome_set_action_enabled(&chrome, "nonexistent", false));

    /* Built-in command_palette trigger */
    CHECK(!chrome.palette_open);
    CHECK(app_chrome_trigger_action(&chrome, NULL, APP_KEYBINDING_COMMAND_PALETTE));
    CHECK(chrome.palette_open);

    app_chrome_shutdown(&chrome);
    cleanup_test_dir(dir, path);
}

static void test_deferred_keybindings_persistence_and_reset(void) {
    char dir[1024], path[1088];
    temp_settings_path("persist", dir, sizeof dir, path, sizeof path);

    const char *json =
        "{\n"
        "  \"keybindings\": {\n"
        "    \"save\": { \"key\": \"E\", \"mods\": \"ctrl shift\" },\n"
        "    \"find\": { \"key\": \"\", \"mods\": \"\" },\n"
        "    \"obsolete_action\": { \"key\": \"Z\", \"mods\": \"ctrl\" }\n"
        "  }\n"
        "}\n";
    CHECK(write_text_file(path, json));

    app_chrome chrome;
    init_test_chrome(&chrome, path);
    CHECK(chrome.settings.deferred_keybinding_count == 3);

    const app_action_desc actions[] = {
        {
            .id = "save",
            .title = "Save",
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_S },
        },
        {
            .id = "find",
            .title = "Find",
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_F },
        },
    };
    CHECK(app_chrome_register_actions(&chrome, actions, 2, NULL) == 2);

    /* Deferred combos applied on registration */
    cui_key_combo save_combo = chrome_action_combo(&chrome, &chrome.actions[0]);
    cui_key_combo find_combo = chrome_action_combo(&chrome, &chrome.actions[1]);
    CHECK(save_combo.key == CUI_KEY_E);
    CHECK(save_combo.mods == (CUI_MOD_CTRL | CUI_MOD_SHIFT));
    CHECK(find_combo.key == CUI_KEY_NONE);
    CHECK(find_combo.mods == 0);

    /* Conflict detection */
    int save_kb = app_settings_find_keybinding(&chrome.settings, "save");
    int find_kb = app_settings_find_keybinding(&chrome.settings, "find");
    CHECK(app_keybinding_conflict(&chrome.settings, save_kb) == APP_CONFLICT_NONE);
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_NONE);

    chrome.settings.keybindings[find_kb].combo = save_combo;
    CHECK(app_keybinding_conflict(&chrome.settings, save_kb) == find_kb);
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == save_kb);

    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_COMMA };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
#ifdef __APPLE__
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY | CUI_MOD_CTRL, CUI_KEY_F };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_Q };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_W };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
#else
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_Q };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ 0, CUI_KEY_F11 };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_SETTINGS);
#endif

    /* Hidden-from-keybindings actions with fixed combos flag conflicts via chrome_keybinding_conflict */
    CHECK(app_chrome_register_action(&chrome, &(app_action_desc){
        .id = "fixed_hidden",
        .title = "Fixed Hidden",
        .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P },
        .hide_from_keybindings = true,
    }));
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P };
    CHECK(app_keybinding_conflict(&chrome.settings, find_kb) == APP_CONFLICT_NONE);
    CHECK(chrome_keybinding_conflict(&chrome, find_kb) == APP_CONFLICT_SETTINGS);

    /* Restore find to cleared and save to disk */
    chrome.settings.keybindings[find_kb].combo = (cui_key_combo){ 0, CUI_KEY_NONE };
    CHECK(app_settings_save(&chrome.settings, path));

    char saved[4096];
    CHECK(read_text_file(path, saved, sizeof saved));
    CHECK(strstr(saved, "\"save\"") != NULL);
    CHECK(strstr(saved, "\"find\"") != NULL);
    CHECK(strstr(saved, "\"obsolete_action\"") == NULL);

    /* Reset settings preserves registered actions and restores default combos */
    chrome_reset_settings(&chrome);
    CHECK(chrome.settings.keybinding_count == 3);
    save_combo = chrome_action_combo(&chrome, &chrome.actions[0]);
    find_combo = chrome_action_combo(&chrome, &chrome.actions[1]);
    CHECK(save_combo.key == CUI_KEY_S && save_combo.mods == CUI_MOD_PRIMARY);
    CHECK(find_combo.key == CUI_KEY_F && find_combo.mods == CUI_MOD_PRIMARY);

    app_chrome_shutdown(&chrome);
    cleanup_test_dir(dir, path);
}

static void dynamic_provider(app_palette_builder *builder, void *user) {
    (void)user;
    for (int i = 0; i < 2; ++i) {
        char label[64];
        snprintf(label, sizeof label, "Recent File %d", i + 1);
        CHECK(app_palette_add(builder, &(app_palette_command){
            .title = label,
            .icon = CUI_ICON_FILE_TEXT,
            .group = "Recent Files",
            .shortcut = "Alt R",
            .on_trigger = on_custom,
        }));
    }
    /* Add a command targeting an existing built-in group to verify coalescing */
    CHECK(app_palette_add(builder, &(app_palette_command){
        .title = "Go to Dashboard",
        .group = "Navigation",
        .on_trigger = on_custom,
    }));
}

static void flooding_provider(app_palette_builder *builder, void *user) {
    (void)user;
    for (int i = 0; i < 200; ++i) {
        char label[32];
        snprintf(label, sizeof label, "Item %d", i);
        (void)app_palette_add(builder, &(app_palette_command){
            .title = label,
            .group = "Dynamic",
            .on_trigger = on_custom,
        });
    }
}

static void test_palette_provider_and_group_coalescing(void) {
    char dir[1024], path[1088];
    temp_settings_path("palette", dir, sizeof dir, path, sizeof path);

    app_chrome chrome;
    init_test_chrome(&chrome, path);

    const app_action_desc actions[] = {
        { .id = "save", .title = "Save", .group = "Actions" },
        { .id = "hidden", .title = "Hidden Action", .hide_from_palette = true },
        { .id = "disabled", .title = "Disabled Action", .disabled = true },
        { .id = "custom_group", .title = "Export PDF", .group = "Document" },
    };
    CHECK(app_chrome_register_actions(&chrome, actions, 4, NULL) == 4);
    app_chrome_set_palette_provider(&chrome, dynamic_provider, NULL);

    chrome_palette_snapshot snap;
    chrome_collect_palette_snapshot(&chrome, NULL, &snap);

    bool saw_save = false, saw_hidden = false, saw_disabled = false;
    bool saw_recent1 = false, saw_recent2 = false, saw_dashboard = false, saw_export = false;
    for (int i = 0; i < snap.count; ++i) {
        const char *t = snap.commands[i].title;
        if (strcmp(t, "Save") == 0) saw_save = true;
        if (strcmp(t, "Hidden Action") == 0) saw_hidden = true;
        if (strcmp(t, "Disabled Action") == 0) saw_disabled = true;
        if (strcmp(t, "Recent File 1") == 0) saw_recent1 = true;
        if (strcmp(t, "Recent File 2") == 0) saw_recent2 = true;
        if (strcmp(t, "Go to Dashboard") == 0) saw_dashboard = true;
        if (strcmp(t, "Export PDF") == 0) saw_export = true;
    }
    CHECK(saw_save);
    CHECK(!saw_hidden);
    CHECK(!saw_disabled);
    CHECK(saw_recent1);
    CHECK(saw_recent2);
    CHECK(saw_dashboard);
    CHECK(saw_export);

    /* Verify every group name forms a single contiguous run (no split group headers) */
    for (int i = 0; i < snap.count; ++i) {
        const char *g = snap.commands[i].group;
        bool left_run = false;
        for (int j = i + 1; j < snap.count; ++j) {
            bool same = (strcmp(snap.commands[j].group, g) == 0);
            if (!same) left_run = true;
            else CHECK(!left_run);
        }
    }

    /* Verify a flooding dynamic provider cannot crowd out built-in chrome commands */
    app_chrome_set_palette_provider(&chrome, flooding_provider, NULL);
    chrome_collect_palette_snapshot(&chrome, NULL, &snap);
    CHECK(snap.count == APP_MAX_PALETTE_COMMANDS);
    bool saw_open_settings = false, saw_theme_dark = false, saw_fullscreen = false, saw_open_log = false;
    for (int i = 0; i < snap.count; ++i) {
        const char *t = snap.commands[i].title;
        if (strcmp(t, "Open Settings") == 0) saw_open_settings = true;
        if (strcmp(t, "Theme: Dark") == 0) saw_theme_dark = true;
        if (strcmp(t, "Toggle Fullscreen") == 0) saw_fullscreen = true;
        if (strcmp(t, "Open Log File") == 0) saw_open_log = true;
    }
    CHECK(saw_open_settings);
    CHECK(saw_theme_dark);
    CHECK(saw_fullscreen);
    CHECK(saw_open_log);

    app_chrome_shutdown(&chrome);
    cleanup_test_dir(dir, path);
}

static void test_shortcuts_and_text_focus(void) {
    char dir[1024], path[1088];
    temp_settings_path("shortcuts", dir, sizeof dir, path, sizeof path);

    app_chrome chrome;
    init_test_chrome(&chrome, path);

    test_state st = { 0 };
    const app_action_desc actions[] = {
        {
            .id = "save",
            .title = "Save",
            .default_combo = { CUI_MOD_PRIMARY, CUI_KEY_S },
            .on_trigger = on_save,
        },
        {
            .id = "clash_fs",
            .title = "Clash Fullscreen",
#ifdef __APPLE__
            .default_combo = { CUI_MOD_PRIMARY | CUI_MOD_CTRL, CUI_KEY_F },
#else
            .default_combo = { 0, CUI_KEY_F11 },
#endif
            .on_trigger = on_custom,
        },
    };
    CHECK(app_chrome_register_actions(&chrome, actions, 2, &st) == 2);

    cui_ctx *ui = cui_create(NULL);
    CHECK(ui != NULL);
    cui_set_viewport(ui, 800, 600, 1.0f);
    cui_set_reduced_motion(ui, true);

    char text_buf[64] = "";
    cui_id input_id = 0;

    /* Layout frame to discover the input rect. */
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    input_id = cui_make_id(ui, "test-input");
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);

    /* Unfocused: Save shortcut triggers normally. */
    cui_input_key(ui, CUI_KEY_S, true, CUI_MOD_PRIMARY);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_S, false, 0);
    CHECK(st.save_calls == 1);

    /* Built-in fullscreen shortcut must not double-fire a clashing action. */
#ifdef __APPLE__
    cui_input_key(ui, CUI_KEY_F, true, CUI_MOD_PRIMARY | CUI_MOD_CTRL);
#else
    cui_input_key(ui, CUI_KEY_F11, true, 0);
#endif
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
#ifdef __APPLE__
    cui_input_key(ui, CUI_KEY_F, false, 0);
#else
    cui_input_key(ui, CUI_KEY_F11, false, 0);
#endif
    CHECK(st.custom_calls == 0);

    /* Click to focus the text input. */
    cui_rect r = cui_get_rect(ui, input_id);
    cui_input_mouse_move(ui, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);

    /* Focused text input: global Save and Command Palette shortcuts must be suppressed. */
    cui_input_key(ui, CUI_KEY_S, true, CUI_MOD_PRIMARY);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_S, false, 0);
    CHECK(st.save_calls == 1);

    cui_input_key(ui, CUI_KEY_K, true, CUI_MOD_PRIMARY);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_K, false, 0);
    CHECK(!chrome.palette_open);

    /* Reserved chrome shortcuts still work (and don't leak to actions) while a text input is focused. */
#ifdef __APPLE__
    cui_input_key(ui, CUI_KEY_F, true, CUI_MOD_PRIMARY | CUI_MOD_CTRL);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_F, false, 0);
    CHECK(st.custom_calls == 0);
#else
    cui_input_key(ui, CUI_KEY_COMMA, true, CUI_MOD_PRIMARY);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    cui_input(ui, "test-input", text_buf, sizeof text_buf);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_COMMA, false, 0);
    CHECK(chrome.settings_open);
    chrome.settings_open = false;
#endif

    cui_destroy(ui);
    app_chrome_shutdown(&chrome);
    cleanup_test_dir(dir, path);
}

static void test_palette_shortcut_no_input_leak(void) {
    char dir[1024], path[1088];
    temp_settings_path("palette_leak", dir, sizeof dir, path, sizeof path);

    app_chrome chrome;
    init_test_chrome(&chrome, path);

    test_state st = { 0 };
    CHECK(app_chrome_register_action(&chrome, &(app_action_desc){
        .id = "first_act",
        .title = "First Action",
        .on_trigger = on_save,
        .user = &st,
    }));

    cui_ctx *ui = cui_create(NULL);
    CHECK(ui != NULL);
    cui_set_viewport(ui, 800, 600, 1.0f);
    cui_set_reduced_motion(ui, true);

    /* Warmup frame. */
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);

    /* 1. Enter binding must open palette without firing the first command. */
    chrome.settings.keybindings[chrome.command_palette_kb].combo = (cui_key_combo){ 0, CUI_KEY_ENTER };
    cui_input_key(ui, CUI_KEY_ENTER, true, 0);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    CHECK(chrome.palette_open);
    CHECK(st.save_calls == 0);
    /* Next frame renders the palette cleanly and leaves it open. */
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    CHECK(chrome.palette_open);
    CHECK(st.save_calls == 0);

    /* 2. Shift+Escape binding must open palette without immediately closing it. */
    chrome.palette_open = false;
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);

    chrome.settings.keybindings[chrome.command_palette_kb].combo =
        (cui_key_combo){ CUI_MOD_SHIFT, CUI_KEY_ESCAPE };
    cui_input_key(ui, CUI_KEY_ESCAPE, true, CUI_MOD_SHIFT);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    CHECK(chrome.palette_open);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    CHECK(chrome.palette_open);

    /* 3. Printable '/' binding must not leak '/' into the palette search input. */
    chrome.palette_open = false;
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);

    chrome.settings.keybindings[chrome.command_palette_kb].combo = (cui_key_combo){ 0, CUI_KEY_SLASH };
    cui_input_key(ui, CUI_KEY_SLASH, true, 0);
    cui_input_text(ui, "/");
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_SLASH, false, 0);
    CHECK(chrome.palette_open);

    /* Render the palette, then press Enter: if query is empty, "First Action" is selected. */
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_ENTER, true, 0);
    cui_begin_frame(ui, 1.0f / 60.0f);
    app_chrome_begin_frame(&chrome, ui);
    app_chrome_end_frame(&chrome, ui);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    CHECK(!chrome.palette_open);
    CHECK(st.save_calls == 1);

    cui_destroy(ui);
    app_chrome_shutdown(&chrome);
    cleanup_test_dir(dir, path);
}

static void test_settings_dir_rejects_file_and_env_path(void) {
    char dir[1024], path[1088];
    temp_settings_path("dircheck", dir, sizeof dir, path, sizeof path);

    int exit_code = 0;
    CHECK(!app_chrome_init(NULL, 0, NULL, &exit_code));
    CHECK(exit_code == 1);

    char blocker[1088];
    char nested[1152];
#ifdef _WIN32
    snprintf(blocker, sizeof blocker, "%s\\not_a_dir", dir);
    snprintf(nested, sizeof nested, "%s\\settings.json", blocker);
#else
    snprintf(blocker, sizeof blocker, "%s/not_a_dir", dir);
    snprintf(nested, sizeof nested, "%s/settings.json", blocker);
#endif
    CHECK(write_text_file(blocker, "file"));
    char out_dir[1088];
    CHECK(!app_settings_dir(out_dir, sizeof out_dir, nested, true));
    remove(blocker);

#ifndef _WIN32
    /* APP_SETTINGS_PATH is used when --settings is not provided on the CLI. */
    CHECK(setenv("APP_SETTINGS_PATH", path, 1) == 0);
    CHECK(setenv("CUI_THEME", "invalid", 1) == 0);
    CHECK(setenv("CUI_ACCENT", "invalid", 1) == 0);
    app_chrome chrome;
    char arg0[] = "actions_tests";
    char *argv[] = { arg0 };
    exit_code = 0;
    CHECK(app_chrome_init(&chrome, 1, argv, &exit_code));
    CHECK(chrome.cli.settings_path != NULL);
    CHECK(strcmp(chrome.cli.settings_path, path) == 0);
    CHECK(!chrome.env_theme_override);
    CHECK(!chrome.env_accent_override);
    app_chrome_shutdown(&chrome);

    CHECK(setenv("CUI_THEME", "dark", 1) == 0);
    CHECK(setenv("CUI_ACCENT", "blue", 1) == 0);
    CHECK(app_chrome_init(&chrome, 1, argv, &exit_code));
    CHECK(chrome.env_theme_override);
    CHECK(chrome.env_accent_override);
    app_chrome_shutdown(&chrome);

    unsetenv("CUI_THEME");
    unsetenv("CUI_ACCENT");
    unsetenv("APP_SETTINGS_PATH");
#endif

    cleanup_test_dir(dir, path);
}

int main(void) {
    test_registration_and_trigger();
    test_deferred_keybindings_persistence_and_reset();
    test_palette_provider_and_group_coalescing();
    test_shortcuts_and_text_focus();
    test_palette_shortcut_no_input_leak();
    test_settings_dir_rejects_file_and_env_path();
    if (failures > 0) {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    puts("All actions tests passed.");
    return 0;
}
