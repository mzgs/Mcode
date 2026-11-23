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

enum {
    MCODE_MAX_MODELS_PER_AGENT = 64,
};

typedef struct {
    char id[APP_MODEL_NAME_MAX];
    char label[96];
    char detail[160];
} mcode_model_option;

typedef struct mcode_model_fetch mcode_model_fetch;

typedef struct {
    mcode_model_option items[APP_AGENT_COUNT][MCODE_MAX_MODELS_PER_AGENT];
    int count[APP_AGENT_COUNT];
    bool loading_agy;
    bool loaded_agy;
    mcode_model_fetch *agy_fetch;
} mcode_model_catalog;

/* Detect installed CLI binaries for Claude Code, Codex, and Antigravity CLI. */
void mcode_agent_detect_clis(const app_settings *settings, mcode_cli_status *out);

/* Initialize / refresh / poll / free the per-agent model catalog.
 * Loads Claude presets, Codex local models cache (~/.codex/models_cache.json),
 * and optionally spawns `agy models` asynchronously when Antigravity CLI is available. */
void mcode_model_catalog_init(mcode_model_catalog *cat);
void mcode_model_catalog_refresh(mcode_model_catalog *cat, const mcode_cli_status *clis);
bool mcode_model_catalog_poll(mcode_model_catalog *cat);
void mcode_model_catalog_free(mcode_model_catalog *cat);

/* Default model configured in settings for `agent` (empty string if none). */
const char *mcode_agent_default_model(const app_settings *settings, app_agent_kind agent);
/* Effective model passed to the CLI (thread override, else settings default, else ""). */
const char *mcode_agent_effective_model(const mcode_thread *thread, const app_settings *settings);
/* Human-readable display name for `model_id` (falls back to `model_id` or "Default"). */
const char *mcode_model_display_label(const mcode_model_catalog *cat, app_agent_kind agent,
                                      const char *model_id);

/* Pure parsers exposed for unit tests. */
int mcode_agent_parse_codex_models_json(const char *json, size_t len,
                                        mcode_model_option *out, int max_out);
int mcode_agent_parse_agy_models_text(const char *text,
                                      mcode_model_option *out, int max_out);

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
