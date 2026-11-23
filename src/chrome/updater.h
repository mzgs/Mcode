#ifndef APP_UPDATER_H
#define APP_UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APP_UPDATE_CHECK_INTERVAL_SEC = 24 * 60 * 60,
};

typedef enum {
    APP_UPDATE_IDLE = 0,
    APP_UPDATE_CHECKING,
    APP_UPDATE_UP_TO_DATE,
    APP_UPDATE_AVAILABLE,
    APP_UPDATE_UPDATING,
    APP_UPDATE_READY_TO_RELAUNCH,
    APP_UPDATE_ERROR,
} app_update_state;

typedef struct {
    app_update_state state;
    bool has_update;
    char latest_tag[64];
    char latest_version[64];
    char release_url[512];
    char relaunch_path[4096];
    char error[256];
} app_update_snapshot;

typedef struct app_updater app_updater;

/* Pure helpers for validation and version comparison. */
bool app_updater_valid_repo(const char *repo);
bool app_updater_extract_release_tag(const char *url, char *tag, size_t cap);
/* Returns 1 if candidate > current, 0 if equal, -1 if older, -2 on parse error. */
int app_updater_compare_versions(const char *candidate, const char *current);
/* True when at least APP_UPDATE_CHECK_INTERVAL_SEC has elapsed since last_checked (or never checked). */
bool app_updater_check_due(int64_t last_checked, int64_t now);

/* True when APP_GITHUB_REPO is configured with a valid "owner/repo" slug. */
bool app_updater_configured(void);

app_updater *app_updater_create(const char *repo, const char *current_version);
void app_updater_destroy(app_updater *updater);

bool app_updater_check(app_updater *updater);
bool app_updater_start_install(app_updater *updater);
void app_updater_snapshot(app_updater *updater, app_update_snapshot *out);
bool app_updater_is_busy(app_updater *updater);

/* Spawn a detached helper that waits for this process to exit and launches path. */
bool app_updater_relaunch(const char *relaunch_path);

#endif
