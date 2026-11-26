cmake_minimum_required(VERSION 3.14)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment version" FORCE)
endif()
project(mcode VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(APP_NAME "Mcode" CACHE STRING "App name used for the window and settings folder")
set(APP_DESCRIPTION "Native desktop control plane for AI coding agents." CACHE STRING "Short app description for OS metadata and About")
set(APP_AUTHOR "Mcode" CACHE STRING "Publisher / author name for OS executable metadata")
set(APP_COPYRIGHT "" CACHE STRING "Optional copyright notice for macOS Info.plist and Windows version info")
set(APP_BUNDLE_ID "com.mcode.app" CACHE STRING "Reverse-DNS app identifier (macOS CFBundleIdentifier, Linux desktop/icon ID)")
set(APP_GITHUB_REPO "" CACHE STRING "GitHub owner/repo for update checks (empty to disable)")

function(app_escape_xml out_var value)
    string(REPLACE "&" "&amp;" value "${value}")
    string(REPLACE "<" "&lt;" value "${value}")
    string(REPLACE ">" "&gt;" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(app_escape_c out_var value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(app_escape_rc out_var value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\"\"" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(app_escape_desktop out_var value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(cui
    GIT_REPOSITORY https://github.com/mzgs/Better-C-UI.git
    GIT_TAG d5f4bd6f4d4359ca3667e8a848324e0bb05a17d8)
FetchContent_Declare(yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 6447536015f3d600f3d65323b10976103b337ca7)
FetchContent_Declare(libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG 934bc2fbf21800ac3458a499df8820ca5fb45fd3)
FetchContent_MakeAvailable(yyjson libvterm)
add_library(vterm STATIC
    "${libvterm_SOURCE_DIR}/src/encoding.c"
    "${libvterm_SOURCE_DIR}/src/keyboard.c"
    "${libvterm_SOURCE_DIR}/src/mouse.c"
    "${libvterm_SOURCE_DIR}/src/parser.c"
    "${libvterm_SOURCE_DIR}/src/pen.c"
    "${libvterm_SOURCE_DIR}/src/screen.c"
    "${libvterm_SOURCE_DIR}/src/state.c"
    "${libvterm_SOURCE_DIR}/src/unicode.c"
    "${libvterm_SOURCE_DIR}/src/vterm.c")
include(GNUInstallDirs)
target_include_directories(vterm
    PUBLIC "$<BUILD_INTERFACE:${libvterm_SOURCE_DIR}/include>"
    PRIVATE "${libvterm_SOURCE_DIR}/src")
install(TARGETS vterm EXPORT cuiVTermTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" EXCLUDE_FROM_ALL)
add_library(VTerm::VTerm ALIAS vterm)
set(CUI_VTERM ON CACHE BOOL "" FORCE)
if(WIN32 AND NOT IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    # Work around Better-C-UI passing "/" to file(RELATIVE_PATH) on Windows.
    if(CMAKE_INSTALL_LIBDIR)
        get_filename_component(_cui_libdir "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}" ABSOLUTE)
    else()
        get_filename_component(_cui_libdir "${CMAKE_INSTALL_PREFIX}/lib" ABSOLUTE)
    endif()
    set(CMAKE_INSTALL_LIBDIR "${_cui_libdir}")
    FetchContent_MakeAvailable(cui)
    unset(CMAKE_INSTALL_LIBDIR)
else()
    FetchContent_MakeAvailable(cui)
endif()

app_escape_c(APP_C_NAME "${APP_NAME}")
app_escape_c(APP_C_DESCRIPTION "${APP_DESCRIPTION}")
app_escape_c(APP_C_BUNDLE_ID "${APP_BUNDLE_ID}")
app_escape_c(APP_C_GITHUB_REPO "${APP_GITHUB_REPO}")

configure_file(resources/app_config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app_config.h" @ONLY)

add_library(app_common INTERFACE)
target_include_directories(app_common INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
    target_compile_options(app_common INTERFACE /utf-8 /W4)
else()
    target_compile_options(app_common INTERFACE -Wall -Wextra)
endif()

if(APPLE)
    enable_language(OBJC)
    add_library(app_platform STATIC src/platform/platform_macos.m)
    set_source_files_properties(src/platform/platform_macos.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(app_platform PRIVATE "-framework Cocoa")
elseif(WIN32)
    add_library(app_platform STATIC src/platform/platform_win32.c)
    target_link_libraries(app_platform PRIVATE advapi32 user32 winhttp shell32 ole32)
else()
    add_library(app_platform STATIC src/platform/platform_x11.c)
    target_link_libraries(app_platform PRIVATE cui::cui)
endif()
target_include_directories(app_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/platform")
target_link_libraries(app_platform PRIVATE app_common)

add_library(app_log STATIC src/log/app_log.c)
target_include_directories(app_log PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/log")
target_link_libraries(app_log PRIVATE app_common Threads::Threads)

add_library(app_settings STATIC src/settings/settings.c)
target_include_directories(app_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/settings")
target_link_libraries(app_settings PUBLIC app_log app_platform cui::cui PRIVATE app_common yyjson)
if(WIN32)
    target_link_libraries(app_settings PRIVATE uuid shell32 ole32)
endif()

add_library(app_chrome STATIC
    src/chrome/app_chrome.c
    src/chrome/settings_dialog.c
    src/chrome/command_palette.c
    src/chrome/updater.c)
target_include_directories(app_chrome PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/chrome")
target_link_libraries(app_chrome PUBLIC app_settings PRIVATE app_common Threads::Threads)
if(APPLE)
    target_sources(app_chrome PRIVATE src/platform/app_menu_macos.m)
    set_source_files_properties(src/platform/app_menu_macos.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(app_chrome PRIVATE "-framework Cocoa")
endif()

add_library(app_mcode STATIC
    src/mcode/mcode_types.c
    src/mcode/mcode_proc.c
    src/mcode/mcode_git.c
    src/mcode/mcode_files.c
    src/mcode/mcode_agent.c
    src/mcode/mcode_store.c
    src/mcode/mcode_ui.c
    src/mcode/mcode_ui_sidebar.c
    src/mcode/mcode_ui_chat.c
    src/mcode/mcode_ui_diff.c
    src/mcode/mcode_ui_terminal.c
    src/mcode/mcode_ui_usage.c
    src/mcode/mcode_ui_dialogs.c)
target_include_directories(app_mcode PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/mcode")
target_link_libraries(app_mcode PUBLIC app_chrome cui::vterm PRIVATE app_common yyjson Threads::Threads)

add_executable(empty_gui MACOSX_BUNDLE main.c)
target_link_libraries(empty_gui PRIVATE app_mcode app_chrome app_common)
cui_app_target(empty_gui)
install(TARGETS empty_gui
    BUNDLE DESTINATION . COMPONENT app
    RUNTIME DESTINATION . COMPONENT app)
if(APPLE)
    set(APP_OSX_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT APP_OSX_MIN_VERSION)
        set(APP_OSX_MIN_VERSION "12.0")
    endif()
    app_escape_xml(APP_PLIST_NAME "${APP_NAME}")
    app_escape_xml(APP_PLIST_BUNDLE_ID "${APP_BUNDLE_ID}")
    app_escape_xml(APP_PLIST_COPYRIGHT "${APP_COPYRIGHT}")

    set(APP_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.icns")
    set_source_files_properties("${APP_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(empty_gui PRIVATE "${APP_MACOS_ICON}")

    set_target_properties(empty_gui PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        MACOSX_BUNDLE_ICON_FILE "app.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_BUNDLE_ID}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    add_custom_command(TARGET empty_gui POST_BUILD
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/codesign_macos.sh" "$<TARGET_BUNDLE_DIR:empty_gui>"
        COMMENT "Code signing ${APP_NAME}.app (if ~/.macos-codesign/macos-codesign.p12 is present)"
        VERBATIM)
    add_custom_target(run COMMAND $<TARGET_FILE:empty_gui> DEPENDS empty_gui
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
elseif(WIN32)
    set(APP_VER_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(APP_VER_MINOR "${PROJECT_VERSION_MINOR}")
    set(APP_VER_PATCH "${PROJECT_VERSION_PATCH}")
    set(APP_VER_TWEAK "${PROJECT_VERSION_TWEAK}")
    if("${APP_VER_MAJOR}" STREQUAL "")
        set(APP_VER_MAJOR 0)
    endif()
    if("${APP_VER_MINOR}" STREQUAL "")
        set(APP_VER_MINOR 0)
    endif()
    if("${APP_VER_PATCH}" STREQUAL "")
        set(APP_VER_PATCH 0)
    endif()
    if("${APP_VER_TWEAK}" STREQUAL "")
        set(APP_VER_TWEAK 0)
    endif()
    get_target_property(APP_WIN32_OUTPUT_NAME empty_gui OUTPUT_NAME)
    if(NOT APP_WIN32_OUTPUT_NAME)
        set(APP_WIN32_OUTPUT_NAME "empty_gui")
    endif()
    app_escape_rc(APP_RC_NAME "${APP_NAME}")
    app_escape_rc(APP_RC_DESCRIPTION "${APP_DESCRIPTION}")
    app_escape_rc(APP_RC_AUTHOR "${APP_AUTHOR}")
    app_escape_rc(APP_RC_COPYRIGHT "${APP_COPYRIGHT}")
    app_escape_rc(APP_RC_TARGET "${APP_WIN32_OUTPUT_NAME}")
    set(APP_WIN32_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
    app_escape_rc(APP_RC_ICON_PATH "${APP_WIN32_ICON}")
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/app.rc" PROPERTIES
        OBJECT_DEPENDS "${APP_WIN32_ICON}")
    target_sources(empty_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
else()
    get_target_property(APP_LINUX_OUTPUT_NAME empty_gui OUTPUT_NAME)
    if(NOT APP_LINUX_OUTPUT_NAME)
        set(APP_LINUX_OUTPUT_NAME "empty_gui")
    endif()
    app_escape_desktop(APP_DESKTOP_NAME "${APP_NAME}")
    app_escape_desktop(APP_DESKTOP_DESCRIPTION "${APP_DESCRIPTION}")
    set(APP_EXEC_NAME "${APP_LINUX_OUTPUT_NAME}")
    configure_file(resources/app.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/${APP_BUNDLE_ID}.desktop" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${APP_BUNDLE_ID}.desktop"
        DESTINATION share/applications COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.png"
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME "${APP_BUNDLE_ID}.png" COMPONENT app)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(log_tests tests/log_tests.c)
    target_link_libraries(log_tests PRIVATE app_log app_common)
    add_test(NAME log_tests COMMAND log_tests)

    add_executable(updater_tests tests/updater_tests.c)
    target_link_libraries(updater_tests PRIVATE app_chrome app_common)
    add_test(NAME updater_tests COMMAND updater_tests)

    add_executable(actions_tests tests/actions_tests.c)
    target_link_libraries(actions_tests PRIVATE app_chrome app_common)
    add_test(NAME actions_tests COMMAND actions_tests)

    add_executable(mcode_tests tests/mcode_tests.c)
    target_link_libraries(mcode_tests PRIVATE app_mcode app_common)
    add_test(NAME mcode_tests COMMAND mcode_tests)

    add_test(NAME window_smoke COMMAND ${CMAKE_COMMAND} -E env
        "CUI_SCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/window-smoke.png"
        CUI_SCREENSHOT_FRAMES=1
        $<TARGET_FILE:empty_gui>
        --settings "${CMAKE_CURRENT_BINARY_DIR}/smoke-test/window-settings.json")
    set_tests_properties(window_smoke PROPERTIES
        TIMEOUT 15 PASS_REGULAR_EXPRESSION "cui: saved")

    if(APPLE)
        add_test(NAME run_script_smoke COMMAND ${CMAKE_COMMAND} -E env
            "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "BUILD_CONFIG=$<CONFIG>"
            "APP_SETTINGS_PATH=${CMAKE_CURRENT_BINARY_DIR}/smoke-test/run-settings.json"
            "CUI_SCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/run-smoke.png"
            CUI_SCREENSHOT_FRAMES=1 ${CMAKE_CURRENT_SOURCE_DIR}/run.sh)
        set_tests_properties(run_script_smoke PROPERTIES
            TIMEOUT 120 PASS_REGULAR_EXPRESSION "cui: saved" RUN_SERIAL TRUE)
    endif()
endif()
