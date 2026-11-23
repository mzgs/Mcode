#ifndef MCODE_AGENT_H
#define MCODE_AGENT_H

#include "mcode_types.h"
#include "settings.h"
#include <stdbool.h>

typedef struct mcode_agent_run mcode_agent_run;

typedef struct {
    bool available[APP_AGENT_COUNT];
    char resolved_path[APP_AGENT_COUNT][APP_CLI_PATH_MAX];
} mcode_cli_status;

/* Detect installed CLI binaries for Claude Code, Codex, and Antigravity CLI. */
void mcode_agent_detect_clis(const app_settings *settings, mcode_cli_status *out);

/* Human-readable display name & short badge for an agent kind. */
const char *mcode_agent_label(app_agent_kind kind);
const char *mcode_agent_short_label(app_agent_kind kind);
const char *mcode_agent_icon(app_agent_kind kind);

/* Start a streaming turn for `thread` using the configured CLI adapter.
 * Appends a new running mcode_turn to `thread` and returns its live run handle. */
mcode_agent_run *mcode_agent_start(mcode_thread *thread, const app_settings *settings,
                                   const char *prompt, char *err, size_t err_cap);

/* Poll queued streaming events from `run` into `thread` on the UI thread.
 * Returns true if any new state arrived (or the run completed).
 * When *out_finished is set to true, caller should call mcode_agent_destroy(run)
 * and clear thread->active_run. */
bool mcode_agent_poll(mcode_agent_run *run, mcode_thread *thread, bool *out_finished);

/* Request stopping an active agent run. */
void mcode_agent_stop(mcode_agent_run *run);

/* Destroy an agent run (stops and joins background thread if still active). */
void mcode_agent_destroy(mcode_agent_run *run);

/* Exposed for unit tests: feed a single output line through a provider's parser
 * directly into a thread & turn (without spawning a subprocess). */
void mcode_agent_parse_line_for_test(app_agent_kind agent, mcode_thread *thread,
                                     mcode_turn *turn, const char *line, bool is_stderr);

#endif
