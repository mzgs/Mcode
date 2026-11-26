#ifndef MCODE_AGENT_H
#define MCODE_AGENT_H

#include "mcode_types.h"
#include "settings.h"
#include <stdbool.h>

typedef struct mcode_agent_run mcode_agent_run;
typedef struct mcode_title_run mcode_title_run;

typedef struct {
    bool available[APP_AGENT_COUNT];
    char resolved_path[APP_AGENT_COUNT][APP_CLI_PATH_MAX];
} mcode_cli_status;

enum {
    MCODE_MAX_MODELS_PER_AGENT = 64,
    MCODE_MAX_EFFORTS_PER_MODEL = 8,
};

typedef struct {
    char id[APP_MODEL_NAME_MAX];
    char label[96];
    char detail[160];
    char default_effort[24];
    char efforts[MCODE_MAX_EFFORTS_PER_MODEL][24];
    int effort_count;
    bool supports_fast;
    char fast_desc[96];
    int64_t context_window;
} mcode_model_option;

typedef struct mcode_model_fetch mcode_model_fetch;
typedef struct mcode_usage_fetch mcode_usage_fetch;
typedef struct mcode_simple_fetch mcode_simple_fetch;

typedef struct {
    mcode_provider_quota quotas[APP_AGENT_COUNT];
    mcode_usage_fetch *codex_fetch;
    mcode_simple_fetch *agy_fetch;
    mcode_simple_fetch *claude_fetch;
    bool agy_refresh_pending;
} mcode_usage_tracker;

typedef struct {
    mcode_model_option items[APP_AGENT_COUNT][MCODE_MAX_MODELS_PER_AGENT];
    int count[APP_AGENT_COUNT];
    char cli_default_model[APP_AGENT_COUNT][APP_MODEL_NAME_MAX];
    char cli_default_effort[APP_AGENT_COUNT][24];
    bool cli_default_fast[APP_AGENT_COUNT];
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
void mcode_model_catalog_begin_shutdown(mcode_model_catalog *cat);
void mcode_model_catalog_free(mcode_model_catalog *cat);

/* Default model configured in settings for `agent` (empty string if none). */
const char *mcode_agent_default_model(const app_settings *settings, app_agent_kind agent);
/* Effective model passed to the CLI (thread override, else settings default, else ""). */
const char *mcode_agent_effective_model(const mcode_thread *thread, const app_settings *settings);
/* Human-readable display name for `model_id` (falls back to `model_id` or "Default"). */
const char *mcode_model_display_label(const mcode_model_catalog *cat, app_agent_kind agent,
                                      const char *model_id);
/* Human-readable label for a reasoning effort slug (e.g. "high" -> "High", "xhigh" -> "XHigh"). */
const char *mcode_effort_display_label(const char *effort);
/* Find a catalog model entry by ID, or NULL if not found. */
const mcode_model_option *mcode_model_find(const mcode_model_catalog *cat, app_agent_kind agent,
                                           const char *model_id);
/* Format the active model's display name and muted effort/tier tag for the chat box.
 * Examples: ("GPT-6.2 Sol", "High"), ("Gemini 4 Neon", "High"), ("Claude Opus", "High"). */
void mcode_model_format_display(const mcode_model_catalog *cat, const mcode_thread *thread,
                                const app_settings *settings,
                                char *name_out, size_t name_cap,
                                char *effort_out, size_t effort_cap);
/* Whether the thread's currently active model supports Fast vs Standard speed tiers.
 * If out_desc is non-NULL, writes the tier description (e.g. "2x speed, increased usage") or "". */
bool mcode_model_supports_fast(const mcode_model_catalog *cat, const mcode_thread *thread,
                               const app_settings *settings, const char **out_desc);
/* Resolved fast-mode state for `thread` (explicit override, else CLI default). */
bool mcode_thread_is_fast_mode(const mcode_model_catalog *cat, const mcode_thread *thread);
/* Effective model context window in tokens (0 if unknown). */
int64_t mcode_model_context_window(const mcode_model_catalog *cat, const mcode_thread *thread,
                                   const app_settings *settings);

/* Live provider quota / account usage tracker (asynchronous Codex app-server RPC,
 * Antigravity `agy -p /usage --output-format json`, Claude `claude auth status` +
 * local session logs, plus real-time rate-limit events from active streams). */
void mcode_usage_tracker_init(mcode_usage_tracker *tracker);
void mcode_usage_tracker_refresh(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                                 bool agy_busy);
void mcode_usage_tracker_refresh_agent(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                                       app_agent_kind agent, bool agy_busy);
bool mcode_usage_tracker_is_loading(const mcode_usage_tracker *tracker);
bool mcode_usage_tracker_poll(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                              bool agy_busy);
void mcode_usage_tracker_begin_shutdown(mcode_usage_tracker *tracker);
void mcode_usage_tracker_free(mcode_usage_tracker *tracker);

/* Pure parsers exposed for unit tests. */
bool mcode_agent_parse_codex_rate_limits_json(const char *json, size_t len,
                                              mcode_provider_quota *out);
bool mcode_agent_parse_codex_account_usage_json(const char *json, size_t len,
                                                const char *today_ymd,
                                                mcode_provider_quota *out);
bool mcode_agent_parse_agy_usage_json(const char *json, size_t len,
                                      mcode_provider_quota *out);
bool mcode_agent_parse_claude_auth_json(const char *json, size_t len,
                                        mcode_provider_quota *out);
int mcode_agent_parse_codex_models_json(const char *json, size_t len,
                                        mcode_model_option *out, int max_out);
int mcode_agent_parse_agy_models_text(const char *text,
                                      mcode_model_option *out, int max_out);
void mcode_agent_parse_codex_config_toml(const char *toml,
                                         char *model_out, size_t model_cap,
                                         char *effort_out, size_t effort_cap,
                                         bool *fast_out);
void mcode_agent_parse_claude_settings_json(const char *json, size_t len,
                                            char *model_out, size_t model_cap,
                                            char *effort_out, size_t effort_cap);

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
 * If `usage` is non-NULL, live provider rate-limit events are merged into it.
 * When *out_finished is set to true, caller should call mcode_agent_destroy(run)
 * and clear thread->active_run. */
bool mcode_agent_poll(mcode_agent_run *run, mcode_thread *thread,
                      mcode_usage_tracker *usage, bool *out_finished);

/* Request stopping an active agent run. */
void mcode_agent_stop(mcode_agent_run *run);

typedef enum {
    MCODE_STEER_FAILED = 0,
    MCODE_STEER_INJECTED,     /* Delivered live into the active turn */
    MCODE_STEER_INTERRUPTING, /* Gracefully interrupting active turn so caller runs steer next */
} mcode_steer_result;

/* Steer an active agent run with a mid-turn redirection message.
 * - Codex RPC (`turn/steer`), Claude CTL (live `stream-json` `user` frame with
 *   `priority: "next"`), and Antigravity ACP (live `session/prompt`): inject live
 *   into the active turn without canceling (falling back to next turn if rejected,
 *   unstarted before exit, or blocked on an approval prompt). */
mcode_steer_result mcode_agent_steer(mcode_agent_run *run, mcode_thread *thread,
                                     const char *message);

/* True if `run` supports injecting steer messages live into the current turn without interrupting. */
bool mcode_agent_supports_live_steer(const mcode_agent_run *run);

/* Respond to the active interactive tool approval prompt (if any) and clear it on `thread`. */
bool mcode_agent_respond_approval(mcode_agent_run *run, mcode_thread *thread,
                                  mcode_approval_decision decision);

/* Destroy an agent run (stops and joins background thread if still active). */
void mcode_agent_destroy(mcode_agent_run *run);

/* Destroy an agent run with an explicit graceful-SIGTERM wait budget in ms (0 = immediate SIGKILL). */
void mcode_agent_destroy_timeout(mcode_agent_run *run, int grace_ms);

/* Start an asynchronous one-shot title generation request using `thread->agent`.
 * Returns a handle owned by the caller (stored on `thread->title_run`), or NULL. */
mcode_title_run *mcode_agent_start_title(const mcode_thread *thread,
                                         const app_settings *settings,
                                         const char *prompt);

/* Poll an active title request. Returns true once finished; if a valid title
 * was extracted, writes it to `out_title` and sets `*out_ok = true`. */
bool mcode_agent_poll_title(mcode_title_run *run, char *out_title, size_t cap, bool *out_ok);

/* Non-blocking request to stop a pending title generation subprocess. */
void mcode_agent_title_stop(mcode_title_run *run);

/* Non-blocking immediate SIGKILL for a disposable title generation subprocess (used on shutdown). */
void mcode_agent_title_kill(mcode_title_run *run);

/* Destroy a title request (stops and joins background process if still running). */
void mcode_agent_title_destroy(mcode_title_run *run);

/* Pure helpers exposed for unit tests:
 * Extract and sanitize a thread title from provider CLI output. */
bool mcode_agent_sanitize_title(const char *raw, char *out, size_t cap);
bool mcode_agent_parse_title_response(app_agent_kind agent, const char *raw, size_t len,
                                      char *out, size_t cap);

/* Classify whether a non-zero command exit represents a benign read-only probe
 * (e.g., rg/grep no matches, ls/stat on missing optional files, git status outside a repo).
 * Pass exit_code < 0 when the numeric exit code is unknown. */
bool mcode_command_is_benign_probe_exit(const char *cmd, const char *output, int exit_code);
bool mcode_tool_is_benign_probe_failure(const char *tool_name, const char *input,
                                        const char *output, int exit_code);

/* Exposed for unit tests: feed a single output line through a provider's parser
 * directly into a thread & turn (without spawning a subprocess). */
void mcode_agent_parse_line_for_test(app_agent_kind agent, mcode_thread *thread,
                                     mcode_turn *turn, const char *line, bool is_stderr);

#endif
