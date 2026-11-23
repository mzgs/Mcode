#include "chrome_internal.h"
#include <stdint.h>
#include <string.h>

enum {
    BUILTIN_NONE = -1,
    BUILTIN_OPEN_SETTINGS = 0,
    BUILTIN_APPEARANCE_SETTINGS,
    BUILTIN_KEYBINDINGS,
    BUILTIN_ABOUT,
    BUILTIN_THEME_SYSTEM,
    BUILTIN_THEME_LIGHT,
    BUILTIN_THEME_DARK,
    BUILTIN_TOGGLE_FULLSCREEN,
    BUILTIN_OPEN_SETTINGS_FOLDER,
    BUILTIN_OPEN_LOG_FILE,
    BUILTIN_CHECK_UPDATES,
};

typedef struct {
    app_action_fn on_trigger;
    void *user;
    int builtin_id;
} palette_dispatch;

struct app_palette_builder {
    cui_command commands[APP_MAX_PALETTE_COMMANDS];
    palette_dispatch dispatch[APP_MAX_PALETTE_COMMANDS];
    int count;
    int max_dynamic;
    char text_pool[8192];
    size_t text_used;
    void *default_user;
};

static const char *pool_strdup(app_palette_builder *builder, const char *str) {
    if (!str || !str[0]) return NULL;
    size_t len = strlen(str);
    if (builder->text_used + len + 1 > sizeof builder->text_pool) return NULL;
    char *dst = builder->text_pool + builder->text_used;
    memcpy(dst, str, len + 1);
    builder->text_used += len + 1;
    return dst;
}

static bool palette_push_raw(app_palette_builder *builder,
                             const char *title, const char *icon,
                             const char *group, const char *shortcut,
                             app_action_fn on_trigger, void *user,
                             int builtin_id) {
    int limit = (builtin_id == BUILTIN_NONE && builder && builder->max_dynamic > 0)
        ? builder->max_dynamic : APP_MAX_PALETTE_COMMANDS;
    if (!builder || !title || !title[0] || builder->count >= limit)
        return false;
    int idx = builder->count++;
    builder->commands[idx] = (cui_command){
        .title = title,
        .icon = (icon && icon[0]) ? icon : NULL,
        .group = (group && group[0]) ? group : "Actions",
        .shortcut = (shortcut && shortcut[0]) ? shortcut : NULL,
    };
    builder->dispatch[idx] = (palette_dispatch){
        .on_trigger = on_trigger,
        .user = user,
        .builtin_id = builtin_id,
    };
    return true;
}

bool app_palette_add(app_palette_builder *builder, const app_palette_command *cmd) {
    if (!builder || !cmd || !cmd->title || !cmd->title[0]) return false;
    int limit = builder->max_dynamic > 0 ? builder->max_dynamic : APP_MAX_PALETTE_COMMANDS;
    if (builder->count >= limit) return false;

    size_t saved_used = builder->text_used;
    const char *title = pool_strdup(builder, cmd->title);
    if (!title) return false;

    const char *icon = NULL;
    if (cmd->icon && cmd->icon[0]) {
        icon = pool_strdup(builder, cmd->icon);
        if (!icon) {
            builder->text_used = saved_used;
            return false;
        }
    }

    const char *group = "Actions";
    if (cmd->group && cmd->group[0]) {
        group = pool_strdup(builder, cmd->group);
        if (!group) {
            builder->text_used = saved_used;
            return false;
        }
    }

    const char *shortcut = NULL;
    if (cmd->shortcut && cmd->shortcut[0]) {
        shortcut = pool_strdup(builder, cmd->shortcut);
        if (!shortcut) {
            builder->text_used = saved_used;
            return false;
        }
    }

    void *user = cmd->user ? cmd->user : builder->default_user;
    return palette_push_raw(builder, title, icon, group, shortcut,
                            cmd->on_trigger, user, BUILTIN_NONE);
}

static bool groups_equal(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static void coalesce_groups(const app_palette_builder *builder,
                            cui_command *ordered_cmds,
                            palette_dispatch *ordered_dispatch) {
    bool emitted[APP_MAX_PALETTE_COMMANDS];
    memset(emitted, 0, sizeof emitted);
    int out = 0;
    for (int i = 0; i < builder->count; ++i) {
        if (emitted[i]) continue;
        const char *group = builder->commands[i].group;
        for (int j = i; j < builder->count; ++j) {
            if (emitted[j] || !groups_equal(builder->commands[j].group, group)) continue;
            emitted[j] = true;
            ordered_cmds[out] = builder->commands[j];
            if (ordered_dispatch) ordered_dispatch[out] = builder->dispatch[j];
            ++out;
        }
    }
}

static void populate_palette(app_chrome *chrome, cui_ctx *ui, app_palette_builder *builder) {
    memset(builder, 0, sizeof *builder);
    int builtin_count = 10 + (app_updater_configured() ? 1 : 0);
    builder->max_dynamic = APP_MAX_PALETTE_COMMANDS - builtin_count;

    /* 1. App-registered actions */
    for (int i = 0; i < chrome->action_count; ++i) {
        const app_registered_action *action = &chrome->actions[i];
        if (action->hide_from_palette || !action->enabled) continue;
        cui_key_combo combo = chrome_action_combo(chrome, action);
        const char *shortcut = (ui && combo.key != CUI_KEY_NONE)
            ? cui_shortcut_label(ui, combo) : NULL;
        palette_push_raw(builder, action->title, action->icon, action->group, shortcut,
                         action->on_trigger, action->user, BUILTIN_NONE);
    }

    /* 2. Dynamic provider commands */
    if (chrome->palette_provider) {
        builder->default_user = chrome->palette_provider_user;
        chrome->palette_provider(builder, chrome->palette_provider_user);
        builder->default_user = NULL;
    }

    /* 3. Built-in chrome commands */
    const char *settings_shortcut = ui
        ? cui_shortcut_label(ui, (cui_key_combo){ CUI_MOD_PRIMARY, CUI_KEY_COMMA })
        : NULL;

    palette_push_raw(builder, "Open Settings", chrome_section_icons[SETTINGS_SECTION_GENERAL],
                     "Navigation", settings_shortcut, NULL, NULL, BUILTIN_OPEN_SETTINGS);
    palette_push_raw(builder, "Appearance Settings", chrome_section_icons[SETTINGS_SECTION_APPEARANCE],
                     "Navigation", NULL, NULL, NULL, BUILTIN_APPEARANCE_SETTINGS);
    palette_push_raw(builder, "Keybindings", chrome_section_icons[SETTINGS_SECTION_KEYBINDINGS],
                     "Navigation", NULL, NULL, NULL, BUILTIN_KEYBINDINGS);
    palette_push_raw(builder, "About " APP_NAME, chrome_section_icons[SETTINGS_SECTION_ABOUT],
                     "Navigation", NULL, NULL, NULL, BUILTIN_ABOUT);

    palette_push_raw(builder, "Theme: System", chrome_theme_icons[APP_THEME_SYSTEM],
                     "Appearance", NULL, NULL, NULL, BUILTIN_THEME_SYSTEM);
    palette_push_raw(builder, "Theme: Light", chrome_theme_icons[APP_THEME_LIGHT],
                     "Appearance", NULL, NULL, NULL, BUILTIN_THEME_LIGHT);
    palette_push_raw(builder, "Theme: Dark", chrome_theme_icons[APP_THEME_DARK],
                     "Appearance", NULL, NULL, NULL, BUILTIN_THEME_DARK);
    palette_push_raw(builder, "Toggle Fullscreen", CUI_ICON_MAXIMIZE_2,
                     "Appearance", NULL, NULL, NULL, BUILTIN_TOGGLE_FULLSCREEN);

    palette_push_raw(builder, "Open Settings Folder", CUI_ICON_FOLDER_OPEN,
                     "Actions", NULL, NULL, NULL, BUILTIN_OPEN_SETTINGS_FOLDER);
    palette_push_raw(builder, "Open Log File", CUI_ICON_FILE_TEXT,
                     "Actions", NULL, NULL, NULL, BUILTIN_OPEN_LOG_FILE);
    if (app_updater_configured()) {
        palette_push_raw(builder, "Check for Updates", CUI_ICON_REFRESH_CW,
                         "Actions", NULL, NULL, NULL, BUILTIN_CHECK_UPDATES);
    }
}

static const char *rebase_pool_ptr(const char *ptr, const char *src_base, size_t src_len,
                                   const char *dst_base) {
    if (!ptr) return NULL;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)src_base;
    if (p >= base && p - base < src_len) {
        return dst_base + (size_t)(p - base);
    }
    return ptr;
}

void chrome_collect_palette_snapshot(app_chrome *chrome, cui_ctx *ui, chrome_palette_snapshot *out) {
    if (!chrome || !out) return;
    memset(out, 0, sizeof *out);
    app_palette_builder builder;
    populate_palette(chrome, ui, &builder);
    memcpy(out->text_pool, builder.text_pool, builder.text_used);
    out->text_used = builder.text_used;
    out->count = builder.count;
    coalesce_groups(&builder, out->commands, NULL);
    for (int i = 0; i < out->count; ++i) {
        out->commands[i].title = rebase_pool_ptr(out->commands[i].title,
                                                 builder.text_pool, builder.text_used, out->text_pool);
        out->commands[i].icon = rebase_pool_ptr(out->commands[i].icon,
                                                builder.text_pool, builder.text_used, out->text_pool);
        out->commands[i].group = rebase_pool_ptr(out->commands[i].group,
                                                 builder.text_pool, builder.text_used, out->text_pool);
        out->commands[i].shortcut = rebase_pool_ptr(out->commands[i].shortcut,
                                                    builder.text_pool, builder.text_used, out->text_pool);
    }
}

static void dispatch_builtin(app_chrome *chrome, cui_ctx *ui, int builtin_id) {
    switch (builtin_id) {
    case BUILTIN_OPEN_SETTINGS:
        chrome->settings_open = true;
        break;
    case BUILTIN_APPEARANCE_SETTINGS:
        chrome->settings_section = SETTINGS_SECTION_APPEARANCE;
        chrome->settings_open = true;
        break;
    case BUILTIN_KEYBINDINGS:
        chrome->settings_section = SETTINGS_SECTION_KEYBINDINGS;
        chrome->settings_open = true;
        break;
    case BUILTIN_ABOUT:
        chrome->settings_section = SETTINGS_SECTION_ABOUT;
        chrome->settings_open = true;
        break;
    case BUILTIN_THEME_SYSTEM:
    case BUILTIN_THEME_LIGHT:
    case BUILTIN_THEME_DARK: {
        static const app_theme_mode modes[] = { APP_THEME_SYSTEM, APP_THEME_LIGHT, APP_THEME_DARK };
        static const char *const messages[] = {
            "Theme set to System.", "Theme set to Light.", "Theme set to Dark."
        };
        int idx = builtin_id - BUILTIN_THEME_SYSTEM;
        chrome->settings.appearance.theme_mode = modes[idx];
        chrome->env_theme_override = false;
        chrome->has_cached_system_dark = false;
        chrome_apply_appearance(chrome, ui);
        if (chrome_save_settings(chrome, ui))
            app_chrome_notify(chrome, ui, messages[idx], CUI_TONE_INFO);
        break;
    }
    case BUILTIN_TOGGLE_FULLSCREEN:
        chrome_toggle_fullscreen();
        break;
    case BUILTIN_OPEN_SETTINGS_FOLDER:
        chrome_open_settings_folder(chrome, ui);
        break;
    case BUILTIN_OPEN_LOG_FILE:
        chrome_open_log_file(chrome, ui);
        break;
    case BUILTIN_CHECK_UPDATES:
        chrome->settings_section = SETTINGS_SECTION_ABOUT;
        chrome->settings_open = true;
        chrome_check_for_updates(chrome, false);
        break;
    default:
        break;
    }
}

void chrome_draw_command_palette(app_chrome *chrome, cui_ctx *ui) {
    if (!chrome->palette_open) {
        (void)cui_command_palette(ui, &chrome->palette_open, NULL, 0);
        return;
    }

    app_palette_builder builder;
    populate_palette(chrome, ui, &builder);

    cui_command ordered_cmds[APP_MAX_PALETTE_COMMANDS];
    palette_dispatch ordered_dispatch[APP_MAX_PALETTE_COMMANDS];
    coalesce_groups(&builder, ordered_cmds, ordered_dispatch);

    int chosen = cui_command_palette(ui, &chrome->palette_open, ordered_cmds, builder.count);
    if (chosen < 0 || chosen >= builder.count) return;

    palette_dispatch picked = ordered_dispatch[chosen];
    if (picked.builtin_id != BUILTIN_NONE) {
        dispatch_builtin(chrome, ui, picked.builtin_id);
    } else if (picked.on_trigger) {
        picked.on_trigger(ui, picked.user);
    }
}
