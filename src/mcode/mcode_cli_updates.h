#ifndef MCODE_CLI_UPDATES_H
#define MCODE_CLI_UPDATES_H

#include "mcode_agent.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MCODE_CLI_UPD_IDLE = 0,
    MCODE_CLI_UPD_CHECKING,
    MCODE_CLI_UPD_UP_TO_DATE,
    MCODE_CLI_UPD_AVAILABLE,
    MCODE_CLI_UPD_UPDATING,
    MCODE_CLI_UPD_ERROR,
} mcode_cli_update_state;

typedef struct {
    mcode_cli_update_state state;
    bool installed;
    bool update_available;
    bool checking;
    bool updating;
    char installed_version[64];
    char latest_version[64];
    char latest_tag[64];
    char release_url[512];
    char update_cmd_display[256];
    char status_line[160];
    char error[160];
    int64_t checked_epoch;
} mcode_cli_update_info;

typedef struct {
    mcode_cli_update_info items[APP_AGENT_COUNT];
    int available_count;
    int checking_count;
    int updating_count;
} mcode_cli_updates_snapshot;

typedef struct {
    bool ok;
    app_agent_kind agent;
    char installed_version[64];
    char latest_version[64];
    char message[192];
} mcode_cli_update_result;

struct mcode_cli_check_job;
struct mcode_cli_install_job;

typedef struct {
    pthread_mutex_t mu;
    bool initialized;
    bool abort_requested;
    bool dirty;
    mcode_cli_update_info items[APP_AGENT_COUNT];
    struct mcode_cli_check_job *check_jobs[APP_AGENT_COUNT];
    struct mcode_cli_install_job *install_jobs[APP_AGENT_COUNT];
    bool pending_post_verify[APP_AGENT_COUNT];
    char pending_post_bin[APP_AGENT_COUNT][APP_CLI_PATH_MAX];
    bool finished_result_ready[APP_AGENT_COUNT];
    mcode_cli_update_result finished_results[APP_AGENT_COUNT];
} mcode_cli_updates;

/* Pure helpers (exposed for unit tests). */
const char *mcode_cli_github_repo(app_agent_kind agent);
bool mcode_cli_extract_version(const char *raw, char *out, size_t cap);
bool mcode_cli_parse_release_redirect(const char *resolved_url,
                                      char *tag_out, size_t tag_cap,
                                      char *ver_out, size_t ver_cap);
/* Format human-readable update command for display or integrated terminal execution.
 * `real_path` may be NULL or empty if symlink resolution was unavailable. */
void mcode_cli_format_update_cmd(app_agent_kind agent,
                                 const char *resolved_path,
                                 const char *real_path,
                                 char *cmd_out, size_t cmd_cap);

/* Lifecycle */
void mcode_cli_updates_init(mcode_cli_updates *u);
void mcode_cli_updates_begin_shutdown(mcode_cli_updates *u);
void mcode_cli_updates_free(mcode_cli_updates *u);

/* Trigger async version + remote release checks.
 * When `local_only` is true, only re-runs `<bin> --version` against the already-known
 * latest version (no network request). */
void mcode_cli_updates_check_all(mcode_cli_updates *u, const mcode_cli_status *clis,
                                 bool local_only);
void mcode_cli_updates_check_agent(mcode_cli_updates *u, const mcode_cli_status *clis,
                                   app_agent_kind agent, bool local_only);

/* Start updating a specific provider CLI asynchronously in the background. */
bool mcode_cli_updates_start_install(mcode_cli_updates *u, const mcode_cli_status *clis,
                                     app_agent_kind agent);

/* Poll background check/update jobs on the UI thread.
 * Returns true if any state changed since the last poll. */
bool mcode_cli_updates_poll(mcode_cli_updates *u, const mcode_cli_status *clis);

/* Consume one finished update notification (if any). Returns true if one was popped. */
bool mcode_cli_updates_take_result(mcode_cli_updates *u, mcode_cli_update_result *out);

/* Thread-safe snapshot for rendering. */
void mcode_cli_updates_snapshot_get(mcode_cli_updates *u, mcode_cli_updates_snapshot *out);

bool mcode_cli_updates_is_busy(mcode_cli_updates *u);
int mcode_cli_updates_available_count(mcode_cli_updates *u);

#endif
