#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include "app_config.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool macos_settings_path(char *path, size_t capacity) {
    @autoreleasepool {
        NSURL *support = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                               inDomains:NSUserDomainMask].firstObject;
        NSString *name = NSBundle.mainBundle.infoDictionary[@"CFBundleName"];
        if (!name.length) name = @APP_NAME;
        if (!support || !name.length) return false;
        NSURL *folder = [support URLByAppendingPathComponent:name isDirectory:YES];
        NSURL *file = [folder URLByAppendingPathComponent:@"settings.json" isDirectory:NO];
        return [file getFileSystemRepresentation:path maxLength:capacity];
    }
}

int app_system_dark(void) {
    if (@available(macOS 10.14, *)) {
        @autoreleasepool {
            NSAppearance *appearance = NSApp ? [NSApp effectiveAppearance] : nil;
            if (appearance) {
                NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[
                    NSAppearanceNameAqua, NSAppearanceNameDarkAqua
                ]];
                return [match isEqualToString:NSAppearanceNameDarkAqua] ? 1 : 0;
            }
            NSString *style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
            return [style isEqualToString:@"Dark"] ? 1 : 0;
        }
    }
    return -1;
}

static bool macos_rect_visible_on_any_screen(NSRect frame) {
    /* Ensure the top titlebar band is reachable on at least one active display. */
    CGFloat bar_h = frame.size.height < 32.0 ? frame.size.height : 32.0;
    NSRect titlebar = NSMakeRect(frame.origin.x, NSMaxY(frame) - bar_h, frame.size.width, bar_h);
    for (NSScreen *screen in [NSScreen screens]) {
        NSRect visible = screen.visibleFrame;
        NSRect overlap = NSIntersectionRect(titlebar, visible);
        if (overlap.size.width >= 64.0 && overlap.size.height >= 16.0) return true;
    }
    return false;
}

bool app_window_is_normal(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return true;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        return !win.isMiniaturized;
    }
}

bool app_window_get_pos(void *native_window, const char *backend, int *x, int *y) {
    if (!native_window || !backend || !x || !y || strcmp(backend, "cocoa") != 0) return false;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        if (win.isMiniaturized) return false;
        NSRect frame = win.frame;
        *x = (int)llround(frame.origin.x);
        *y = (int)llround(frame.origin.y);
        return true;
    }
}

bool app_window_get_bounds(void *native_window, const char *backend,
                           bool *fullscreen, bool *normal,
                           int *width, int *height,
                           bool *has_pos, int *x, int *y) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return false;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        bool fs = (win.styleMask & NSWindowStyleMaskFullScreen) != 0;
        bool norm = !win.isMiniaturized;
        NSRect frame = win.frame;
        NSRect content = [win contentRectForFrameRect:frame];
        if (fullscreen) *fullscreen = fs;
        if (normal) *normal = norm;
        if (width) *width = (int)llround(content.size.width);
        if (height) *height = (int)llround(content.size.height);
        if (has_pos) *has_pos = norm;
        if (x) *x = norm ? (int)llround(frame.origin.x) : 0;
        if (y) *y = norm ? (int)llround(frame.origin.y) : 0;
        return true;
    }
}

bool app_window_set_pos(void *native_window, const char *backend, int x, int y) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return false;
    if (x < APP_WINDOW_POS_MIN || x > APP_WINDOW_POS_MAX ||
        y < APP_WINDOW_POS_MIN || y > APP_WINDOW_POS_MAX)
        return false;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        NSRect frame = win.frame;
        frame.origin = NSMakePoint((CGFloat)x, (CGFloat)y);
        if (!macos_rect_visible_on_any_screen(frame)) return false;
        [win setFrameOrigin:frame.origin];
        return true;
    }
}

void app_window_center(void *native_window, const char *backend, int width, int height) {
    (void)width;
    (void)height;
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        [win center];
    }
}

void app_window_apply_metadata(void *native_window, const char *backend) {
    (void)native_window;
    (void)backend;
}

static char kMacOSAppearanceContext;
static CGFloat g_integrated_titlebar_height = 0.0;

static void macos_layout_traffic_lights(NSWindow *win, CGFloat titlebar_height) {
    if (!win || titlebar_height <= 0.0) return;
    if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return;

    NSButton *closeBtn = [win standardWindowButton:NSWindowCloseButton];
    NSButton *minBtn = [win standardWindowButton:NSWindowMiniaturizeButton];
    NSButton *zoomBtn = [win standardWindowButton:NSWindowZoomButton];
    if (!closeBtn || !minBtn || !zoomBtn) return;
    NSView *titlebarView = closeBtn.superview;
    if (!titlebarView) return;

    NSView *container = titlebarView.superview;
    if (container && container.superview) {
        NSRect tfBounds = container.superview.bounds;
        NSRect cf = container.frame;
        if (fabs(cf.size.height - titlebar_height) > 0.5 ||
            fabs(NSMaxY(cf) - NSMaxY(tfBounds)) > 0.5) {
            cf.origin.y = NSMaxY(tfBounds) - titlebar_height;
            cf.size.height = titlebar_height;
            container.frame = cf;
        }
        NSRect tvf = titlebarView.frame;
        if (fabs(tvf.size.height - titlebar_height) > 0.5 || fabs(tvf.origin.y) > 0.5) {
            tvf.origin.y = 0.0;
            tvf.size.height = titlebar_height;
            titlebarView.frame = tvf;
        }
    }

    CGFloat viewH = titlebarView.bounds.size.height;
    NSButton *btns[3] = { closeBtn, minBtn, zoomBtn };
    CGFloat leftX = 14.0;
    CGFloat spacing = 20.0;
    for (int i = 0; i < 3; ++i) {
        NSRect bf = btns[i].frame;
        CGFloat btnH = bf.size.height;
        CGFloat topOffset = floor((titlebar_height - btnH) * 0.5);
        bf.origin.x = leftX + (CGFloat)i * spacing;
        bf.origin.y = titlebarView.isFlipped ? topOffset : (viewH - topOffset - btnH);
        btns[i].frame = bf;
    }
    [titlebarView updateTrackingAreas];
}

static BOOL macos_content_view_cannot_move_window(id self, SEL _cmd) {
    (void)self;
    (void)_cmd;
    return NO;
}

@interface AppWindowObserver : NSObject
@property(nonatomic, assign) app_window_hooks hooks;
@property(nonatomic, assign) bool observingAppearance;
- (void)detach;
- (void)windowDidMove:(NSNotification *)note;
- (void)windowDidResize:(NSNotification *)note;
- (void)windowDidExitFullScreen:(NSNotification *)note;
- (void)windowDidUpdate:(NSNotification *)note;
- (void)windowWillClose:(NSNotification *)note;
@end

@implementation AppWindowObserver
- (void)detach {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    if (self.observingAppearance && NSApp) {
        [NSApp removeObserver:self
                   forKeyPath:@"effectiveAppearance"
                      context:&kMacOSAppearanceContext];
        self.observingAppearance = false;
    }
    memset(&_hooks, 0, sizeof _hooks);
}

- (void)windowDidMove:(NSNotification *)note {
    (void)note;
    if (_hooks.on_moved) {
        _hooks.on_moved(_hooks.user);
    }
}

- (void)windowDidResize:(NSNotification *)note {
    if (g_integrated_titlebar_height > 0.0 && [note.object isKindOfClass:[NSWindow class]]) {
        macos_layout_traffic_lights((NSWindow *)note.object, g_integrated_titlebar_height);
    }
}

- (void)windowDidExitFullScreen:(NSNotification *)note {
    if (g_integrated_titlebar_height > 0.0 && [note.object isKindOfClass:[NSWindow class]]) {
        NSWindow *win = (NSWindow *)note.object;
        macos_layout_traffic_lights(win, g_integrated_titlebar_height);
        dispatch_async(dispatch_get_main_queue(), ^{
            macos_layout_traffic_lights(win, g_integrated_titlebar_height);
        });
    }
}

- (void)windowDidUpdate:(NSNotification *)note {
    if (g_integrated_titlebar_height > 0.0 && [note.object isKindOfClass:[NSWindow class]]) {
        macos_layout_traffic_lights((NSWindow *)note.object, g_integrated_titlebar_height);
    }
}

- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    void (*on_closing)(void *) = _hooks.on_closing;
    void *user = _hooks.user;
    _hooks.on_closing = NULL;
    if (on_closing) {
        on_closing(user);
    }
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    if (context != &kMacOSAppearanceContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    void (^notify)(void) = ^{
        if (self->_hooks.on_theme_changed) {
            self->_hooks.on_theme_changed(self->_hooks.user);
        }
        if (NSApp) {
            NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:0
                                         windowNumber:0
                                              context:nil
                                              subtype:0
                                                data1:0
                                                data2:0];
            if (ev) [NSApp postEvent:ev atStart:NO];
        }
    };
    if ([NSThread isMainThread]) notify();
    else dispatch_async(dispatch_get_main_queue(), notify);
}

- (void)dealloc {
    [self detach];
}
@end

static AppWindowObserver *g_window_observer;

void app_window_set_hooks(void *native_window, const char *backend, const app_window_hooks *hooks) {
    @autoreleasepool {
        if (g_window_observer) {
            [g_window_observer detach];
            g_window_observer = nil;
        }
        if (!native_window || !backend || !hooks || strcmp(backend, "cocoa") != 0) return;

        NSWindow *win = (__bridge NSWindow *)native_window;
        AppWindowObserver *obs = [[AppWindowObserver alloc] init];
        obs.hooks = *hooks;

        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:obs selector:@selector(windowDidMove:)
                   name:NSWindowDidMoveNotification object:win];
        [nc addObserver:obs selector:@selector(windowDidResize:)
                   name:NSWindowDidResizeNotification object:win];
        [nc addObserver:obs selector:@selector(windowDidExitFullScreen:)
                   name:NSWindowDidExitFullScreenNotification object:win];
        [nc addObserver:obs selector:@selector(windowDidUpdate:)
                   name:NSWindowDidUpdateNotification object:win];
        [nc addObserver:obs selector:@selector(windowWillClose:)
                   name:NSWindowWillCloseNotification object:win];
        if (@available(macOS 10.14, *)) {
            if (NSApp && hooks->on_theme_changed) {
                [NSApp addObserver:obs
                        forKeyPath:@"effectiveAppearance"
                           options:0
                           context:&kMacOSAppearanceContext];
                obs.observingAppearance = true;
            }
        }
        g_window_observer = obs;
    }
}

void app_platform_shutdown(void) {
    @autoreleasepool {
        if (g_window_observer) {
            [g_window_observer detach];
            g_window_observer = nil;
        }
    }
}

void app_window_set_dark(void *native_window, const char *backend, bool dark) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return;
    if (@available(macOS 10.14, *)) {
        @autoreleasepool {
            NSWindow *win = (__bridge NSWindow *)native_window;
            NSAppearanceName desired = dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
            if (![win.appearance.name isEqualToString:desired]) {
                win.appearance = [NSAppearance appearanceNamed:desired];
            }
            win.backgroundColor = dark ? [NSColor blackColor] : [NSColor whiteColor];
        }
    }
}

bool app_window_enable_integrated_titlebar(void *native_window, const char *backend, float height) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return false;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        if ((win.styleMask & NSWindowStyleMaskFullSizeContentView) == 0) {
            NSSize prevContentSize = win.contentView ? win.contentView.frame.size
                                                     : [win contentRectForFrameRect:win.frame].size;
            NSRect prevFrame = win.frame;
            win.styleMask |= NSWindowStyleMaskFullSizeContentView;
            NSRect nextFrame = [win frameRectForContentRect:NSMakeRect(0, 0,
                                                                       prevContentSize.width,
                                                                       prevContentSize.height)];
            nextFrame.origin.x = prevFrame.origin.x;
            nextFrame.origin.y = NSMaxY(prevFrame) - nextFrame.size.height;
            [win setFrame:nextFrame display:NO];
        }
        win.titlebarAppearsTransparent = YES;
        win.titleVisibility = NSWindowTitleHidden;
        win.movableByWindowBackground = NO;

        /* Ensure CUIView receives clicks inside the transparent titlebar band
         * instead of AppKit swallowing mouseDown to drag the window. */
        if (win.contentView) {
            Class viewClass = [win.contentView class];
            if (viewClass && viewClass != [NSView class]) {
                class_replaceMethod(viewClass,
                                    @selector(mouseDownCanMoveWindow),
                                    (IMP)macos_content_view_cannot_move_window,
                                    "c@:");
            }
        }

        g_integrated_titlebar_height = height > 28.0f ? (CGFloat)height : 40.0;
        macos_layout_traffic_lights(win, g_integrated_titlebar_height);
        dispatch_async(dispatch_get_main_queue(), ^{
            macos_layout_traffic_lights(win, g_integrated_titlebar_height);
        });
        return true;
    }
}

float app_window_titlebar_left_inset(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return 0.0f;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return 0.0f;
        NSButton *zoomBtn = [win standardWindowButton:NSWindowZoomButton];
        if (zoomBtn && zoomBtn.superview && win.contentView) {
            NSRect r = [zoomBtn convertRect:zoomBtn.bounds toView:win.contentView];
            CGFloat rightEdge = NSMaxX(r);
            if (rightEdge > 40.0 && rightEdge < 120.0) {
                return (float)(rightEdge + 12.0);
            }
        }
        return 78.0f;
    }
}

void app_window_start_drag(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        NSEvent *ev = NSApp.currentEvent;
        if (!ev) return;
        if (ev.type == NSEventTypeLeftMouseDown || ev.type == NSEventTypeLeftMouseDragged) {
            [win performWindowDragWithEvent:ev];
        }
    }
}

void app_window_titlebar_double_click(void *native_window, const char *backend) {
    if (!native_window || !backend || strcmp(backend, "cocoa") != 0) return;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)native_window;
        NSString *action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
        if ([action isEqualToString:@"None"]) return;
        if ([action isEqualToString:@"Minimize"]) {
            [win performMiniaturize:nil];
        } else {
            [win performZoom:nil];
        }
    }
}

static void macos_set_err(char *err, size_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg ? msg : "Unknown error.");
}

static const char *macos_release_arch(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x64";
#else
    return NULL;
#endif
}

bool app_os_info(char *out, size_t capacity) {
    if (!out || capacity == 0) return false;
    @autoreleasepool {
        NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
        const char *arch = macos_release_arch();
        int n = snprintf(out, capacity, "macOS %ld.%ld.%ld (%s)",
                         (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion,
                         arch ? arch : "unknown");
        return n > 0 && (size_t)n < capacity;
    }
}

static bool macos_url_fetch(NSURL *url, NSString *destPath,
                            NSTimeInterval reqTimeout, NSTimeInterval resTimeout,
                            app_updater_abort_fn should_abort, void *abort_user,
                            NSString **outFinalUrl, char *err, size_t err_cap) {
    if (should_abort && should_abort(abort_user)) {
        macos_set_err(err, err_cap, destPath ? "Update cancelled." : "Update check cancelled.");
        return false;
    }
    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = reqTimeout;
    cfg.timeoutIntervalForResource = resTimeout;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg];

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    if (!destPath) req.HTTPMethod = @"HEAD";
    req.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [req setValue:[NSString stringWithFormat:@"%s/%s", APP_NAME, APP_VERSION]
forHTTPHeaderField:@"User-Agent"];

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSString *resolved = nil;
    __block NSString *errMsg = nil;
    void (^finish)(NSURL *, NSURLResponse *, NSError *) =
        ^(NSURL *location, NSURLResponse *response, NSError *error) {
        if (error) {
            errMsg = error.localizedDescription ?: (destPath ? @"Download failed." : @"Network error.");
        } else if (![response isKindOfClass:[NSHTTPURLResponse class]]) {
            errMsg = destPath ? @"Unexpected download response." : @"Unexpected server response.";
        } else {
            NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
            NSInteger code = http.statusCode;
            if (!destPath) {
                if (code < 200 || code >= 400) {
                    errMsg = [NSString stringWithFormat:@"Server returned HTTP %ld.", (long)code];
                } else {
                    resolved = http.URL.absoluteString;
                    if (!resolved.length) errMsg = @"Empty redirect response.";
                }
            } else if (code == 200 && location) {
                NSError *moveErr = nil;
                if (![[NSFileManager defaultManager] moveItemAtURL:location
                                                             toURL:[NSURL fileURLWithPath:destPath]
                                                             error:&moveErr]) {
                    errMsg = moveErr.localizedDescription ?: @"Could not save update archive.";
                }
            } else if (code == 404) {
                errMsg = @"No macOS update archive was found for this release.";
            } else {
                errMsg = [NSString stringWithFormat:@"Download failed (HTTP %ld).", (long)code];
            }
        }
        dispatch_semaphore_signal(sem);
    };

    NSURLSessionTask *task = destPath
        ? (NSURLSessionTask *)[session downloadTaskWithRequest:req completionHandler:finish]
        : (NSURLSessionTask *)[session dataTaskWithRequest:req
                                         completionHandler:^(NSData *data, NSURLResponse *r, NSError *e) {
              (void)data;
              finish(nil, r, e);
          }];
    [task resume];

    bool aborted = false;
    for (;;) {
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) == 0) break;
        if (!aborted && should_abort && should_abort(abort_user)) {
            aborted = true;
            [task cancel];
            [session invalidateAndCancel];
        }
    }
    if (!aborted) [session finishTasksAndInvalidate];

    if (aborted || (should_abort && should_abort(abort_user))) {
        macos_set_err(err, err_cap, destPath ? "Update cancelled." : "Update check cancelled.");
        return false;
    }
    if (errMsg) {
        macos_set_err(err, err_cap, errMsg.UTF8String);
        return false;
    }
    if (outFinalUrl) *outFinalUrl = resolved;
    return true;
}

bool app_http_resolve_redirect(const char *url, char *final_url, size_t capacity,
                               app_updater_abort_fn should_abort, void *abort_user,
                               char *err, size_t err_cap) {
    if (!url || !final_url || capacity == 0) {
        macos_set_err(err, err_cap, "Invalid update check request.");
        return false;
    }
    final_url[0] = '\0';
    @autoreleasepool {
        NSString *urlStr = [NSString stringWithUTF8String:url];
        NSURL *nsUrl = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!nsUrl) {
            macos_set_err(err, err_cap, "Invalid update URL.");
            return false;
        }
        NSString *resolved = nil;
        if (!macos_url_fetch(nsUrl, nil, 15.0, 20.0, should_abort, abort_user, &resolved, err, err_cap)) {
            return false;
        }
        const char *utf8 = resolved.UTF8String;
        if (!utf8 || strlen(utf8) >= capacity) {
            macos_set_err(err, err_cap, "Resolved URL is too long.");
            return false;
        }
        snprintf(final_url, capacity, "%s", utf8);
        return true;
    }
}

bool app_updater_can_self_update(void) {
    if (!macos_release_arch()) return false;
    @autoreleasepool {
        NSString *bundlePath = NSBundle.mainBundle.bundlePath;
        if (!bundlePath.length || ![bundlePath hasSuffix:@".app"]) return false;
        if ([bundlePath rangeOfString:@"/AppTranslocation/"].location != NSNotFound) return false;
        NSString *parentDir = [bundlePath stringByDeletingLastPathComponent];
        if (!parentDir.length) return false;
        return access(parentDir.fileSystemRepresentation, W_OK) == 0 &&
               access(bundlePath.fileSystemRepresentation, W_OK) == 0;
    }
}

static bool macos_run_tool(NSString *launchPath, NSArray<NSString *> *args,
                           app_updater_abort_fn should_abort, void *abort_user,
                           const char *fail_msg, char *err, size_t err_cap) {
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:launchPath];
    task.arguments = args;
    NSFileHandle *nullHandle = [NSFileHandle fileHandleWithNullDevice];
    task.standardOutput = nullHandle;
    task.standardError = nullHandle;
    bool ok = [task launchAndReturnError:nil];
    bool aborted = false;
    if (ok) {
        while ([task isRunning]) {
            if (!aborted && should_abort && should_abort(abort_user)) {
                aborted = true;
                [task terminate];
                break;
            }
            usleep(25000);
        }
        [task waitUntilExit];
        ok = !aborted && task.terminationReason == NSTaskTerminationReasonExit && task.terminationStatus == 0;
    }
    if (!ok && fail_msg) {
        macos_set_err(err, err_cap,
                      (aborted || (should_abort && should_abort(abort_user)))
                          ? "Update cancelled." : fail_msg);
    }
    return ok;
}

bool app_updater_install_release(const char *repo, const char *tag,
                                 char *relaunch_path, size_t relaunch_cap,
                                 app_updater_abort_fn should_abort, void *abort_user,
                                 char *err, size_t err_cap) {
    if (!repo || !tag || !app_updater_can_self_update()) {
        macos_set_err(err, err_cap, "In-place update is unavailable for this installation.");
        return false;
    }
    const char *slash = strchr(repo, '/');
    const char *arch = macos_release_arch();
    if (!slash || !slash[1] || !arch) {
        macos_set_err(err, err_cap, "Invalid repository or architecture.");
        return false;
    }

    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *bundlePath = NSBundle.mainBundle.bundlePath;
        NSString *parentDir = [bundlePath stringByDeletingLastPathComponent];
        NSString *bundleName = [bundlePath lastPathComponent];

        NSString *urlStr = [NSString stringWithFormat:
            @"https://github.com/%s/releases/download/%s/%s-%s-macos-%s.zip",
            repo, tag, slash + 1, tag, arch];
        NSURL *url = [NSURL URLWithString:urlStr];
        if (!url) {
            macos_set_err(err, err_cap, "Invalid release download URL.");
            return false;
        }

        NSString *tmpBase = NSTemporaryDirectory();
        if (!tmpBase.length) tmpBase = @"/tmp";
        char tmpTemplate[4096];
        int n = snprintf(tmpTemplate, sizeof tmpTemplate, "%s/app-update.XXXXXX", tmpBase.fileSystemRepresentation);
        if (n < 0 || (size_t)n >= sizeof tmpTemplate || !mkdtemp(tmpTemplate)) {
            macos_set_err(err, err_cap, "Could not create a temporary directory.");
            return false;
        }
        NSString *tmpDir = [fm stringWithFileSystemRepresentation:tmpTemplate length:strlen(tmpTemplate)];
        NSString *archivePath = [tmpDir stringByAppendingPathComponent:@"update.zip"];
        NSString *extractDir = [tmpDir stringByAppendingPathComponent:@"extract"];
        NSString *stagedPath = nil;
        bool ok = false;

        do {
            if (!macos_url_fetch(url, archivePath, 30.0, 180.0, should_abort, abort_user, NULL, err, err_cap)) {
                break;
            }
            if (![fm createDirectoryAtPath:extractDir withIntermediateDirectories:YES attributes:nil error:nil]) {
                macos_set_err(err, err_cap, "Could not prepare extraction directory.");
                break;
            }
            if (!macos_run_tool(@"/usr/bin/ditto", @[ @"-x", @"-k", archivePath, extractDir ],
                                should_abort, abort_user, "Could not extract update archive.", err, err_cap)) {
                break;
            }

            NSString *srcApp = nil;
            for (NSString *entry in [fm contentsOfDirectoryAtPath:extractDir error:nil]) {
                if (![entry hasSuffix:@".app"]) continue;
                NSString *candidate = [extractDir stringByAppendingPathComponent:entry];
                if ([fm fileExistsAtPath:[candidate stringByAppendingPathComponent:@"Contents/Info.plist"]]) {
                    srcApp = candidate;
                    break;
                }
            }
            if (!srcApp) {
                macos_set_err(err, err_cap, "Downloaded archive does not contain an app bundle.");
                break;
            }

            pid_t pid = getpid();
            stagedPath = [parentDir stringByAppendingPathComponent:
                [NSString stringWithFormat:@".%@.updating.%ld", bundleName, (long)pid]];
            NSString *backupPath = [parentDir stringByAppendingPathComponent:
                [NSString stringWithFormat:@".%@.backup.%ld", bundleName, (long)pid]];
            [fm removeItemAtPath:stagedPath error:nil];
            [fm removeItemAtPath:backupPath error:nil];

            if (!macos_run_tool(@"/usr/bin/ditto", @[ srcApp, stagedPath ],
                                should_abort, abort_user, "Could not stage updated app bundle.", err, err_cap)) {
                break;
            }
            (void)macos_run_tool(@"/usr/bin/xattr", @[ @"-dr", @"com.apple.quarantine", stagedPath ],
                                 should_abort, abort_user, NULL, NULL, 0);

            if (should_abort && should_abort(abort_user)) {
                macos_set_err(err, err_cap, "Update cancelled.");
                break;
            }

            const char *curFs = bundlePath.fileSystemRepresentation;
            const char *stagedFs = stagedPath.fileSystemRepresentation;
            const char *backupFs = backupPath.fileSystemRepresentation;
            if (rename(curFs, backupFs) != 0) {
                macos_set_err(err, err_cap, "Could not replace the installed app bundle.");
                break;
            }
            if (rename(stagedFs, curFs) != 0) {
                (void)rename(backupFs, curFs);
                macos_set_err(err, err_cap, "Could not move the updated app bundle into place.");
                break;
            }
            stagedPath = nil;
            [fm removeItemAtPath:backupPath error:nil];

            if (relaunch_path && relaunch_cap > 0) {
                if (![bundlePath getFileSystemRepresentation:relaunch_path maxLength:relaunch_cap]) {
                    relaunch_path[0] = '\0';
                }
            }
            ok = true;
        } while (0);

        if (stagedPath) [fm removeItemAtPath:stagedPath error:nil];
        [fm removeItemAtPath:tmpDir error:nil];
        return ok;
    }
}
