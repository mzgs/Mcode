# Mcode

A cross-platform, resizable C99 desktop app template using [Better-C-UI](https://github.com/mzgs/Better-C-UI).
Add your state, registered actions, and UI calls inside `main.c` (`app_state`, `app_chrome_register_actions()`, and `frame()`). Template scaffolding lives under `src/`.

## Quick start (macOS)

```sh
./run.sh
```

The script works from any directory, configures the first build, and lets CMake
rebuild changed files before launching. When nothing changed, it runs the existing
app without recompiling. A failed build stops the script before launch.

## Project layout

```text
main.c                 App entry point (state, action handler, frame UI)
resources/             App icons (app.png, app.icns, app.ico) & OS metadata templates
src/
  chrome/              Lifecycle, CLI parsing, Settings dialog, Command Palette
  log/                 Thread-safe, size-capped file logger (`app_log_*`)
  settings/            JSON persistence, atomic writes, keybinding dispatch & conflicts
  platform/            OS dark-mode detection, window positioning, macOS native menu bar
```

## Built-in features

- **Clean app entry point**: `main.c` stays minimal—wrap your UI between `app_chrome_begin_frame()` and `app_chrome_end_frame()` and register app actions with `app_chrome_register_actions()`.
- **App icons & OS metadata**: Ships with default icons and platform packaging wired in `CMakeLists.txt`—macOS `Info.plist` + `app.icns` (Retina + automatic graphics switching), Windows `app.ico` + `VS_VERSION_INFO` executable metadata, and Linux `.desktop` entry + icon (`WM_CLASS` matched to `APP_BUNDLE_ID`). Replace `resources/app.png` and run `./resources/generate_icons.sh` on macOS to regenerate `.icns` and `.ico`.
- **Native & cross-platform menu bar**: On macOS, native **App** (**About**, **Settings…** `⌘,`), **File**, **Edit** (**Cut**, **Copy**, **Paste**, **Select All**), **View** (**Command Palette…**, **Enter/Exit Full Screen** `⌃⌘F`), and **Window** menus automatically include any registered action placed in `APP_MENU_FILE`, `APP_MENU_EDIT`, or `APP_MENU_VIEW` and stay dynamically synced with keybindings, enabled state, and checkmarks. On Windows and Linux, the top menu bar provides **File**, **Edit**, **View**, and **Help** (plus **Ctrl+,** for Settings and **F11** for Fullscreen) with the same dynamic actions.
- **Appearance preferences**: Persisted theme mode (**System**, **Light**, **Dark**), accent color, text scale (`85%`–`135%`), and **Reduce motion**.
- **Window state persistence**: Restores the last non-fullscreen window position (validated against active displays so windows never open off-screen), size, and fullscreen state across launches.
- **Extensible actions & keybindings**: Register actions once via `app_chrome_register_action()` / `app_chrome_register_actions()` to wire them into global shortcuts, **Settings → Keybindings** (filterable, rebindable, persisted to `settings.json`, with duplicate/reserved conflict warnings), menus, and the Command Palette without editing `src/`. Toggle runtime availability or menu checkmarks with `app_chrome_set_action_enabled()` and `app_chrome_set_action_checked()`.
- **Extensible Command Palette**: Press **⌘K** on macOS or **Ctrl+K** on Windows/Linux (rebindable in Keybindings) to search registered app actions, dynamic commands injected via `app_chrome_set_palette_provider()` / `app_palette_add()`, navigation, theme switching, fullscreen, and diagnostics.
- **Toast notifications & Settings file tools**: Import, export, reset (with confirmation), or reveal the settings folder directly from Settings. Unknown fields in imported `settings.json` files are safely ignored across versions.
- **File logging, Crash reporting & Diagnostics**: Thread-safe, size-capped logging (`app_log_info`/`warn`/`error`/`debug`) writes to `app.log` beside `settings.json` (1 MiB cap, rotating to `app.log.1`) so errors aren't lost when launched from Finder or Explorer. Fatal crash handlers (`SIGSEGV`/`SIGABRT`/`SIGBUS`/`SIGFPE`/`SIGILL` on macOS/Linux, unhandled exceptions + `SIGABRT` on Windows) append a crash summary and stack trace to `app.log` and `stderr` before letting the OS terminate the process normally. **Settings → About** includes a one-click **Copy** button for debug info (version, OS/arch, backend, renderer, theme, window size, paths) and an **Open Log** shortcut (also in the Command Palette).
- **In-app update check**: Optionally checks GitHub Releases silently at most once a day on startup (configurable via **Check for updates on startup** in **Settings → About**, or disabled when `APP_GITHUB_REPO` is empty). When a newer release is found, a toast with an **Update** action and a **New** badge on **About** let the user update directly. On macOS and Linux it downloads, installs in place, and relaunches (falling back to opening the release page on Windows or non-writable locations).
- **CLI flags**: `--help` (`-h`), `--version` (`-v`), `--settings <path>`, and `--software` (force software renderer).

Settings, appearance, window state, and keybinding changes autosave to:

- macOS — `~/Library/Application Support/<app name>/settings.json`
- Windows — `%LOCALAPPDATA%\<app name>\settings.json`
- Linux — `$XDG_CONFIG_HOME/<app name>/settings.json`, defaulting to `~/.config/<app name>/settings.json`

Set `APP_NAME` in `CMakeLists.txt` or pass `-DAPP_NAME="My App"` when configuring
to name a new app and give it its own settings folder.

## Build and test

Requires Git, CMake 3.14+, and a C compiler with the system SDK. On macOS,
install Xcode Command Line Tools and CMake. Linux also needs X11 development
headers (`libx11-dev` on Debian/Ubuntu). Windows supports Visual Studio or MinGW.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The first configure downloads pinned versions of Better-C-UI and yyjson and needs internet
access. Its examples, tests, and optional add-ons are excluded by default.

Run on macOS:

```sh
open "build/C GUI Template.app"
```

Run on Linux: `./build/mcode`. With Visual Studio on Windows:
`build\Release\mcode.exe` (MinGW: `build\mcode.exe`).

Verify window creation, rendering, and clean exit:

```sh
ctest --test-dir build -C Release --output-on-failure
```

The smoke test requires a graphical desktop session and saves `build/window-smoke.png`.

## Release

Cross-platform CI (`.github/workflows/ci.yml`) runs only when a `v*` tag is pushed via `release.sh`:

```sh
./release.sh
```

This auto-increments `VERSION` in `CMakeLists.txt` (`0.1.0` → `0.1.1`), commits, tags `v<version>`, and pushes to trigger GitHub Actions builds and smoke tests for macOS, Linux, and Windows, publishing packaged archives as a GitHub Release. Pass an explicit version (e.g. `./release.sh 1.0.0`) to jump to a specific version, or `--dry-run` to preview.

### Install latest release (macOS / Linux)

Install or update directly from GitHub Releases (avoids browser `com.apple.quarantine` Gatekeeper warnings for non-notarized macOS builds):

```sh
curl -fsSL https://raw.githubusercontent.com/mzgs/Mcode/main/install.sh | sh
```

Pass a specific tag with `sh -s -- v0.1.3`, or override `REPO`, `VERSION`, and `INSTALL_DIR` via environment variables. On macOS this installs into `/Applications` (or `~/Applications` if `/Applications` is not writable); on Linux it installs the binary into `~/.local/bin` along with its `.desktop` entry and icon into `~/.local/share`. Update `REPO` at the top of `install.sh` when forking this template.

### macOS code signing (optional)

To keep a consistent signature across updates (so macOS preserves granted permissions) without committing private keys to git, store your `.p12` certificate and password once on your machine:

```text
~/.macos-codesign/macos-codesign.p12
~/.macos-codesign/password
```

When those files exist and `gh` is logged in (`gh auth login`), `./release.sh` automatically uploads `MACOS_CERT_P12` and `MACOS_CERT_PASSWORD` to the current GitHub repo on the first release—so new apps forked from this template get signed automatically with zero manual secret setup. You can also set the two secrets manually in GitHub Actions settings. Works with either a free self-signed Code Signing `.p12` or an Apple Developer ID `.p12`; set a unique `APP_BUNDLE_ID` in `CMakeLists.txt` per app.

## First-time setup (new project)

Run the bootstrap script to configure `CMakeLists.txt`, `install.sh`, `README.md`, and `AGENTS.md` in one step (safe to re-run later, or pass `--dry-run` to preview):

```sh
./init.sh
```

Or non-interactively with flags:

```sh
./init.sh --name "My Notes" --repo "alice/my-notes" --yes
```

`init.sh` auto-detects defaults from your git remote (`origin`) and `git config user.name`, sets `APP_NAME`, `APP_DESCRIPTION`, `APP_AUTHOR`, `APP_COPYRIGHT`, `APP_BUNDLE_ID`, `APP_GITHUB_REPO` (pass `--repo ""` to disable updates), renames the CMake project and executable target, resets `VERSION` to `0.1.0`, and clears any stale `build/CMakeCache.txt`.

Then finish setup:

1. **App icon (`resources/`)** — replace `resources/app.png` (1024×1024 PNG) with your own icon, then run `./resources/generate_icons.sh` on macOS (or replace `resources/app.icns` and `resources/app.ico` directly).
2. **Verify build & first release**:
   - Build and run locally (`./run.sh` on macOS, or `cmake -S . -B build && cmake --build build`) and run `ctest --test-dir build --output-on-failure`.
   - Ensure GitHub Actions is enabled for the new repo (and optionally configure `~/.macos-codesign/` + `gh auth login` for macOS code signing), then run `./release.sh 0.1.0` when ready for your first release.
