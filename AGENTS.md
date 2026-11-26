# Working on this app

This is a C99 desktop app using [Better-C-UI](https://github.com/mzgs/Better-C-UI).
Keep changes small and use existing CUI components before adding custom drawing
or dependencies.

## Layout

- `main.c`: user-facing entry point (`app_state`, `app_chrome_register_actions()`, `frame()`, `main()`).
- `resources/`: app icons (`app.png`, `app.icns`, `app.ico`, `generate_icons.sh`) and OS metadata templates (`Info.plist.in`, `app.rc.in`, `app.desktop.in`).
- `src/chrome/`: app chrome (`app_chrome.{h,c}`, `settings_dialog.c`, `command_palette.c`).
- `src/log/`: thread-safe, size-capped file logging and crash signal handlers (`app_log.{h,c}`).
- `src/settings/`: JSON persistence, atomic writes, and keybinding helpers (`settings.{h,c}`).
- `src/platform/`: OS-specific dark mode, window positioning, and macOS native menu bar (`platform.h`, `platform_*.{c,m}`, `app_menu.h`, `app_menu_macos.m`).

## Library reference

- `CMakeLists.txt` pins the library revision through FetchContent. Preserve the
  pin unless updating the library is part of the task.
- Configure the project first to populate `build/_deps/cui-src/`.
- Before using an unfamiliar API, read the pinned checkout's
  `docs/getting-started.md`, relevant `docs/components.md` or
  `docs/components-more.md` entries, and public headers in `include/cui/`.
  Use `examples/` for complete usage patterns. Check actual signatures rather
  than guessing APIs from other UI libraries or newer online documentation.
- Keep app changes in this repository's source files. Files under `build/`,
  including the fetched library, are generated or disposable.

## CUI conventions

- Keep `int main(int argc, char **argv)` and the `cui_app_run` callback setup in `main.c`.
  CUI owns window creation, the event loop, rendering, and cleanup.
- Describe the UI inside `frame()` every frame. Store persistent application
  state outside its automatic local variables, using static state or the
  callback's `user` pointer as needed.
- Include `<cui/cui_app.h>` for the app layer and `<cui/cui.h>` for widgets.
  Use C99 designated options, for example
  `cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY)`.
- Handle widget return values for interactions. Give repeated widgets stable,
  unique IDs with `##suffix` labels or balanced `cui_push_id_*`/`cui_pop_id` calls.
- Do not `return`, `break`, or `goto` out of CUI container macro blocks; their
  end calls must run. Use explicit begin/end APIs when early exits are needed.
- Keep the frame callback responsive. The app sleeps when idle; leave
  continuous rendering off unless the feature needs it.
- Retain `cui::cui` linking and `cui_app_target(mcode)` in CMake so platform
  libraries and GUI executable flags remain correct.

## Build and verify

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

CTest runs `window_smoke` and (on macOS) `run_script_smoke`. The smoke tests need a graphical desktop, save
`build/window-smoke.png`, and check startup, rendering, and exit.
For UI changes, also run the app and exercise the changed interactions,
keyboard navigation, resizing, and light/dark themes. On macOS launch with
`./run.sh` to build changed files and run; see `README.md` for other platforms.
Report checks performed and any platform or interaction left untested.
