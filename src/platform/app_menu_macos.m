#import <Cocoa/Cocoa.h>
#include <cui/cui_app.h>
#include <string.h>
#include "app_config.h"
#include "app_menu.h"

static bool combo_equal(cui_key_combo a, cui_key_combo b) {
    return a.key == b.key && a.mods == b.mods;
}

typedef struct {
    int action_index;
    app_menu_group menu;
    char title[APP_KEYBINDING_LABEL_MAX];
    cui_key_combo combo;
    bool enabled;
    bool checked;
} cached_menu_action;

static NSString *macos_key_equivalent(cui_key key, bool *is_function_key) {
    *is_function_key = false;
    if (key >= CUI_KEY_A && key <= CUI_KEY_Z) {
        unichar ch = (unichar)('a' + (key - CUI_KEY_A));
        return [NSString stringWithCharacters:&ch length:1];
    }
    if (key >= CUI_KEY_0 && key <= CUI_KEY_9) {
        unichar ch = (unichar)('0' + (key - CUI_KEY_0));
        return [NSString stringWithCharacters:&ch length:1];
    }
    if (key >= CUI_KEY_F1 && key <= CUI_KEY_F12) {
        *is_function_key = true;
        unichar ch = (unichar)(NSF1FunctionKey + (key - CUI_KEY_F1));
        return [NSString stringWithCharacters:&ch length:1];
    }
    unichar ch = 0;
    switch (key) {
    case CUI_KEY_TAB: ch = '\t'; break;
    case CUI_KEY_ENTER: ch = '\r'; break;
    case CUI_KEY_ESCAPE: ch = 0x1b; break;
    case CUI_KEY_BACKSPACE: ch = NSBackspaceCharacter; break;
    case CUI_KEY_DELETE: ch = NSDeleteFunctionKey; *is_function_key = true; break;
    case CUI_KEY_SPACE: ch = ' '; break;
    case CUI_KEY_LEFT: ch = NSLeftArrowFunctionKey; *is_function_key = true; break;
    case CUI_KEY_RIGHT: ch = NSRightArrowFunctionKey; *is_function_key = true; break;
    case CUI_KEY_UP: ch = NSUpArrowFunctionKey; *is_function_key = true; break;
    case CUI_KEY_DOWN: ch = NSDownArrowFunctionKey; *is_function_key = true; break;
    case CUI_KEY_HOME: ch = NSHomeFunctionKey; *is_function_key = true; break;
    case CUI_KEY_END: ch = NSEndFunctionKey; *is_function_key = true; break;
    case CUI_KEY_PAGE_UP: ch = NSPageUpFunctionKey; *is_function_key = true; break;
    case CUI_KEY_PAGE_DOWN: ch = NSPageDownFunctionKey; *is_function_key = true; break;
    case CUI_KEY_MINUS: ch = '-'; break;
    case CUI_KEY_EQUAL: ch = '='; break;
    case CUI_KEY_COMMA: ch = ','; break;
    case CUI_KEY_PERIOD: ch = '.'; break;
    case CUI_KEY_SLASH: ch = '/'; break;
    default: return @"";
    }
    return [NSString stringWithCharacters:&ch length:1];
}

static void macos_apply_key_combo(NSMenuItem *item, cui_key_combo combo) {
    if (!item) return;
    bool is_fn = false;
    NSString *equiv = combo.key != CUI_KEY_NONE ? macos_key_equivalent(combo.key, &is_fn) : @"";
    if (!equiv.length ||
        (!is_fn && !(combo.mods & (CUI_MOD_SUPER | CUI_MOD_CTRL | CUI_MOD_ALT)))) {
        item.keyEquivalent = @"";
        item.keyEquivalentModifierMask = 0;
        return;
    }
    NSEventModifierFlags mask = 0;
    if (combo.mods & CUI_MOD_SUPER) mask |= NSEventModifierFlagCommand;
    if (combo.mods & CUI_MOD_CTRL) mask |= NSEventModifierFlagControl;
    if (combo.mods & CUI_MOD_ALT) mask |= NSEventModifierFlagOption;
    if (combo.mods & CUI_MOD_SHIFT) mask |= NSEventModifierFlagShift;
    item.keyEquivalent = equiv;
    item.keyEquivalentModifierMask = mask;
}

static void sync_combo_item(NSMenuItem *item, cui_key_combo prev, cui_key_combo next, bool force) {
    if (force || !combo_equal(prev, next)) macos_apply_key_combo(item, next);
}

@interface AppMenuTarget : NSObject <NSMenuItemValidation> {
@public
    cached_menu_action _cachedActions[APP_MAX_ACTIONS];
    int _cachedActionCount;
}
@property(nonatomic, assign) bool *settingsOpen;
@property(nonatomic, assign) int *settingsSection;
@property(nonatomic, assign) int aboutSection;
@property(nonatomic, assign) bool *paletteOpen;
@property(nonatomic, assign) void *user;
@property(nonatomic, assign) void (*onAction)(int action_index, void *user);
@property(nonatomic, assign) void (*onEditKey)(cui_key key, void *user);
@property(nonatomic, assign) void (*onToggleFullscreen)(void *user);
@property(nonatomic, assign) void (*onWillTerminate)(void *user);
@property(nonatomic, strong) NSMenuItem *fileMenuItem;
@property(nonatomic, strong) NSMenu *fileMenu;
@property(nonatomic, strong) NSMenu *editMenu;
@property(nonatomic, strong) NSMenu *viewMenu;
@property(nonatomic, assign) NSInteger editStaticCount;
@property(nonatomic, assign) NSInteger viewStaticCount;
@property(nonatomic, strong) NSMutableArray<NSMenuItem *> *dynamicItems;
@property(nonatomic, strong) NSMenuItem *paletteItem;
@property(nonatomic, strong) NSMenuItem *fullscreenItem;
@property(nonatomic, assign) bool hasSyncedState;
@property(nonatomic, assign) cui_key_combo lastPaletteCombo;
@property(nonatomic, assign) bool lastFullscreen;
@property(nonatomic, strong) id keyMonitor;
- (void)showAbout:(id)sender;
- (void)showSettings:(id)sender;
- (void)triggerAction:(NSMenuItem *)sender;
- (void)triggerPalette:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)triggerEditKey:(NSMenuItem *)sender;
- (void)quitApp:(id)sender;
- (void)clearHooks;
@end

@implementation AppMenuTarget
- (instancetype)init {
    self = [super init];
    if (self) {
        _dynamicItems = [NSMutableArray array];
    }
    return self;
}

- (void)applyHooks:(const app_menu_hooks *)hooks {
    self.settingsOpen = hooks->settings_open;
    self.settingsSection = hooks->settings_section;
    self.aboutSection = hooks->about_section;
    self.paletteOpen = hooks->palette_open;
    self.user = hooks->user;
    self.onAction = hooks->on_action;
    self.onEditKey = hooks->on_edit_key;
    self.onToggleFullscreen = hooks->on_toggle_fullscreen;
    self.onWillTerminate = hooks->on_will_terminate;
}

- (void)requestRedraw {
    cui_ctx *ui = cui_app_ui(NULL);
    if (ui) cui_request_redraw(ui);
}

- (void)showAbout:(id)sender {
    (void)sender;
    if (self.paletteOpen) *self.paletteOpen = false;
    if (self.settingsSection) *self.settingsSection = self.aboutSection;
    if (self.settingsOpen) *self.settingsOpen = true;
    [self requestRedraw];
}

- (void)showSettings:(id)sender {
    (void)sender;
    if (self.paletteOpen) *self.paletteOpen = false;
    if (self.settingsOpen) *self.settingsOpen = true;
    [self requestRedraw];
}

- (void)triggerAction:(NSMenuItem *)sender {
    if (self.onAction) self.onAction((int)sender.tag, self.user);
    [self requestRedraw];
}

- (void)triggerPalette:(id)sender {
    (void)sender;
    if (self.settingsOpen) *self.settingsOpen = false;
    if (self.paletteOpen) *self.paletteOpen = true;
    [self requestRedraw];
}

- (void)toggleFullscreen:(id)sender {
    (void)sender;
    if (self.onToggleFullscreen) self.onToggleFullscreen(self.user);
    [self requestRedraw];
}

- (void)triggerEditKey:(NSMenuItem *)sender {
    if (self.onEditKey) self.onEditKey((cui_key)sender.tag, self.user);
    [self requestRedraw];
}

- (void)quitApp:(id)sender {
    (void)sender;
    if (self.onWillTerminate) self.onWillTerminate(self.user);
    cui_app_quit(NULL);
    [self requestRedraw];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    bool isKeyEvent = (NSApp.currentEvent.type == NSEventTypeKeyDown);
    cui_ctx *ui = cui_app_ui(NULL);
    bool modalOpen = (self.settingsOpen && *self.settingsOpen) ||
                     (self.paletteOpen && *self.paletteOpen);

    if (item.action == @selector(triggerPalette:)) {
        if (isKeyEvent && (modalOpen || (ui && cui_wants_keyboard(ui)))) return NO;
        return YES;
    }
    if (item.action == @selector(triggerAction:)) {
        if (modalOpen) return NO;
        if (isKeyEvent && ui && cui_wants_keyboard(ui)) return NO;
        int tag = (int)item.tag;
        for (int i = 0; i < _cachedActionCount; ++i) {
            if (_cachedActions[i].action_index == tag) {
                return _cachedActions[i].enabled ? YES : NO;
            }
        }
        return NO;
    }
    return YES;
}

- (void)clearHooks {
    self.settingsOpen = NULL;
    self.settingsSection = NULL;
    self.aboutSection = 0;
    self.paletteOpen = NULL;
    self.user = NULL;
    self.onAction = NULL;
    self.onEditKey = NULL;
    self.onToggleFullscreen = NULL;
    self.onWillTerminate = NULL;
}
@end

static AppMenuTarget *shared_menu_target;

static NSMenuItem *make_item(NSString *title, SEL action, NSString *equiv,
                             NSEventModifierFlags mask, id target, NSInteger tag) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:equiv];
    item.keyEquivalentModifierMask = mask;
    item.target = target;
    item.tag = tag;
    return item;
}

static NSMenuItem *make_submenu_item(NSString *title, NSMenu *submenu) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = submenu;
    return item;
}

static void trim_menu_to_count(NSMenu *menu, NSInteger count) {
    while (menu.numberOfItems > count) {
        [menu removeItemAtIndex:menu.numberOfItems - 1];
    }
}

static void ensure_key_monitor(AppMenuTarget *target) {
    if (!target.keyMonitor) {
        /* CUI consumes custom Command shortcuts before the native menu sees them. */
        target.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
            if (!target.settingsOpen || *target.settingsOpen) return event;
            NSEventModifierFlags modifiers = event.modifierFlags &
                (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                 NSEventModifierFlagOption | NSEventModifierFlagShift);
            if (modifiers == NSEventModifierFlagCommand &&
                [event.charactersIgnoringModifiers isEqualToString:@","]) {
                [target showSettings:nil];
                return nil;
            }
            return event;
        }];
    }
}

void app_menu_install(const app_menu_hooks *hooks) {
    if (!hooks || !hooks->settings_open) return;
    if (shared_menu_target) {
        [shared_menu_target applyHooks:hooks];
        ensure_key_monitor(shared_menu_target);
        return;
    }
    NSMenu *bar = NSApp.mainMenu;
    NSMenu *appMenu = bar.itemArray.firstObject.submenu;
    if (!bar || !appMenu) return;

    AppMenuTarget *target = [[AppMenuTarget alloc] init];
    [target applyHooks:hooks];
    shared_menu_target = target;

    NSMenuItem *about = make_item([@"About " stringByAppendingString:@APP_NAME],
        @selector(showAbout:), @"", 0, target, 0);
    NSMenuItem *settings = make_item(@"Settings…",
        @selector(showSettings:), @",", NSEventModifierFlagCommand, target, 0);

    [appMenu insertItem:about atIndex:0];
    [appMenu insertItem:[NSMenuItem separatorItem] atIndex:1];
    [appMenu insertItem:settings atIndex:2];
    [appMenu insertItem:[NSMenuItem separatorItem] atIndex:3];

    /* Route Quit through cui_app_quit so cui_app_run() exits cleanly and runs shutdown hooks. */
    for (NSMenuItem *item in appMenu.itemArray) {
        if (item.action == @selector(terminate:)) {
            item.target = target;
            item.action = @selector(quitApp:);
        }
    }

    /* File menu (populated dynamically from registered APP_MENU_FILE actions) */
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem *fileMenuItem = make_submenu_item(@"File", fileMenu);
    fileMenuItem.hidden = YES;
    target.fileMenu = fileMenu;
    target.fileMenuItem = fileMenuItem;

    /* Edit menu (standard clipboard items + dynamic APP_MENU_EDIT actions) */
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItem:make_item(@"Cut", @selector(triggerEditKey:), @"x",
                                NSEventModifierFlagCommand, target, CUI_KEY_X)];
    [editMenu addItem:make_item(@"Copy", @selector(triggerEditKey:), @"c",
                                NSEventModifierFlagCommand, target, CUI_KEY_C)];
    [editMenu addItem:make_item(@"Paste", @selector(triggerEditKey:), @"v",
                                NSEventModifierFlagCommand, target, CUI_KEY_V)];
    [editMenu addItem:make_item(@"Select All", @selector(triggerEditKey:), @"a",
                                NSEventModifierFlagCommand, target, CUI_KEY_A)];
    target.editMenu = editMenu;
    target.editStaticCount = editMenu.numberOfItems;

    /* View menu (Command Palette + Fullscreen + dynamic APP_MENU_VIEW actions) */
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    target.paletteItem = make_item(@"Command Palette…", @selector(triggerPalette:), @"k",
                                   NSEventModifierFlagCommand, target, 0);
    [viewMenu addItem:target.paletteItem];
    [viewMenu addItem:[NSMenuItem separatorItem]];
    target.fullscreenItem = make_item(@"Enter Full Screen", @selector(toggleFullscreen:), @"f",
                                      NSEventModifierFlagCommand | NSEventModifierFlagControl, target, 0);
    [viewMenu addItem:target.fullscreenItem];
    target.viewMenu = viewMenu;
    target.viewStaticCount = viewMenu.numberOfItems;

    NSInteger insertAt = bar.numberOfItems > 1 ? 1 : bar.numberOfItems;
    [bar insertItem:fileMenuItem atIndex:insertAt];
    [bar insertItem:make_submenu_item(@"Edit", editMenu) atIndex:insertAt + 1];
    [bar insertItem:make_submenu_item(@"View", viewMenu) atIndex:insertAt + 2];

    ensure_key_monitor(target);
}

void app_menu_shutdown(void) {
    if (!shared_menu_target) return;
    @autoreleasepool {
        AppMenuTarget *target = shared_menu_target;
        if (target.keyMonitor) {
            [NSEvent removeMonitor:target.keyMonitor];
            target.keyMonitor = nil;
        }
        [target clearHooks];
    }
}

void app_menu_sync(const app_menu_state *state) {
    if (!shared_menu_target || !state) return;
    AppMenuTarget *target = shared_menu_target;
    bool force = !target.hasSyncedState;

    @autoreleasepool {
        sync_combo_item(target.paletteItem, target.lastPaletteCombo, state->palette_combo, force);
    }
    if (force || target.lastFullscreen != state->fullscreen) {
        target.fullscreenItem.title = state->fullscreen ? @"Exit Full Screen" : @"Enter Full Screen";
    }
    target.lastPaletteCombo = state->palette_combo;
    target.lastFullscreen = state->fullscreen;

    int count = state->action_count;
    if (count < 0) count = 0;
    if (count > APP_MAX_ACTIONS) count = APP_MAX_ACTIONS;

    bool structureChanged = force || (count != target->_cachedActionCount);
    if (!structureChanged) {
        for (int i = 0; i < count; ++i) {
            const app_menu_action *a = &state->actions[i];
            const cached_menu_action *c = &target->_cachedActions[i];
            const char *title = a->title ? a->title : "";
            if (a->action_index != c->action_index ||
                a->menu != c->menu ||
                strcmp(title, c->title) != 0) {
                structureChanged = true;
                break;
            }
        }
    }

    if (structureChanged) {
        @autoreleasepool {
            trim_menu_to_count(target.fileMenu, 0);
            trim_menu_to_count(target.editMenu, target.editStaticCount);
            trim_menu_to_count(target.viewMenu, target.viewStaticCount);
            [target.dynamicItems removeAllObjects];

            bool editHasSep = false;
            bool viewHasSep = false;
            int kept = 0;
            for (int i = 0; i < count; ++i) {
                const app_menu_action *a = &state->actions[i];
                NSMenu *dest = nil;
                if (a->menu == APP_MENU_FILE) {
                    dest = target.fileMenu;
                } else if (a->menu == APP_MENU_EDIT) {
                    if (!editHasSep) {
                        [target.editMenu addItem:[NSMenuItem separatorItem]];
                        editHasSep = true;
                    }
                    dest = target.editMenu;
                } else if (a->menu == APP_MENU_VIEW) {
                    if (!viewHasSep) {
                        [target.viewMenu addItem:[NSMenuItem separatorItem]];
                        viewHasSep = true;
                    }
                    dest = target.viewMenu;
                }
                if (!dest) continue;

                const char *title = a->title ? a->title : "";
                NSString *nsTitle = [NSString stringWithUTF8String:title] ?: @"";
                NSMenuItem *item = make_item(nsTitle, @selector(triggerAction:), @"", 0,
                                             target, a->action_index);
                macos_apply_key_combo(item, a->combo);
                item.state = a->checked ? NSControlStateValueOn : NSControlStateValueOff;
                [dest addItem:item];
                [target.dynamicItems addObject:item];

                cached_menu_action *c = &target->_cachedActions[kept++];
                c->action_index = a->action_index;
                c->menu = a->menu;
                snprintf(c->title, sizeof c->title, "%s", title);
                c->combo = a->combo;
                c->enabled = a->enabled;
                c->checked = a->checked;
            }
            target->_cachedActionCount = kept;
            target.fileMenuItem.hidden = (target.fileMenu.numberOfItems == 0);
        }
    } else {
        for (int i = 0; i < target->_cachedActionCount; ++i) {
            const app_menu_action *a = &state->actions[i];
            cached_menu_action *c = &target->_cachedActions[i];
            NSMenuItem *item = target.dynamicItems[(NSUInteger)i];
            if (!combo_equal(c->combo, a->combo)) {
                @autoreleasepool {
                    macos_apply_key_combo(item, a->combo);
                }
                c->combo = a->combo;
            }
            if (c->checked != a->checked) {
                item.state = a->checked ? NSControlStateValueOn : NSControlStateValueOff;
                c->checked = a->checked;
            }
            c->enabled = a->enabled;
        }
    }

    target.hasSyncedState = true;
}
