#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_agent.h"
#include "mcode_proc.h"
#include "app_log.h"
#include <yyjson.h>

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <direct.h>
#include <sys/stat.h>
#endif

typedef enum {
    EV_SESSION = 0,
    EV_TEXT_DELTA,
    EV_TEXT_FALLBACK,
    EV_REASONING_DELTA,
    EV_REASONING_DONE,
    EV_TOOL_START,
    EV_TOOL_INPUT_DELTA,
    EV_TOOL_OUTPUT_DELTA,
    EV_TOOL_RESULT,
    EV_IMAGE,
    EV_PLAN_UPDATE,
    EV_PLAN_DELTA,
    EV_PLAN_SET,
    EV_NOTICE,
    EV_APPROVAL_REQUEST,
    EV_APPROVAL_CLEAR,
    EV_QUESTION_REQUEST,
    EV_QUESTION_CLEAR,
    EV_STATUS,
    EV_USAGE,
    EV_RATE_LIMIT,
    EV_STEER_REJECTED,
    EV_ERROR,
    EV_DONE,
} agent_ev_kind;

typedef enum {
    AGENT_PROTO_STREAM = 0,
    AGENT_PROTO_CLAUDE_CTL,
    AGENT_PROTO_CODEX_RPC,
    AGENT_PROTO_AGY_ACP,
} agent_proto_kind;

typedef enum {
    APPROVAL_KIND_NONE = 0,
    APPROVAL_KIND_AGY_ACP,
    APPROVAL_KIND_CLAUDE_TOOL,
    APPROVAL_KIND_CODEX_CMD,
    APPROVAL_KIND_CODEX_FILE,
    APPROVAL_KIND_CODEX_PERMS,
    APPROVAL_KIND_CODEX_LEGACY,
    APPROVAL_KIND_CODEX_ELICIT,
} approval_rpc_kind;

typedef enum {
    QUESTION_KIND_NONE = 0,
    QUESTION_KIND_CODEX,
    QUESTION_KIND_CLAUDE,
} question_rpc_kind;

typedef struct agent_ev {
    agent_ev_kind kind;
    char *text;
    char *tool_id;
    char *tool_name;
    char *tool_detail;
    char *extra_a; /* approval reason / image path */
    char *extra_b; /* approval raw details */
    bool flag; /* failed / killed / can_allow_session / replace_usage */
    bool flag_b; /* steer_interrupted */
    int int_a; /* exit_code / notice_tone / plan_step_count */
    int64_t tok_in;
    int64_t tok_out;
    int64_t tok_cached;
    int64_t tok_ctx;
    int64_t tok_ctx_win;
    double dbl_a; /* cost_usd */
    mcode_provider_quota *quota; /* non-NULL only for EV_RATE_LIMIT */
    mcode_plan_step *plan_steps; /* non-NULL only for EV_PLAN_UPDATE */
    mcode_pending_question *question; /* non-NULL only for EV_QUESTION_REQUEST */
    struct agent_ev *next;
} agent_ev;

typedef struct claude_live_steer {
    char uuid[40];
    char *text;
    bool started;
    bool completed;
} claude_live_steer;

struct mcode_agent_run {
    app_agent_kind agent;
    agent_proto_kind proto;
    mcode_proc *proc;

    /* Bidirectional RPC handshake / turn state */
    char *init_prompt;
    char init_cwd[MCODE_PATH_MAX];
    char init_model[APP_MODEL_NAME_MAX];
    char resolved_model[APP_MODEL_NAME_MAX];
    char init_effort[32];
    mcode_speed_mode init_speed;
    mcode_permission_mode init_perm_mode;
    char init_session_id[MCODE_SESSION_MAX];
    char active_session_id[MCODE_SESSION_MAX];
    char active_turn_id[128];
    int next_rpc_id;
    bool rpc_prompt_sent;
    bool rpc_turn_done;
    bool stop_requested;
    bool steer_interrupting;
    bool deferred_interrupt;
    double interrupt_requested_at;
    bool has_emitted_text;
    bool need_text_sep;
    int64_t codex_turn_in_tokens;
    int64_t codex_turn_out_tokens;
    int64_t codex_turn_cached_tokens;
    int64_t codex_last_ctx_tokens;
    int64_t codex_ctx_window;
    bool codex_emitted_live_usage;
    int64_t claude_last_ctx_tokens;
    bool claude_pass_had_text;
    bool claude_seen_replay_support;
    char claude_init_uuid[40];
    double claude_waiting_steer_at;
    claude_live_steer *claude_steers;
    int claude_steer_count;
    int claude_steer_cap;

    /* Codex & Antigravity ACP live-steer tracking (protected by mu) */
    char **pending_live_steers;
    int pending_live_steer_count;
    int pending_live_steer_cap;
    int64_t inflight_steer_ids[16];
    char *inflight_steer_texts[16];
    int inflight_steer_count;
    int64_t agy_open_prompts[16];
    int agy_open_prompt_count;

    /* Pending approval RPC FIFO queue (protected by mu) */
    struct pending_approval_item *approval_head;
    struct pending_approval_item *approval_tail;

    /* Pending interactive question RPC metadata (protected by mu) */
    bool question_pending;
    question_rpc_kind question_kind;
    bool question_rpc_id_is_str;
    char question_rpc_id_str[128];
    int64_t question_rpc_id_num;
    char *question_payload_json;

    /* Background line assembly */
    mcode_str out_line;
    mcode_str err_line;
    mcode_str stderr_tail;
    bool streamed_text_deltas;
    bool streamed_thinking_deltas;
    char active_tool_id[128];

    /* Thread-safe event queue */
    pthread_mutex_t mu;
    agent_ev *head;
    agent_ev *tail;
    bool finished;
};

static void ev_free(agent_ev *ev) {
    if (!ev) return;
    free(ev->text);
    free(ev->tool_id);
    free(ev->tool_name);
    free(ev->tool_detail);
    free(ev->extra_a);
    free(ev->extra_b);
    free(ev->quota);
    free(ev->plan_steps);
    free(ev->question);
    free(ev);
}

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static void push_ev(mcode_agent_run *run, agent_ev *ev) {
    if (!run || !ev) { ev_free(ev); return; }
    ev->next = NULL;
    pthread_mutex_lock(&run->mu);
    if (run->tail) run->tail->next = ev;
    else run->head = ev;
    run->tail = ev;
    pthread_mutex_unlock(&run->mu);
}

static void emit_simple(mcode_agent_run *run, agent_ev_kind kind, const char *text) {
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = kind;
    ev->text = dup_or_null(text);
    push_ev(run, ev);
}

static void emit_tool_start(mcode_agent_run *run, const char *id, const char *name,
                            const char *detail, const char *input) {
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_TOOL_START;
    ev->tool_id = dup_or_null(id);
    ev->tool_name = dup_or_null(name);
    ev->tool_detail = dup_or_null(detail);
    ev->text = dup_or_null(input);
    push_ev(run, ev);
}

static void emit_tool_result_code(mcode_agent_run *run, const char *id, const char *output,
                                  bool failed, int exit_code) {
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_TOOL_RESULT;
    ev->tool_id = dup_or_null(id);
    ev->text = dup_or_null(output);
    ev->flag = failed;
    ev->int_a = exit_code;
    push_ev(run, ev);
}

static void emit_tool_result(mcode_agent_run *run, const char *id, const char *output, bool failed) {
    emit_tool_result_code(run, id, output, failed, -1);
}

static void emit_image_ev(mcode_agent_run *run, const char *image_path, const char *caption) {
    if (!run || !image_path || !image_path[0]) return;
    emit_simple(run, EV_REASONING_DONE, NULL);
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_IMAGE;
    ev->extra_a = dup_or_null(image_path);
    ev->text = dup_or_null(caption ? caption : "");
    push_ev(run, ev);
}

static bool file_exists_nonempty(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static unsigned char *decode_base64_bytes(const char *src, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!src || !src[0]) return NULL;
    if (strncmp(src, "data:", 5) == 0) {
        const char *comma = strchr(src, ',');
        if (!comma) return NULL;
        src = comma + 1;
    }
    size_t slen = strlen(src);
    if (slen < 4) return NULL;
    size_t max_out = (slen / 4) * 3 + 4;
    unsigned char *out = malloc(max_out);
    if (!out) return NULL;

    size_t w = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=') break;
        if (isspace(c)) continue;
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[w++] = (unsigned char)((acc >> bits) & 0xFFu);
        }
    }
    if (w == 0) {
        free(out);
        return NULL;
    }
    if (out_len) *out_len = w;
    return out;
}

static bool save_base64_image_to_disk(const char *raw_b64, const char *mime_hint,
                                      const char *id_hint, char *out_path, size_t out_cap) {
    if (out_path && out_cap > 0) out_path[0] = '\0';
    if (!raw_b64 || !raw_b64[0] || !out_path || out_cap == 0) return false;

    size_t nbytes = 0;
    unsigned char *bytes = decode_base64_bytes(raw_b64, &nbytes);
    if (!bytes || nbytes < 8) {
        free(bytes);
        return false;
    }

    const char *ext = NULL;
    if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
        ext = "png";
    } else if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        ext = "jpg";
    } else if (mime_hint && (strstr(mime_hint, "png") || strstr(mime_hint, "PNG"))) {
        ext = "png";
    } else if (mime_hint && (strstr(mime_hint, "jpeg") || strstr(mime_hint, "jpg") ||
                             strstr(mime_hint, "JPEG") || strstr(mime_hint, "JPG"))) {
        ext = "jpg";
    }
    if (!ext) {
        free(bytes);
        return false;
    }

    char base_dir[MCODE_PATH_MAX] = "";
    char img_dir[MCODE_PATH_MAX] = "";
    if (app_settings_dir(base_dir, sizeof base_dir, NULL, true) && base_dir[0]) {
        snprintf(img_dir, sizeof img_dir, "%s/generated_images", base_dir);
    } else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0]) tmp = "/tmp";
        snprintf(img_dir, sizeof img_dir, "%s/mcode-images", tmp);
    }
#ifdef _WIN32
    (void)_mkdir(img_dir);
#else
    (void)mkdir(img_dir, 0700);
#endif

    char safe_id[96] = "";
    if (id_hint && id_hint[0]) {
        size_t w = 0;
        for (const char *p = id_hint; *p && w + 1 < sizeof safe_id; ++p) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || c == '-' || c == '_') safe_id[w++] = (char)c;
        }
        safe_id[w] = '\0';
    }
    if (!safe_id[0]) {
        mcode_gen_id(safe_id, sizeof safe_id, "img");
    }

    int pn = snprintf(out_path, out_cap, "%s/%s.%s", img_dir, safe_id, ext);
    if (pn <= 0 || (size_t)pn >= out_cap) {
        out_path[0] = '\0';
        free(bytes);
        return false;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        out_path[0] = '\0';
        free(bytes);
        return false;
    }
    bool ok = (fwrite(bytes, 1, nbytes, f) == nbytes);
    if (fclose(f) != 0) ok = false;
    free(bytes);
    if (!ok) {
        remove(out_path);
        out_path[0] = '\0';
        return false;
    }
    return true;
}

static const char *jstr(yyjson_val *obj, const char *key);

static bool emit_json_image_content(mcode_agent_run *run, yyjson_val *item, const char *id_hint) {
    if (!run || !yyjson_is_obj(item)) return false;
    const char *t = jstr(item, "type");
    if (!t || strcmp(t, "image") != 0) return false;

    const char *path = jstr(item, "path");
    if (!path) path = jstr(item, "savedPath");
    if (!path) path = jstr(item, "uri");
    if (path && strncmp(path, "file://", 7) == 0) path += 7;
    if (path && file_exists_nonempty(path)) {
        emit_image_ev(run, path, "");
        return true;
    }

    const char *b64 = jstr(item, "data");
    const char *mime = jstr(item, "mimeType");
    if (!mime) mime = jstr(item, "media_type");
    yyjson_val *src = yyjson_obj_get(item, "source");
    if (yyjson_is_obj(src)) {
        if (!b64) b64 = jstr(src, "data");
        if (!mime) mime = jstr(src, "media_type");
        if (!mime) mime = jstr(src, "mimeType");
    }
    if (b64 && b64[0]) {
        char saved[MCODE_PATH_MAX] = "";
        if (save_base64_image_to_disk(b64, mime, id_hint, saved, sizeof saved)) {
            emit_image_ev(run, saved, "");
            return true;
        }
    }
    return false;
}

static void emit_tool_output_delta(mcode_agent_run *run, const char *id, const char *delta) {
    if (!run || !id || !id[0] || !delta || !delta[0]) return;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_TOOL_OUTPUT_DELTA;
    ev->tool_id = dup_or_null(id);
    ev->text = dup_or_null(delta);
    push_ev(run, ev);
}

static void emit_notice_ev(mcode_agent_run *run, const char *text, cui_tone tone) {
    if (!run || !text || !text[0]) return;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_NOTICE;
    ev->text = dup_or_null(text);
    ev->int_a = (int)tone;
    push_ev(run, ev);
}

static void emit_plan_update_ev(mcode_agent_run *run, const char *title,
                                const mcode_plan_step *steps, int count) {
    if (!run) return;
    if (count < 0) count = 0;
    if (count > MCODE_MAX_PLAN_STEPS) count = MCODE_MAX_PLAN_STEPS;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_PLAN_UPDATE;
    ev->text = dup_or_null(title ? title : "");
    ev->int_a = count;
    if (count > 0 && steps) {
        ev->plan_steps = calloc((size_t)count, sizeof *ev->plan_steps);
        if (!ev->plan_steps) {
            ev_free(ev);
            return;
        }
        memcpy(ev->plan_steps, steps, (size_t)count * sizeof *ev->plan_steps);
    }
    push_ev(run, ev);
}

static void emit_question_request_ev(mcode_agent_run *run, const mcode_pending_question *q) {
    if (!run || !q || q->count <= 0) return;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_QUESTION_REQUEST;
    ev->question = calloc(1, sizeof *ev->question);
    if (!ev->question) {
        ev_free(ev);
        return;
    }
    *ev->question = *q;
    ev->question->active = true;
    push_ev(run, ev);
    emit_simple(run, EV_STATUS, "Waiting for answer...");
}

static cui_task_status parse_plan_step_status(const char *s) {
    if (!s || !s[0]) return CUI_TASK_PENDING;
    if (strcmp(s, "completed") == 0 || strcmp(s, "done") == 0) return CUI_TASK_DONE;
    if (strcmp(s, "inProgress") == 0 || strcmp(s, "in_progress") == 0 ||
        strcmp(s, "running") == 0) return CUI_TASK_RUNNING;
    if (strcmp(s, "failed") == 0 || strcmp(s, "error") == 0) return CUI_TASK_FAILED;
    if (strcmp(s, "skipped") == 0 || strcmp(s, "cancelled") == 0 ||
        strcmp(s, "canceled") == 0) return CUI_TASK_SKIPPED;
    return CUI_TASK_PENDING;
}

static bool parse_questions_array(yyjson_val *q_arr, bool is_claude, mcode_pending_question *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!yyjson_is_arr(q_arr)) return false;

    size_t idx, max;
    yyjson_val *qv;
    yyjson_arr_foreach(q_arr, idx, max, qv) {
        if (out->count >= MCODE_MAX_QUESTIONS || !yyjson_is_obj(qv)) continue;
        const char *qtext = jstr(qv, "question");
        if (!qtext || !qtext[0]) continue;
        mcode_question_item *qi = &out->items[out->count];
        const char *qid = jstr(qv, "id");
        const char *hdr = jstr(qv, "header");
        snprintf(qi->id, sizeof qi->id, "%s", (qid && qid[0]) ? qid : qtext);
        snprintf(qi->header, sizeof qi->header, "%s", (hdr && hdr[0]) ? hdr : "Question");
        snprintf(qi->question, sizeof qi->question, "%s", qtext);

        yyjson_val *ms = yyjson_obj_get(qv, "multiSelect");
        if (!ms) ms = yyjson_obj_get(qv, "multi_select");
        qi->multi_select = yyjson_is_bool(ms) && yyjson_get_bool(ms);

        yyjson_val *io = yyjson_obj_get(qv, "isOther");
        qi->allow_other = is_claude ? true : (yyjson_is_bool(io) && yyjson_get_bool(io));

        yyjson_val *isec = yyjson_obj_get(qv, "isSecret");
        qi->is_secret = yyjson_is_bool(isec) && yyjson_get_bool(isec);

        yyjson_val *opts = yyjson_obj_get(qv, "options");
        if (yyjson_is_arr(opts)) {
            size_t oi, omax;
            yyjson_val *ov;
            yyjson_arr_foreach(opts, oi, omax, ov) {
                if (qi->option_count >= MCODE_MAX_QUESTION_OPTIONS || !yyjson_is_obj(ov)) continue;
                const char *lbl = jstr(ov, "label");
                if (!lbl || !lbl[0]) continue;
                const char *desc = jstr(ov, "description");
                snprintf(qi->options[qi->option_count].label,
                         sizeof qi->options[qi->option_count].label, "%s", lbl);
                if (desc && desc[0]) {
                    snprintf(qi->options[qi->option_count].description,
                             sizeof qi->options[qi->option_count].description, "%s", desc);
                }
                qi->option_count++;
            }
        }
        if (qi->option_count == 0) {
            qi->allow_other = true;
        }
        out->count++;
    }
    out->active = (out->count > 0);
    return out->active;
}

/* ---- Multimodal local image extraction & base64 encoding for prompt inputs ---- */
enum {
    MCODE_PROMPT_IMAGE_MAX = 6,
    MCODE_PROMPT_IMAGE_MAX_BYTES = 15 * 1024 * 1024,
};

static bool ag_starts_with_icase(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        ++s;
        ++prefix;
    }
    return true;
}

static int ag_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool ag_has_image_ext(const char *path) {
    if (!path || !path[0]) return false;
    size_t len = 0;
    while (path[len] && path[len] != '?' && path[len] != '#') ++len;
    while (len > 0 && isspace((unsigned char)path[len - 1])) --len;
    if (len < 4) return false;
    const char *dot = NULL;
    for (size_t i = len; i > 0; --i) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') break;
        if (path[i - 1] == '.') { dot = path + (i - 1); break; }
    }
    if (!dot) return false;
    size_t elen = (size_t)((path + len) - (dot + 1));
    if (elen == 3) return ag_starts_with_icase(dot + 1, "png") || ag_starts_with_icase(dot + 1, "jpg");
    if (elen == 4) return ag_starts_with_icase(dot + 1, "jpeg");
    return false;
}

static bool ag_probe_image_file(const char *path, const char **out_mime) {
    if (out_mime) *out_mime = NULL;
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 8 || st.st_size > MCODE_PROMPT_IMAGE_MAX_BYTES) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char hdr[8] = { 0 };
    size_t nr = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (nr < 4) return false;
    if (hdr[0] == 0x89 && hdr[1] == 0x50 && hdr[2] == 0x4E && hdr[3] == 0x47) {
        if (out_mime) *out_mime = "image/png";
        return true;
    }
    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) {
        if (out_mime) *out_mime = "image/jpeg";
        return true;
    }
    return false;
}

static bool ag_resolve_image_candidate(const char *raw, const char *workdir,
                                       char *out_path, size_t out_cap) {
    if (!raw || !raw[0] || !out_path || out_cap == 0) return false;
    out_path[0] = '\0';
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (!raw[0]) return false;
    if (!ag_has_image_ext(raw)) return false;

    char buf[MCODE_PATH_MAX] = "";
    if (ag_starts_with_icase(raw, "file://")) {
        const char *p = raw + 7;
        if (ag_starts_with_icase(p, "localhost/")) p += 9;
#ifdef _WIN32
        if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') ++p;
#endif
        size_t w = 0;
        while (*p && *p != '?' && *p != '#' && w + 1 < sizeof buf) {
            if (*p == '%' && p[1] && p[2]) {
                int hi = ag_hex_nibble(p[1]);
                int lo = ag_hex_nibble(p[2]);
                if (hi >= 0 && lo >= 0) {
                    buf[w++] = (char)((hi << 4) | lo);
                    p += 3;
                    continue;
                }
            }
            buf[w++] = *p++;
        }
        buf[w] = '\0';
    } else {
        /* Reject non-file URI schemes */
        if (isalpha((unsigned char)raw[0])) {
            const char *q = raw + 1;
            while (*q && (isalnum((unsigned char)*q) || *q == '+' || *q == '-' || *q == '.')) ++q;
            if (q[0] == ':' && q[1] == '/' && q[2] == '/') return false;
        }
        snprintf(buf, sizeof buf, "%s", raw);
    }
    if (!buf[0]) return false;

    if (buf[0] == '~' && (buf[1] == '/' || buf[1] == '\\')) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
        if (home && home[0]) {
            snprintf(out_path, out_cap, "%s%s", home, buf + 1);
        } else {
            snprintf(out_path, out_cap, "%s", buf);
        }
    } else {
        bool is_abs = (buf[0] == '/' || buf[0] == '\\' ||
                       (isalpha((unsigned char)buf[0]) && buf[1] == ':' &&
                        (buf[2] == '/' || buf[2] == '\\')));
        if (!is_abs && workdir && workdir[0]) {
            size_t wlen = strlen(workdir);
            while (wlen > 1 && (workdir[wlen - 1] == '/' || workdir[wlen - 1] == '\\')) --wlen;
            const char *rel = buf;
            if (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
            snprintf(out_path, out_cap, "%.*s/%s", (int)wlen, workdir, rel);
        } else {
            snprintf(out_path, out_cap, "%s", buf);
        }
    }
    return ag_probe_image_file(out_path, NULL);
}

static void ag_add_unique_img(const char *raw, const char *workdir,
                              char out[][MCODE_PATH_MAX], int *count, int max_imgs) {
    if (*count >= max_imgs || !raw || !raw[0]) return;
    char resolved[MCODE_PATH_MAX] = "";
    if (!ag_resolve_image_candidate(raw, workdir, resolved, sizeof resolved)) return;
    for (int i = 0; i < *count; ++i) {
        if (strcmp(out[i], resolved) == 0) return;
    }
    snprintf(out[(*count)++], MCODE_PATH_MAX, "%s", resolved);
}

static bool ag_is_tok_delim(unsigned char c) {
    return isspace(c) || c == ')' || c == ']' || c == '>' || c == '"' || c == '\'' || c == ',' || c == ';';
}

static size_t ag_find_spaced_img_end(const char *s, size_t len, size_t start) {
    static const char *const k_exts[] = { ".png", ".jpg", ".jpeg" };
    size_t line_end = start;
    while (line_end < len && s[line_end] != '\n' && s[line_end] != '\r') ++line_end;
    if (line_end - start >= MCODE_PATH_MAX) line_end = start + MCODE_PATH_MAX - 1;
    for (size_t p = start; p < line_end; ++p) {
        if (s[p] != '.') continue;
        for (size_t ei = 0; ei < sizeof k_exts / sizeof k_exts[0]; ++ei) {
            size_t elen = strlen(k_exts[ei]);
            if (p + elen <= line_end && ag_starts_with_icase(s + p, k_exts[ei])) {
                size_t after = p + elen;
                if (after == line_end || ag_is_tok_delim((unsigned char)s[after])) return after;
            }
        }
    }
    return 0;
}

static int extract_prompt_local_images(const char *s, const char *workdir,
                                       char out[][MCODE_PATH_MAX], int max_imgs) {
    int count = 0;
    if (!s || !s[0] || max_imgs <= 0) return 0;
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len && count < max_imgs) {
        if (s[i] == '@' && (i == 0 || isspace((unsigned char)s[i - 1]) || s[i - 1] == '(') &&
            i + 1 < len && !isspace((unsigned char)s[i + 1])) {
            size_t start = i + 1;
            if (s[start] == '"' || s[start] == '\'') {
                char q = s[start++];
                size_t end = start;
                while (end < len && s[end] != q && s[end] != '\n' && s[end] != '\r') ++end;
                char tok[MCODE_PATH_MAX];
                size_t tl = end - start;
                if (tl >= sizeof tok) tl = sizeof tok - 1;
                memcpy(tok, s + start, tl);
                tok[tl] = '\0';
                ag_add_unique_img(tok, workdir, out, &count, max_imgs);
                i = (end < len && s[end] == q) ? end + 1 : end;
                continue;
            }
            size_t end = start;
            while (end < len && !ag_is_tok_delim((unsigned char)s[end])) ++end;
            char tok[MCODE_PATH_MAX];
            size_t tl = end - start;
            if (tl >= sizeof tok) tl = sizeof tok - 1;
            memcpy(tok, s + start, tl);
            tok[tl] = '\0';
            while (tl > 0 && (tok[tl - 1] == '.' || tok[tl - 1] == ':')) tok[--tl] = '\0';
            int before = count;
            ag_add_unique_img(tok, workdir, out, &count, max_imgs);
            if (count == before) {
                size_t sp_end = ag_find_spaced_img_end(s, len, start);
                if (sp_end > end) {
                    tl = sp_end - start;
                    if (tl >= sizeof tok) tl = sizeof tok - 1;
                    memcpy(tok, s + start, tl);
                    tok[tl] = '\0';
                    ag_add_unique_img(tok, workdir, out, &count, max_imgs);
                    if (count > before) end = sp_end;
                }
            }
            i = end;
            continue;
        }
        bool bound = (i == 0 || isspace((unsigned char)s[i - 1]) || s[i - 1] == '(' ||
                      s[i - 1] == '<' || s[i - 1] == '"' || s[i - 1] == '\'');
        if (bound) {
            bool is_furi = ag_starts_with_icase(s + i, "file://");
            bool is_home = (s[i] == '~' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '\\'));
            bool is_abs = (s[i] == '/' && i + 1 < len && !isspace((unsigned char)s[i + 1]) && s[i + 1] != '/');
            if (is_furi || is_home || is_abs) {
                size_t end = i;
                while (end < len && !ag_is_tok_delim((unsigned char)s[end])) ++end;
                char tok[MCODE_PATH_MAX];
                size_t tl = end - i;
                if (tl >= sizeof tok) tl = sizeof tok - 1;
                memcpy(tok, s + i, tl);
                tok[tl] = '\0';
                while (tl > 0 && (tok[tl - 1] == '.' || tok[tl - 1] == ':')) tok[--tl] = '\0';
                int before = count;
                ag_add_unique_img(tok, workdir, out, &count, max_imgs);
                if (count == before && !is_furi) {
                    size_t sp_end = ag_find_spaced_img_end(s, len, i);
                    if (sp_end > end) {
                        tl = sp_end - i;
                        if (tl >= sizeof tok) tl = sizeof tok - 1;
                        memcpy(tok, s + i, tl);
                        tok[tl] = '\0';
                        ag_add_unique_img(tok, workdir, out, &count, max_imgs);
                        if (count > before) end = sp_end;
                    }
                }
                i = end;
                continue;
            }
        }
        ++i;
    }
    return count;
}

static bool encode_file_base64(const char *path, const char **out_mime, char **out_b64) {
    if (out_mime) *out_mime = NULL;
    if (out_b64) *out_b64 = NULL;
    const char *mime = NULL;
    if (!ag_probe_image_file(path, &mime)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > MCODE_PROMPT_IMAGE_MAX_BYTES) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    unsigned char *raw = malloc((size_t)sz);
    if (!raw) { fclose(f); return false; }
    size_t nr = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) { free(raw); return false; }

    static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((nr + 2) / 3) * 4;
    char *b64 = malloc(out_len + 1);
    if (!b64) { free(raw); return false; }
    size_t w = 0;
    for (size_t i = 0; i < nr; i += 3) {
        uint32_t v = ((uint32_t)raw[i] << 16);
        if (i + 1 < nr) v |= ((uint32_t)raw[i + 1] << 8);
        if (i + 2 < nr) v |= (uint32_t)raw[i + 2];
        b64[w++] = k_b64[(v >> 18) & 63];
        b64[w++] = k_b64[(v >> 12) & 63];
        b64[w++] = (i + 1 < nr) ? k_b64[(v >> 6) & 63] : '=';
        b64[w++] = (i + 2 < nr) ? k_b64[v & 63] : '=';
    }
    b64[w] = '\0';
    free(raw);
    if (out_mime) *out_mime = mime;
    if (out_b64) *out_b64 = b64;
    else free(b64);
    return true;
}

typedef struct pending_approval_item {
    approval_rpc_kind kind;
    bool rpc_id_is_str;
    char rpc_id_str[128];
    int64_t rpc_id_num;
    char allow_opt[64];
    char allow_always_opt[64];
    char deny_opt[64];
    bool codex_use_amendment;
    char *payload_json;
    char *title;
    char *tool_name;
    char *command;
    char *reason;
    char *details;
    bool can_allow_session;
    struct pending_approval_item *next;
} pending_approval_item;

static void approval_item_free(pending_approval_item *it) {
    if (!it) return;
    free(it->payload_json);
    free(it->title);
    free(it->tool_name);
    free(it->command);
    free(it->reason);
    free(it->details);
    free(it);
}

static void approval_queue_clear_locked(mcode_agent_run *run) {
    pending_approval_item *cur = run->approval_head;
    run->approval_head = NULL;
    run->approval_tail = NULL;
    while (cur) {
        pending_approval_item *next = cur->next;
        approval_item_free(cur);
        cur = next;
    }
}

static agent_ev *approval_make_ev(const pending_approval_item *it) {
    if (!it) return NULL;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return NULL;
    ev->kind = EV_APPROVAL_REQUEST;
    ev->text = dup_or_null(it->title);
    ev->tool_name = dup_or_null(it->tool_name);
    ev->tool_detail = dup_or_null(it->command);
    ev->extra_a = dup_or_null(it->reason);
    ev->extra_b = dup_or_null(it->details);
    ev->flag = it->can_allow_session;
    return ev;
}

static void approval_item_set_rpc_id(pending_approval_item *it, yyjson_val *id_val) {
    if (!it) return;
    it->rpc_id_is_str = false;
    it->rpc_id_str[0] = '\0';
    it->rpc_id_num = 0;
    if (yyjson_is_str(id_val)) {
        it->rpc_id_is_str = true;
        snprintf(it->rpc_id_str, sizeof it->rpc_id_str, "%s", yyjson_get_str(id_val));
    } else if (yyjson_is_int(id_val)) {
        it->rpc_id_num = yyjson_get_sint(id_val);
    } else if (yyjson_is_num(id_val)) {
        it->rpc_id_num = (int64_t)yyjson_get_num(id_val);
    }
}

static bool approval_item_matches_id(const pending_approval_item *it, yyjson_val *id_val,
                                     const char *str_id) {
    if (!it) return false;
    if (str_id && str_id[0]) {
        return it->rpc_id_is_str && strcmp(it->rpc_id_str, str_id) == 0;
    }
    if (!id_val || yyjson_is_null(id_val)) return true;
    if (yyjson_is_str(id_val)) {
        return it->rpc_id_is_str && strcmp(it->rpc_id_str, yyjson_get_str(id_val)) == 0;
    }
    if (yyjson_is_int(id_val)) {
        return !it->rpc_id_is_str && it->rpc_id_num == yyjson_get_sint(id_val);
    }
    if (yyjson_is_num(id_val)) {
        return !it->rpc_id_is_str && it->rpc_id_num == (int64_t)yyjson_get_num(id_val);
    }
    return false;
}

static void approval_queue_push(mcode_agent_run *run, pending_approval_item *it) {
    if (!run || !it) {
        approval_item_free(it);
        return;
    }
    agent_ev *ev = approval_make_ev(it);
    pthread_mutex_lock(&run->mu);
    bool was_empty = (run->approval_head == NULL);
    it->next = NULL;
    if (run->approval_tail) run->approval_tail->next = it;
    else run->approval_head = it;
    run->approval_tail = it;
    pthread_mutex_unlock(&run->mu);

    if (was_empty && ev) {
        push_ev(run, ev);
        emit_simple(run, EV_STATUS, "Waiting for approval...");
    } else {
        ev_free(ev);
    }
}

static void approval_queue_cancel(mcode_agent_run *run, yyjson_val *id_val, const char *str_id) {
    if (!run) return;
    bool head_changed = false;
    agent_ev *next_ev = NULL;
    pthread_mutex_lock(&run->mu);
    pending_approval_item *prev = NULL;
    pending_approval_item *cur = run->approval_head;
    while (cur) {
        pending_approval_item *next = cur->next;
        if (approval_item_matches_id(cur, id_val, str_id)) {
            if (!prev) {
                run->approval_head = next;
                head_changed = true;
            } else {
                prev->next = next;
            }
            if (cur == run->approval_tail) {
                run->approval_tail = prev;
            }
            approval_item_free(cur);
            cur = next;
            continue;
        }
        prev = cur;
        cur = next;
    }
    if (head_changed && run->approval_head) {
        next_ev = approval_make_ev(run->approval_head);
    }
    pthread_mutex_unlock(&run->mu);

    if (head_changed) {
        if (next_ev) {
            push_ev(run, next_ev);
            emit_simple(run, EV_STATUS, "Waiting for approval...");
        } else {
            emit_simple(run, EV_APPROVAL_CLEAR, NULL);
        }
    }
}

static bool send_json_doc(mcode_proc *proc, yyjson_mut_doc *doc) {
    if (!proc || !doc) return false;
    size_t len = 0;
    char *s = yyjson_mut_write(doc, 0, &len);
    if (!s) return false;
    char *line = malloc(len + 2);
    if (!line) {
        free(s);
        return false;
    }
    memcpy(line, s, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    free(s);
    bool ok = mcode_proc_write(proc, line, len + 1);
    free(line);
    return ok;
}

static void rpc_add_saved_id(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                             bool is_str, const char *id_str, int64_t id_num) {
    if (is_str) {
        yyjson_mut_obj_add_strcpy(doc, obj, "id", id_str ? id_str : "");
    } else {
        yyjson_mut_obj_add_sint(doc, obj, "id", id_num);
    }
}

static const char *jstr(yyjson_val *obj, const char *key) {
    if (!yyjson_is_obj(obj)) return NULL;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
}

static int64_t ji64(yyjson_val *obj, const char *key) {
    if (!yyjson_is_obj(obj)) return 0;
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (yyjson_is_sint(v)) return yyjson_get_sint(v);
    if (yyjson_is_uint(v)) return (int64_t)yyjson_get_uint(v);
    if (yyjson_is_num(v)) return (int64_t)yyjson_get_num(v);
    return 0;
}

static int clamp_percent(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

static void parse_codex_rate_window(yyjson_val *w, mcode_rate_window *out) {
    if (!yyjson_is_obj(w) || !out) return;
    yyjson_val *pct_v = yyjson_obj_get(w, "usedPercent");
    if (!yyjson_is_num(pct_v)) return;
    out->available = true;
    out->used_percent = clamp_percent((int)(yyjson_get_num(pct_v) + 0.5));
    int64_t mins = ji64(w, "windowDurationMins");
    if (mins > 0) out->window_mins = mins;
    int64_t resets = ji64(w, "resetsAt");
    if (resets > 0) out->resets_at = resets;
}

static bool parse_codex_rate_snapshot(yyjson_val *rl, mcode_provider_quota *out) {
    if (!yyjson_is_obj(rl) || !out) return false;
    bool got = false;
    const char *plan = jstr(rl, "planType");
    if (plan && plan[0]) {
        snprintf(out->plan, sizeof out->plan, "%s", plan);
    }
    yyjson_val *prim = yyjson_obj_get(rl, "primary");
    if (yyjson_is_obj(prim)) {
        parse_codex_rate_window(prim, &out->primary);
        if (out->primary.available) got = true;
    }
    yyjson_val *sec = yyjson_obj_get(rl, "secondary");
    if (yyjson_is_obj(sec)) {
        parse_codex_rate_window(sec, &out->secondary);
        if (out->secondary.available) got = true;
    }
    yyjson_val *cred = yyjson_obj_get(rl, "credits");
    if (yyjson_is_obj(cred)) {
        yyjson_val *hc = yyjson_obj_get(cred, "hasCredits");
        yyjson_val *un = yyjson_obj_get(cred, "unlimited");
        if (yyjson_is_bool(hc)) out->has_credits = yyjson_get_bool(hc);
        if (yyjson_is_bool(un)) out->unlimited_credits = yyjson_get_bool(un);
        const char *bal = jstr(cred, "balance");
        if (bal && bal[0]) {
            snprintf(out->credits_balance, sizeof out->credits_balance, "%s", bal);
        }
    }
    if (got) {
        out->available = true;
        out->updated_mono = mcode_now_mono();
        out->updated_epoch = mcode_now_epoch();
        out->error[0] = '\0';
    }
    return got;
}

static void emit_usage_ev(mcode_agent_run *run, int64_t in_tok, int64_t out_tok,
                          int64_t cached_tok, int64_t ctx_tok, int64_t ctx_win,
                          double cost_usd, bool replace) {
    agent_ev *uev = calloc(1, sizeof *uev);
    if (!uev) return;
    uev->kind = EV_USAGE;
    uev->tok_in = in_tok;
    uev->tok_out = out_tok;
    uev->tok_cached = cached_tok;
    uev->tok_ctx = ctx_tok;
    uev->tok_ctx_win = ctx_win;
    uev->dbl_a = cost_usd;
    uev->flag = replace;
    push_ev(run, uev);
}

static void emit_rate_limit_ev(mcode_agent_run *run, const mcode_provider_quota *quota) {
    if (!run || !quota || !quota->available) return;
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_RATE_LIMIT;
    ev->quota = malloc(sizeof *ev->quota);
    if (!ev->quota) {
        free(ev);
        return;
    }
    *ev->quota = *quota;
    push_ev(run, ev);
}

static void summarize_command_detail(const char *cmd, char *detail, size_t cap) {
    if (!detail || cap == 0) return;
    detail[0] = '\0';
    if (!cmd || !cmd[0]) return;

    const char *p = cmd;
    while (*p == ' ' || *p == '\t') ++p;
    /* Strip common shell wrapper prefixes like "/bin/zsh -lc " or "bash -lc " */
    static const char *const prefixes[] = {
        "/bin/zsh -lc ", "/bin/bash -lc ", "/bin/sh -c ", "zsh -lc ", "bash -lc ", "sh -c "
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], plen) == 0) {
            p += plen;
            break;
        }
    }
    char quote = 0;
    if (*p == '\'' || *p == '"') {
        quote = *p++;
    }

    size_t max_len = cap - 1;
    if (max_len > 92) max_len = 92;
    size_t w = 0;
    bool truncated = false;
    while (*p && w < max_len) {
        if (*p == '\n' || *p == '\r') {
            truncated = true;
            break;
        }
        if (quote && *p == quote && p[1] == '\0') break;
        detail[w++] = *p++;
    }
    if (*p && !truncated) {
        if (!(quote && *p == quote && p[1] == '\0')) truncated = true;
    }
    while (w > 0 && (detail[w - 1] == ' ' || detail[w - 1] == '\t')) --w;
    if (truncated && w + 3 < cap) {
        detail[w++] = '.';
        detail[w++] = '.';
        detail[w++] = '.';
    }
    detail[w] = '\0';
}

static void summarize_tool_input(yyjson_val *input, char *detail, size_t cap) {
    if (!detail || cap == 0) return;
    detail[0] = '\0';
    if (!yyjson_is_obj(input)) return;
    static const char *const keys[] = {
        "file_path", "path", "Path", "AbsolutePath", "TargetFile", "DirectoryPath", "SearchPath",
        "command", "cmd", "CommandLine",
        "pattern", "Pattern", "query", "Query", "url", "Url", "description", "prompt", "Prompt"
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        const char *v = jstr(input, keys[i]);
        if (v && v[0]) {
            summarize_command_detail(v, detail, cap);
            return;
        }
    }
}

static char *stringify_json_val(yyjson_val *val) {
    if (!val) return NULL;
    if (yyjson_is_str(val)) return strdup(yyjson_get_str(val));
    return yyjson_val_write(val, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL);
}

/* ---- Claude Code stream-json parser ---- */

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Generate an RFC-4122 v4 UUID string. */
static void make_uuid_v4(uint64_t seq, const void *salt, char out[40]) {
    uint64_t mono_ns = (uint64_t)(mcode_now_mono() * 1e9);
    uint64_t epoch_s = (uint64_t)mcode_now_epoch();
    uint64_t seed = mono_ns ^ (epoch_s << 21) ^ ((uint64_t)(uintptr_t)salt) ^ (seq * 0x9e3779b97f4a7c15ULL);
    uint64_t a = splitmix64_next(&seed);
    uint64_t b = splitmix64_next(&seed);
    /* Set RFC-4122 version 4 and variant 10xx */
    a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
    snprintf(out, 40, "%08x-%04x-%04x-%04x-%012llx",
             (unsigned)((a >> 32) & 0xffffffffU),
             (unsigned)((a >> 16) & 0xffffU),
             (unsigned)(a & 0xffffU),
             (unsigned)((b >> 48) & 0xffffU),
             (unsigned long long)(b & 0x0000ffffffffffffULL));
}

/* Generate an RFC-4122 v4 UUID string (caller must hold run->mu). */
static void claude_make_uuid_locked(mcode_agent_run *run, char out[40]) {
    uint64_t seq = (uint64_t)(++run->next_rpc_id);
    make_uuid_v4(seq, run, out);
}

static bool claude_ctl_send_user_msg(mcode_agent_run *run, const char *uuid,
                                     const char *priority, const char *text) {
    if (!run || !run->proc || !text || !text[0]) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "user");
    if (uuid && uuid[0]) {
        yyjson_mut_obj_add_strcpy(doc, root, "uuid", uuid);
    }
    if (priority && priority[0]) {
        yyjson_mut_obj_add_strcpy(doc, root, "priority", priority);
    }
    yyjson_mut_val *msg = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, msg, "role", "user");

    char img_paths[MCODE_PROMPT_IMAGE_MAX][MCODE_PATH_MAX];
    int img_count = extract_prompt_local_images(text, run->init_cwd, img_paths, MCODE_PROMPT_IMAGE_MAX);
    if (img_count > 0) {
        yyjson_mut_val *carr = yyjson_mut_arr(doc);
        int added = 0;
        for (int ii = 0; ii < img_count; ++ii) {
            const char *mime = NULL;
            char *b64 = NULL;
            if (encode_file_base64(img_paths[ii], &mime, &b64)) {
                yyjson_mut_val *im = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, im, "type", "image");
                yyjson_mut_val *src = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, src, "type", "base64");
                yyjson_mut_obj_add_strcpy(doc, src, "media_type", mime);
                yyjson_mut_obj_add_strcpy(doc, src, "data", b64);
                yyjson_mut_obj_add_val(doc, im, "source", src);
                yyjson_mut_arr_append(carr, im);
                free(b64);
                ++added;
            }
        }
        if (added > 0) {
            yyjson_mut_val *tp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, tp, "type", "text");
            yyjson_mut_obj_add_strcpy(doc, tp, "text", text);
            yyjson_mut_arr_append(carr, tp);
            yyjson_mut_obj_add_val(doc, msg, "content", carr);
        } else {
            yyjson_mut_obj_add_strcpy(doc, msg, "content", text);
        }
    } else {
        yyjson_mut_obj_add_strcpy(doc, msg, "content", text);
    }
    yyjson_mut_obj_add_val(doc, root, "message", msg);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

static void claude_mark_steer_started(mcode_agent_run *run, const char *uuid) {
    if (!run || !uuid || !uuid[0]) return;
    pthread_mutex_lock(&run->mu);
    run->claude_seen_replay_support = true;
    for (int i = 0; i < run->claude_steer_count; ++i) {
        if (strcmp(run->claude_steers[i].uuid, uuid) == 0) {
            run->claude_steers[i].started = true;
            run->claude_waiting_steer_at = 0.0;
            break;
        }
    }
    pthread_mutex_unlock(&run->mu);
}

static void claude_reject_steer_uuid(mcode_agent_run *run, const char *uuid) {
    if (!run || !uuid || !uuid[0]) return;
    char *rejected_text = NULL;
    bool close_now = false;
    pthread_mutex_lock(&run->mu);
    for (int i = 0; i < run->claude_steer_count; ++i) {
        if (strcmp(run->claude_steers[i].uuid, uuid) == 0 && !run->claude_steers[i].started) {
            rejected_text = run->claude_steers[i].text;
            for (int j = i + 1; j < run->claude_steer_count; ++j) {
                run->claude_steers[j - 1] = run->claude_steers[j];
            }
            run->claude_steer_count--;
            break;
        }
    }
    if (run->claude_waiting_steer_at > 0.0) {
        int unstarted = 0;
        for (int i = 0; i < run->claude_steer_count; ++i) {
            if (!run->claude_steers[i].started && !run->claude_steers[i].completed) {
                ++unstarted;
            }
        }
        if (unstarted == 0) {
            run->claude_waiting_steer_at = 0.0;
            run->rpc_turn_done = true;
            close_now = true;
        }
    }
    pthread_mutex_unlock(&run->mu);

    if (rejected_text && rejected_text[0]) {
        emit_simple(run, EV_STEER_REJECTED, rejected_text);
    }
    free(rejected_text);
    if (close_now && run->proc) {
        mcode_proc_close_stdin(run->proc);
    }
}

static void parse_claude_json(mcode_agent_run *run, yyjson_val *root) {
    const char *type = jstr(root, "type");
    const char *sid = jstr(root, "session_id");
    if (sid && sid[0]) emit_simple(run, EV_SESSION, sid);
    if (!type) return;

    if (strcmp(type, "system") == 0) {
        const char *subtype = jstr(root, "subtype");
        if (subtype && strcmp(subtype, "status") == 0) {
            const char *st = jstr(root, "status");
            if (st && strcmp(st, "compacting") == 0) {
                emit_simple(run, EV_STATUS, "Compacting context...");
            }
            if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
                yyjson_val *uuids = yyjson_obj_get(root, "user_message_uuids");
                if (yyjson_is_arr(uuids)) {
                    size_t idx, max;
                    yyjson_val *item;
                    yyjson_arr_foreach(uuids, idx, max, item) {
                        if (yyjson_is_str(item)) {
                            claude_mark_steer_started(run, yyjson_get_str(item));
                        }
                    }
                }
            }
        } else if (subtype && strcmp(subtype, "compact_boundary") == 0) {
            emit_notice_ev(run, "Context compacted", CUI_TONE_NEUTRAL);
        } else if (subtype && (strcmp(subtype, "task_started") == 0 ||
                               strcmp(subtype, "task_progress") == 0 ||
                               strcmp(subtype, "task_notification") == 0)) {
            const char *tid = jstr(root, "task_id");
            const char *desc = jstr(root, "description");
            const char *summary = jstr(root, "summary");
            const char *st = jstr(root, "status");
            if (tid && tid[0]) {
                emit_tool_start(run, tid, "subagent", desc ? desc : "", desc ? desc : "");
                if (strcmp(subtype, "task_notification") == 0) {
                    bool failed = st && (strcmp(st, "failed") == 0 || strcmp(st, "error") == 0);
                    emit_tool_result(run, tid, summary ? summary : (st ? st : "Done"), failed);
                } else {
                    char msg[160];
                    snprintf(msg, sizeof msg, "Subagent: %s",
                             (desc && desc[0]) ? desc : "working...");
                    emit_simple(run, EV_STATUS, msg);
                }
            }
        }
        return;
    }

    if (strcmp(type, "command_lifecycle") == 0 && run->proto == AGENT_PROTO_CLAUDE_CTL) {
        pthread_mutex_lock(&run->mu);
        run->claude_seen_replay_support = true;
        pthread_mutex_unlock(&run->mu);
        const char *cmd_uuid = jstr(root, "command_uuid");
        if (!cmd_uuid || !cmd_uuid[0]) cmd_uuid = jstr(root, "uuid");
        const char *state = jstr(root, "state");
        if (cmd_uuid && cmd_uuid[0] && state) {
            if (strcmp(state, "started") == 0) {
                claude_mark_steer_started(run, cmd_uuid);
            } else if (strcmp(state, "discarded") == 0 || strcmp(state, "refused") == 0) {
                claude_reject_steer_uuid(run, cmd_uuid);
            }
        }
        return;
    }

    if (strcmp(type, "control_cancel_request") == 0 && run->proto == AGENT_PROTO_CLAUDE_CTL) {
        const char *rid = jstr(root, "request_id");
        approval_queue_cancel(run, NULL, (rid && rid[0]) ? rid : NULL);
        bool cleared_q = false;
        pthread_mutex_lock(&run->mu);
        if (run->question_pending &&
            (!rid || !rid[0] || strcmp(run->question_rpc_id_str, rid) == 0)) {
            run->question_pending = false;
            cleared_q = true;
        }
        pthread_mutex_unlock(&run->mu);
        if (cleared_q) emit_simple(run, EV_QUESTION_CLEAR, NULL);
        return;
    }

    if (strcmp(type, "control_request") == 0 && run->proto == AGENT_PROTO_CLAUDE_CTL) {
        const char *rid = jstr(root, "request_id");
        yyjson_val *req = yyjson_obj_get(root, "request");
        const char *subtype = jstr(req, "subtype");
        if (rid && rid[0] && subtype && strcmp(subtype, "can_use_tool") == 0) {
            const char *tname = jstr(req, "tool_name");
            if (!tname || !tname[0]) tname = "tool";
            yyjson_val *inp = yyjson_obj_get(req, "input");

            /* Interactive AskUserQuestion tool: present structured options UI instead of raw JSON */
            if (strcmp(tname, "AskUserQuestion") == 0 && yyjson_is_obj(inp)) {
                mcode_pending_question pq;
                if (parse_questions_array(yyjson_obj_get(inp, "questions"), true, &pq)) {
                    char *compact_inp = yyjson_val_write(inp, 0, NULL);
                    pthread_mutex_lock(&run->mu);
                    run->question_pending = true;
                    run->question_kind = QUESTION_KIND_CLAUDE;
                    run->question_rpc_id_is_str = true;
                    snprintf(run->question_rpc_id_str, sizeof run->question_rpc_id_str, "%s", rid);
                    free(run->question_payload_json);
                    run->question_payload_json = compact_inp ? compact_inp : strdup("{}");
                    pthread_mutex_unlock(&run->mu);
                    emit_question_request_ev(run, &pq);
                    return;
                }
            }

            /* ExitPlanMode: emit proposed plan block if included and ask cleanly */
            bool is_exit_plan = (strcmp(tname, "ExitPlanMode") == 0);
            const char *plan_text = (is_exit_plan && yyjson_is_obj(inp)) ? jstr(inp, "plan") : NULL;
            if (plan_text && plan_text[0]) {
                emit_simple(run, EV_PLAN_SET, plan_text);
            }

            const char *reason = jstr(req, "decision_reason");
            const char *blocked = jstr(req, "blocked_path");
            char detail[256] = "";
            summarize_tool_input(inp, detail, sizeof detail);
            char *compact_inp = inp ? yyjson_val_write(inp, 0, NULL) : strdup("{}");
            char *pretty_inp = is_exit_plan && plan_text && plan_text[0]
                ? strdup(plan_text)
                : stringify_json_val(inp);

            char title[160];
            if (is_exit_plan) {
                snprintf(title, sizeof title, "Approve plan & start implementing?");
            } else {
                snprintf(title, sizeof title, "Allow %s?", tname);
            }
            char full_reason[512] = "";
            if (reason && reason[0] && blocked && blocked[0]) {
                snprintf(full_reason, sizeof full_reason, "%s (%s)", reason, blocked);
            } else if (reason && reason[0]) {
                snprintf(full_reason, sizeof full_reason, "%s", reason);
            } else if (blocked && blocked[0]) {
                snprintf(full_reason, sizeof full_reason, "Path: %s", blocked);
            }

            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CLAUDE_TOOL;
                ait->rpc_id_is_str = true;
                snprintf(ait->rpc_id_str, sizeof ait->rpc_id_str, "%s", rid);
                ait->payload_json = compact_inp ? compact_inp : strdup("{}");
                ait->title = dup_or_null(title);
                ait->tool_name = dup_or_null(tname);
                ait->command = dup_or_null(is_exit_plan ? "" : detail);
                ait->reason = dup_or_null(full_reason);
                ait->details = dup_or_null(pretty_inp ? pretty_inp : "");
                ait->can_allow_session = false;
                approval_queue_push(run, ait);
            } else {
                free(compact_inp);
            }
            free(pretty_inp);
        }
        return;
    }

    if (strcmp(type, "stream_event") == 0) {
        yyjson_val *ev = yyjson_obj_get(root, "event");
        if (!yyjson_is_obj(ev)) return;
        const char *et = jstr(ev, "type");
        if (!et) return;

        if (strcmp(et, "content_block_start") == 0) {
            yyjson_val *cb = yyjson_obj_get(ev, "content_block");
            const char *cbt = jstr(cb, "type");
            if (cbt && strcmp(cbt, "thinking") == 0) {
                emit_simple(run, EV_STATUS, "Thinking...");
            } else if (cbt && strcmp(cbt, "tool_use") == 0) {
                const char *tid = jstr(cb, "id");
                const char *tname = jstr(cb, "name");
                snprintf(run->active_tool_id, sizeof run->active_tool_id, "%s", tid ? tid : "");
                emit_tool_start(run, tid, tname ? tname : "tool", "", "");
                char st[160];
                snprintf(st, sizeof st, "Running %s...", tname ? tname : "tool");
                emit_simple(run, EV_STATUS, st);
            }
        } else if (strcmp(et, "content_block_delta") == 0) {
            yyjson_val *delta = yyjson_obj_get(ev, "delta");
            const char *dt = jstr(delta, "type");
            if (!dt) return;
            if (strcmp(dt, "text_delta") == 0) {
                const char *txt = jstr(delta, "text");
                if (txt && txt[0]) {
                    run->streamed_text_deltas = true;
                    run->claude_pass_had_text = true;
                    emit_simple(run, EV_TEXT_DELTA, txt);
                    emit_simple(run, EV_STATUS, "Responding...");
                }
            } else if (strcmp(dt, "thinking_delta") == 0) {
                const char *th = jstr(delta, "thinking");
                if (th && th[0]) {
                    run->streamed_thinking_deltas = true;
                    emit_simple(run, EV_REASONING_DELTA, th);
                }
            } else if (strcmp(dt, "input_json_delta") == 0) {
                const char *pj = jstr(delta, "partial_json");
                if (pj && pj[0] && run->active_tool_id[0]) {
                    agent_ev *aev = calloc(1, sizeof *aev);
                    if (aev) {
                        aev->kind = EV_TOOL_INPUT_DELTA;
                        aev->tool_id = strdup(run->active_tool_id);
                        aev->text = strdup(pj);
                        push_ev(run, aev);
                    }
                }
            }
        } else if (strcmp(et, "content_block_stop") == 0) {
            emit_simple(run, EV_REASONING_DONE, NULL);
            run->active_tool_id[0] = '\0';
        }
        return;
    }

    if (strcmp(type, "assistant") == 0) {
        yyjson_val *msg = yyjson_obj_get(root, "message");
        yyjson_val *msg_usage = yyjson_is_obj(msg) ? yyjson_obj_get(msg, "usage") : NULL;
        if (yyjson_is_obj(msg_usage)) {
            int64_t ctx = ji64(msg_usage, "input_tokens")
                        + ji64(msg_usage, "cache_read_input_tokens")
                        + ji64(msg_usage, "cache_creation_input_tokens");
            if (ctx > 0) run->claude_last_ctx_tokens = ctx;
        }
        yyjson_val *content = yyjson_is_obj(msg) ? yyjson_obj_get(msg, "content") : NULL;
        if (!yyjson_is_arr(content)) return;

        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(content, idx, max, item) {
            const char *it = jstr(item, "type");
            if (!it) continue;
            if (strcmp(it, "text") == 0) {
                const char *txt = jstr(item, "text");
                if (txt && txt[0] && !run->streamed_text_deltas) {
                    run->claude_pass_had_text = true;
                    emit_simple(run, EV_TEXT_DELTA, txt);
                }
            } else if (strcmp(it, "thinking") == 0) {
                const char *th = jstr(item, "thinking");
                if (th && th[0] && !run->streamed_thinking_deltas) {
                    emit_simple(run, EV_REASONING_DELTA, th);
                    emit_simple(run, EV_REASONING_DONE, NULL);
                }
            } else if (strcmp(it, "tool_use") == 0) {
                const char *tid = jstr(item, "id");
                const char *tname = jstr(item, "name");
                yyjson_val *inp = yyjson_obj_get(item, "input");
                if (tname && strcmp(tname, "TodoWrite") == 0 && yyjson_is_obj(inp)) {
                    yyjson_val *todos = yyjson_obj_get(inp, "todos");
                    if (yyjson_is_arr(todos)) {
                        mcode_plan_step steps[MCODE_MAX_PLAN_STEPS];
                        int sc = 0;
                        size_t ti, tmax;
                        yyjson_val *tv;
                        yyjson_arr_foreach(todos, ti, tmax, tv) {
                            if (sc >= MCODE_MAX_PLAN_STEPS || !yyjson_is_obj(tv)) continue;
                            const char *ct = jstr(tv, "content");
                            const char *af = jstr(tv, "activeForm");
                            const char *st = jstr(tv, "status");
                            const char *lbl = (ct && ct[0]) ? ct : af;
                            if (!lbl || !lbl[0]) continue;
                            memset(&steps[sc], 0, sizeof steps[sc]);
                            snprintf(steps[sc].title, sizeof steps[sc].title, "%s", lbl);
                            steps[sc].status = parse_plan_step_status(st);
                            if (steps[sc].status == CUI_TASK_RUNNING && af && af[0] &&
                                strcmp(af, lbl) != 0) {
                                snprintf(steps[sc].detail, sizeof steps[sc].detail, "%s", af);
                            }
                            ++sc;
                        }
                        if (sc > 0) emit_plan_update_ev(run, "Tasks", steps, sc);
                    }
                } else if (tname && strcmp(tname, "ExitPlanMode") == 0 && yyjson_is_obj(inp)) {
                    const char *pt = jstr(inp, "plan");
                    if (pt && pt[0]) emit_simple(run, EV_PLAN_SET, pt);
                }
                char detail[256] = "";
                summarize_tool_input(inp, detail, sizeof detail);
                char *inp_json = stringify_json_val(inp);
                emit_tool_start(run, tid, tname ? tname : "tool", detail, inp_json ? inp_json : "");
                free(inp_json);
                char st[160];
                if (detail[0]) snprintf(st, sizeof st, "%s: %s", tname ? tname : "tool", detail);
                else snprintf(st, sizeof st, "Running %s...", tname ? tname : "tool");
                emit_simple(run, EV_STATUS, st);
            } else if (strcmp(it, "image") == 0) {
                emit_json_image_content(run, item, "claude");
            }
        }
        /* Reset per-message streaming flags for next assistant turn within tool loop */
        run->streamed_text_deltas = false;
        run->streamed_thinking_deltas = false;
        return;
    }

    if (strcmp(type, "user") == 0) {
        yyjson_val *is_rep_v = yyjson_obj_get(root, "isReplay");
        if (yyjson_is_bool(is_rep_v) && yyjson_get_bool(is_rep_v)) {
            if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
                const char *uuid = jstr(root, "uuid");
                pthread_mutex_lock(&run->mu);
                run->claude_seen_replay_support = true;
                pthread_mutex_unlock(&run->mu);
                if (uuid && uuid[0]) {
                    claude_mark_steer_started(run, uuid);
                }
            }
            return;
        }
        yyjson_val *msg = yyjson_obj_get(root, "message");
        yyjson_val *content = yyjson_is_obj(msg) ? yyjson_obj_get(msg, "content") : NULL;
        if (!yyjson_is_arr(content)) return;
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(content, idx, max, item) {
            const char *it = jstr(item, "type");
            if (!it || strcmp(it, "tool_result") != 0) continue;
            const char *tid = jstr(item, "tool_use_id");
            yyjson_val *is_err_v = yyjson_obj_get(item, "is_error");
            bool failed = yyjson_is_bool(is_err_v) && yyjson_get_bool(is_err_v);
            yyjson_val *rc = yyjson_obj_get(item, "content");
            char *out_str = NULL;
            if (yyjson_is_str(rc)) {
                out_str = strdup(yyjson_get_str(rc));
            } else if (yyjson_is_arr(rc)) {
                mcode_str joined;
                mcode_str_init(&joined);
                size_t j, jmax;
                yyjson_val *part;
                yyjson_arr_foreach(rc, j, jmax, part) {
                    const char *pt = jstr(part, "text");
                    if (pt) {
                        if (joined.len > 0) mcode_str_append_cstr(&joined, "\n");
                        mcode_str_append_cstr(&joined, pt);
                    }
                }
                out_str = joined.data ? joined.data : strdup("");
            } else if (rc) {
                out_str = stringify_json_val(rc);
            }
            emit_tool_result(run, tid, out_str ? out_str : "", failed);
            free(out_str);
            if (!failed && yyjson_is_arr(rc)) {
                size_t j, jmax;
                yyjson_val *part;
                yyjson_arr_foreach(rc, j, jmax, part) {
                    emit_json_image_content(run, part, tid);
                }
            }
        }
        return;
    }

    if (strcmp(type, "rate_limit_event") == 0) {
        yyjson_val *info = yyjson_obj_get(root, "rate_limit_info");
        if (yyjson_is_obj(info)) {
            const char *rl_type = jstr(info, "rateLimitType");
            yyjson_val *util_v = yyjson_obj_get(info, "utilization");
            int64_t resets_at = ji64(info, "resetsAt");
            if (yyjson_is_num(util_v)) {
                double u = yyjson_get_num(util_v);
                int pct = clamp_percent((int)((u <= 1.0 ? u * 100.0 : u) + 0.5));
                mcode_provider_quota q;
                memset(&q, 0, sizeof q);
                q.available = true;
                q.updated_mono = mcode_now_mono();
                q.updated_epoch = mcode_now_epoch();
                if (rl_type && (strcmp(rl_type, "seven_day") == 0 || strstr(rl_type, "7d") != NULL ||
                                strstr(rl_type, "week") != NULL)) {
                    q.secondary.available = true;
                    q.secondary.used_percent = pct;
                    q.secondary.window_mins = 10080;
                    q.secondary.resets_at = resets_at;
                } else {
                    q.primary.available = true;
                    q.primary.used_percent = pct;
                    q.primary.window_mins = 300;
                    q.primary.resets_at = resets_at;
                }
                emit_rate_limit_ev(run, &q);
            }
        }
        return;
    }

    if (strcmp(type, "result") == 0) {
        bool steer_int = false;
        bool wait_for_followup = false;
        pthread_mutex_lock(&run->mu);
        steer_int = run->steer_interrupting;
        if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
            int unstarted = 0;
            for (int i = 0; i < run->claude_steer_count; ++i) {
                if (run->claude_steers[i].started) {
                    run->claude_steers[i].completed = true;
                } else if (!run->claude_steers[i].completed) {
                    ++unstarted;
                }
            }
            if (!steer_int && run->claude_seen_replay_support && unstarted > 0) {
                wait_for_followup = true;
                run->claude_waiting_steer_at = mcode_now_mono();
            } else {
                run->rpc_turn_done = true;
                run->claude_waiting_steer_at = 0.0;
            }
        }
        pthread_mutex_unlock(&run->mu);
        yyjson_val *is_err_v = yyjson_obj_get(root, "is_error");
        bool is_err = yyjson_is_bool(is_err_v) && yyjson_get_bool(is_err_v);
        const char *res_text = jstr(root, "result");
        if (is_err && res_text && res_text[0] && !steer_int) {
            emit_simple(run, EV_ERROR, res_text);
        } else if (!is_err && res_text && res_text[0] && !run->claude_pass_had_text) {
            emit_simple(run, EV_TEXT_DELTA, res_text);
        }
        run->claude_pass_had_text = false;
        run->streamed_text_deltas = false;
        run->streamed_thinking_deltas = false;
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        yyjson_val *cost = yyjson_obj_get(root, "total_cost_usd");
        yyjson_val *model_usage = yyjson_obj_get(root, "modelUsage");
        if (yyjson_is_obj(usage) || yyjson_is_num(cost)) {
            int64_t in_tok = 0, out_tok = 0, cached_tok = 0, cache_create = 0, ctx_win = 0;
            if (yyjson_is_obj(usage)) {
                in_tok = ji64(usage, "input_tokens");
                out_tok = ji64(usage, "output_tokens");
                cached_tok = ji64(usage, "cache_read_input_tokens");
                cache_create = ji64(usage, "cache_creation_input_tokens");
                in_tok += cached_tok + cache_create;
            }
            if (yyjson_is_obj(model_usage)) {
                size_t idx, max;
                yyjson_val *key, *val;
                yyjson_obj_foreach(model_usage, idx, max, key, val) {
                    (void)key;
                    int64_t cw = ji64(val, "contextWindow");
                    if (cw > ctx_win) ctx_win = cw;
                }
            }
            int64_t ctx_tok = run->claude_last_ctx_tokens > 0 ? run->claude_last_ctx_tokens : in_tok;
            double usd = yyjson_is_num(cost) ? yyjson_get_num(cost) : 0.0;
            emit_usage_ev(run, in_tok, out_tok, cached_tok, ctx_tok, ctx_win, usd, false);
        }
        if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
            if (wait_for_followup) {
                emit_simple(run, EV_STATUS, "Applying steer...");
            } else {
                mcode_proc_close_stdin(run->proc);
            }
        }
    }
}

typedef enum {
    PROBE_NONE = 0,
    PROBE_SEARCH,
    PROBE_FS,
    PROBE_GIT_READ,
} probe_kind;

static bool str_eq_ci_local(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool str_in_list(const char *s, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s, list[i]) == 0) return true;
    }
    return false;
}

/* Strip outer `/bin/zsh -lc ...` wrapper and POSIX-unquote the single script argument
 * so embedded quote sequences like `'"'!.git'"'` decode to the real shell script. */
static char *unwrap_shell_script(const char *cmd) {
    if (!cmd) return NULL;
    const char *p = cmd;
    while (*p == ' ' || *p == '\t') ++p;

    static const char *const prefixes[] = {
        "/bin/zsh -lc ", "/bin/bash -lc ", "/bin/sh -c ",
        "/bin/zsh -c ", "/bin/bash -c ",
        "zsh -lc ", "bash -lc ", "zsh -c ", "bash -c ", "sh -c "
    };
    bool wrapped = false;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], plen) == 0) {
            p += plen;
            wrapped = true;
            break;
        }
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (!wrapped) return strdup(p);

    size_t len = strlen(p);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t w = 0;
    bool in_sq = false, in_dq = false;
    for (size_t i = 0; i < len; ++i) {
        char c = p[i];
        if (in_sq) {
            if (c == '\'') in_sq = false;
            else out[w++] = c;
        } else if (in_dq) {
            if (c == '"') {
                in_dq = false;
            } else if (c == '\\' && i + 1 < len &&
                       (p[i + 1] == '"' || p[i + 1] == '\\' ||
                        p[i + 1] == '$' || p[i + 1] == '`')) {
                out[w++] = p[++i];
            } else {
                out[w++] = c;
            }
        } else {
            if (c == '\'') {
                in_sq = true;
            } else if (c == '"') {
                in_dq = true;
            } else if (c == '\\' && i + 1 < len) {
                out[w++] = p[++i];
            } else {
                out[w++] = c;
            }
        }
    }
    out[w] = '\0';
    return out;
}

static bool next_shell_token(const char *s, size_t len, size_t *pos, char *out, size_t cap) {
    if (!s || !pos || !out || cap == 0) return false;
    size_t i = *pos;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= len) { *pos = i; return false; }

    size_t w = 0;
    bool in_sq = false, in_dq = false;
    bool got_any = false;
    while (i < len) {
        char c = s[i];
        if (!in_sq && !in_dq) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == ';' || c == '|' || c == '&' || c == '<' || c == '>' ||
                c == '(' || c == ')') {
                break;
            }
            if (c == '#' && !got_any) {
                i = len;
                break;
            }
            if (c == '\'') { in_sq = true; got_any = true; ++i; continue; }
            if (c == '"') { in_dq = true; got_any = true; ++i; continue; }
            if (c == '\\' && i + 1 < len) {
                ++i;
                if (w + 1 < cap) out[w++] = s[i];
                got_any = true;
                ++i;
                continue;
            }
        } else if (in_sq) {
            if (c == '\'') { in_sq = false; ++i; continue; }
        } else if (in_dq) {
            if (c == '"') { in_dq = false; ++i; continue; }
            if (c == '\\' && i + 1 < len &&
                (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$' || s[i + 1] == '`')) {
                ++i;
            }
        }
        if (w + 1 < cap) out[w++] = s[i];
        got_any = true;
        ++i;
    }
    out[w] = '\0';
    *pos = i;
    return got_any;
}

static bool is_env_assignment(const char *tok) {
    if (!tok || (!isalpha((unsigned char)tok[0]) && tok[0] != '_')) return false;
    for (const char *p = tok + 1; *p; ++p) {
        if (*p == '=') return true;
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    }
    return false;
}

static const char *base_cmd_name(const char *tok) {
    if (!tok) return "";
    const char *slash = strrchr(tok, '/');
    return slash ? slash + 1 : tok;
}

static bool stmt_is_errexit(const char *stmt, size_t len) {
    size_t pos = 0;
    char tok[64];
    if (!next_shell_token(stmt, len, &pos, tok, sizeof tok)) return false;
    if (strcmp(tok, "set") != 0) return false;
    bool next_is_opt = false;
    while (next_shell_token(stmt, len, &pos, tok, sizeof tok)) {
        if (next_is_opt) {
            if (strcmp(tok, "errexit") == 0) return true;
            next_is_opt = false;
            continue;
        }
        if (strcmp(tok, "-o") == 0) {
            next_is_opt = true;
        } else if (tok[0] == '-' && tok[1] != '-' && tok[1] != '\0') {
            if (strchr(tok + 1, 'e') != NULL) return true;
        }
    }
    return false;
}

/* Scan statements split on unquoted `\n`, `;`, `&&`, `||`.
 * Sets *out_has_errexit if any statement enables `set -e` / `set -o errexit`,
 * and returns the last non-empty statement slice. */
static void scan_script_statements(const char *script, bool *out_has_errexit,
                                   const char **out_last_start, size_t *out_last_len) {
    if (out_has_errexit) *out_has_errexit = false;
    if (out_last_start) *out_last_start = NULL;
    if (out_last_len) *out_last_len = 0;
    if (!script) return;

    size_t n = strlen(script);
    size_t seg_start = 0;
    bool in_sq = false, in_dq = false;

    for (size_t i = 0; i <= n; ++i) {
        char c = (i < n) ? script[i] : '\0';
        if (i < n) {
            if (in_sq) {
                if (c == '\'') in_sq = false;
                continue;
            }
            if (in_dq) {
                if (c == '\\' && i + 1 < n) { ++i; continue; }
                if (c == '"') in_dq = false;
                continue;
            }
            if (c == '\\' && i + 1 < n) { ++i; continue; }
            if (c == '\'') { in_sq = true; continue; }
            if (c == '"') { in_dq = true; continue; }
            if (c == '#' && (i == seg_start || script[i - 1] == ' ' || script[i - 1] == '\t')) {
                /* End current segment before comment, then skip to newline */
                size_t s = seg_start, e = i;
                while (s < e && (script[s] == ' ' || script[s] == '\t' || script[s] == '\r')) ++s;
                while (e > s && (script[e - 1] == ' ' || script[e - 1] == '\t' || script[e - 1] == '\r')) --e;
                if (e > s) {
                    if (out_has_errexit && stmt_is_errexit(script + s, e - s)) *out_has_errexit = true;
                    if (out_last_start) *out_last_start = script + s;
                    if (out_last_len) *out_last_len = e - s;
                }
                while (i < n && script[i] != '\n') ++i;
                seg_start = i + 1;
                continue;
            }
        }

        bool is_sep = (i == n) || (c == '\n') || (c == ';') ||
                      (c == '&' && i + 1 < n && script[i + 1] == '&') ||
                      (c == '|' && i + 1 < n && script[i + 1] == '|');
        if (!is_sep) continue;

        size_t s = seg_start, e = i;
        while (s < e && (script[s] == ' ' || script[s] == '\t' || script[s] == '\r')) ++s;
        while (e > s && (script[e - 1] == ' ' || script[e - 1] == '\t' || script[e - 1] == '\r')) --e;
        if (e > s) {
            if (out_has_errexit && stmt_is_errexit(script + s, e - s)) *out_has_errexit = true;
            if (out_last_start) *out_last_start = script + s;
            if (out_last_len) *out_last_len = e - s;
        }
        if ((c == '&' || c == '|') && i + 1 < n && script[i + 1] == c) ++i;
        seg_start = i + 1;
    }
}

static bool extract_command_word(const char *seg, size_t len, size_t *pos, char *cmd_out, size_t cap) {
    char tok[128];
    while (next_shell_token(seg, len, pos, tok, sizeof tok)) {
        if (is_env_assignment(tok)) continue;
        if (strcmp(tok, "!") == 0 || strcmp(tok, "time") == 0 ||
            strcmp(tok, "builtin") == 0 || strcmp(tok, "noglob") == 0 ||
            strcmp(tok, "nocorrect") == 0 || strcmp(tok, "env") == 0) {
            continue;
        }
        snprintf(cmd_out, cap, "%s", tok);
        return true;
    }
    cmd_out[0] = '\0';
    return false;
}

static probe_kind classify_single_probe(const char *seg, size_t len) {
    size_t pos = 0;
    char raw_cmd[128];
    if (!extract_command_word(seg, len, &pos, raw_cmd, sizeof raw_cmd)) return PROBE_NONE;

    const char *cmd = base_cmd_name(raw_cmd);
    if (strcmp(cmd, "command") == 0) {
        char next[128];
        size_t saved = pos;
        while (next_shell_token(seg, len, &pos, next, sizeof next)) {
            if (next[0] == '-' && next[1] != '\0' && strcmp(next, "--") != 0) {
                if (strchr(next + 1, 'v') || strchr(next + 1, 'V')) return PROBE_SEARCH;
                continue;
            }
            if (strcmp(next, "--") == 0) {
                if (!next_shell_token(seg, len, &pos, next, sizeof next)) return PROBE_NONE;
            }
            snprintf(raw_cmd, sizeof raw_cmd, "%s", next);
            cmd = base_cmd_name(raw_cmd);
            saved = pos;
            break;
        }
        if (saved == pos && strcmp(cmd, "command") == 0) return PROBE_NONE;
    }

    static const char *const search_cmds[] = {
        "rg", "grep", "egrep", "fgrep", "ag", "ack", "fd", "fdfind",
        "diff", "cmp", "which", "where", "whereis", "type", "test", "[", "[["
    };
    if (str_in_list(cmd, search_cmds, sizeof search_cmds / sizeof search_cmds[0])) {
        return PROBE_SEARCH;
    }

    static const char *const fs_cmds[] = {
        "ls", "stat", "file", "readlink", "realpath", "find"
    };
    if (str_in_list(cmd, fs_cmds, sizeof fs_cmds / sizeof fs_cmds[0])) {
        return PROBE_FS;
    }

    if (strcmp(cmd, "git") == 0) {
        char tok[128];
        while (next_shell_token(seg, len, &pos, tok, sizeof tok)) {
            if (strcmp(tok, "-C") == 0 || strcmp(tok, "-c") == 0 ||
                strcmp(tok, "--git-dir") == 0 || strcmp(tok, "--work-tree") == 0 ||
                strcmp(tok, "--namespace") == 0 || strcmp(tok, "--super-prefix") == 0 ||
                strcmp(tok, "--config-env") == 0) {
                char skip[128];
                (void)next_shell_token(seg, len, &pos, skip, sizeof skip);
                continue;
            }
            if (tok[0] == '-') continue;

            static const char *const git_read_subcmds[] = {
                "status", "rev-parse", "branch", "log", "symbolic-ref", "show-ref",
                "describe", "remote", "ls-files", "diff", "grep", "check-ignore",
                "cat-file", "ls-tree", "rev-list", "merge-base", "name-rev",
                "for-each-ref", "shortlog", "blame"
            };
            if (str_in_list(tok, git_read_subcmds,
                            sizeof git_read_subcmds / sizeof git_read_subcmds[0])) {
                return PROBE_GIT_READ;
            }
            if (strcmp(tok, "config") == 0) {
                char opt[128];
                while (next_shell_token(seg, len, &pos, opt, sizeof opt)) {
                    if (strcmp(opt, "--get") == 0 || strcmp(opt, "--get-all") == 0 ||
                        strcmp(opt, "--get-regexp") == 0 || strcmp(opt, "--list") == 0 ||
                        strcmp(opt, "-l") == 0) {
                        return PROBE_GIT_READ;
                    }
                }
            }
            return PROBE_NONE;
        }
    }

    return PROBE_NONE;
}

static bool stage_is_readonly_filter(const char *seg, size_t len) {
    size_t pos = 0;
    char raw_cmd[128];
    if (!extract_command_word(seg, len, &pos, raw_cmd, sizeof raw_cmd)) return false;
    const char *cmd = base_cmd_name(raw_cmd);
    static const char *const filters[] = {
        "head", "tail", "sort", "uniq", "wc", "cut", "tr", "sed", "awk",
        "cat", "nl", "grep", "egrep", "fgrep", "rg", "column", "fmt", "fold",
        "tee", "less", "more"
    };
    return str_in_list(cmd, filters, sizeof filters / sizeof filters[0]);
}

static probe_kind classify_statement_probe(const char *stmt, size_t len) {
    if (!stmt || len == 0) return PROBE_NONE;
    const char *starts[16];
    size_t lens[16];
    int count = 0;

    size_t seg_start = 0;
    bool in_sq = false, in_dq = false;
    for (size_t i = 0; i <= len; ++i) {
        char c = (i < len) ? stmt[i] : '\0';
        if (i < len) {
            if (in_sq) { if (c == '\'') in_sq = false; continue; }
            if (in_dq) {
                if (c == '\\' && i + 1 < len) { ++i; continue; }
                if (c == '"') in_dq = false;
                continue;
            }
            if (c == '\\' && i + 1 < len) { ++i; continue; }
            if (c == '\'') { in_sq = true; continue; }
            if (c == '"') { in_dq = true; continue; }
        }
        bool is_pipe = (i == len) || (c == '|' && !(i + 1 < len && stmt[i + 1] == '|'));
        if (!is_pipe) continue;
        if (count < 16) {
            starts[count] = stmt + seg_start;
            lens[count] = i - seg_start;
            ++count;
        }
        seg_start = i + 1;
    }

    if (count <= 0) return PROBE_NONE;
    if (count == 1) return classify_single_probe(starts[0], lens[0]);

    probe_kind last_kind = classify_single_probe(starts[count - 1], lens[count - 1]);
    if (last_kind == PROBE_SEARCH) return PROBE_SEARCH;

    probe_kind first_kind = classify_single_probe(starts[0], lens[0]);
    if (first_kind != PROBE_NONE) {
        for (int i = 1; i < count; ++i) {
            if (!stage_is_readonly_filter(starts[i], lens[i])) return PROBE_NONE;
        }
        return first_kind;
    }
    return PROBE_NONE;
}

static bool slice_contains(const char *s, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > len) return false;
    for (size_t i = 0; i + nlen <= len; ++i) {
        if (memcmp(s + i, needle, nlen) == 0) return true;
    }
    return false;
}

static bool line_is_benign_git_fatal(const char *line, size_t len) {
    static const char *const benign_git[] = {
        "not a git repository",
        "does not have any commits yet",
        "No names found",
        "ambiguous argument",
        "unknown revision",
        "Needed a single revision",
        "not a symbolic ref",
        "not a valid object name",
        "bad default revision"
    };
    for (size_t i = 0; i < sizeof benign_git / sizeof benign_git[0]; ++i) {
        if (slice_contains(line, len, benign_git[i])) return true;
    }
    return false;
}

static bool output_has_hard_error(const char *output, probe_kind kind, int exit_code) {
    if (!output || !output[0]) return false;

    static const char *const hard_errors[] = {
        "command not found",
        "Permission denied",
        "Operation not permitted",
        "syntax error",
        "illegal option",
        "invalid option",
        "unrecognized option",
        "unknown option",
        "Segmentation fault",
        "Bus error",
        "Abort trap",
        "error:",
        "Error:",
        "error]",
        "os error "
    };
    for (size_t i = 0; i < sizeof hard_errors / sizeof hard_errors[0]; ++i) {
        if (strstr(output, hard_errors[i])) return true;
    }

    /* Inspect output line-by-line for non-benign `fatal:` or search-tool missing-path errors. */
    const char *p = output;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (slice_contains(p, llen, "fatal:") && !line_is_benign_git_fatal(p, llen)) {
            return true;
        }
        if (kind == PROBE_SEARCH && exit_code < 0 &&
            slice_contains(p, llen, "No such file or directory")) {
            static const char *const search_prefixes[] = {
                "rg:", "grep:", "egrep:", "fgrep:", "diff:", "cmp:", "fd:"
            };
            for (size_t i = 0; i < sizeof search_prefixes / sizeof search_prefixes[0]; ++i) {
                size_t plen = strlen(search_prefixes[i]);
                if (llen >= plen && memcmp(p, search_prefixes[i], plen) == 0) {
                    return true;
                }
            }
        }
        p = nl ? nl + 1 : p + llen;
    }
    return false;
}

bool mcode_command_is_benign_probe_exit(const char *cmd, const char *output, int exit_code) {
    if (!cmd || !cmd[0]) return false;
    if (exit_code == 0) return true;
    if (exit_code > 0 && exit_code != 1 && exit_code != 2 && exit_code != 128) return false;

    char *script = unwrap_shell_script(cmd);
    if (!script) return false;

    bool has_errexit = false;
    const char *last_stmt = NULL;
    size_t last_len = 0;
    scan_script_statements(script, &has_errexit, &last_stmt, &last_len);

    bool benign = false;
    if (!has_errexit && last_stmt && last_len > 0) {
        probe_kind kind = classify_statement_probe(last_stmt, last_len);
        if (kind == PROBE_SEARCH) {
            if (exit_code < 0 || exit_code == 1) {
                benign = !output_has_hard_error(output, kind, exit_code);
            }
        } else if (kind == PROBE_FS) {
            if (exit_code < 0 || exit_code == 1 || exit_code == 2) {
                benign = !output_has_hard_error(output, kind, exit_code);
            }
        } else if (kind == PROBE_GIT_READ) {
            if (exit_code == 128) {
                benign = output && line_is_benign_git_fatal(output, strlen(output)) &&
                         !output_has_hard_error(output, kind, exit_code);
            } else if (exit_code < 0 || exit_code == 1) {
                benign = !output_has_hard_error(output, kind, exit_code);
            }
        }
    }

    free(script);
    return benign;
}

static bool is_command_tool_name(const char *tool_name) {
    if (!tool_name || !tool_name[0]) return false;
    return str_eq_ci_local(tool_name, "bash") ||
           str_eq_ci_local(tool_name, "shell") ||
           str_eq_ci_local(tool_name, "sh") ||
           str_eq_ci_local(tool_name, "run_command") ||
           str_eq_ci_local(tool_name, "command_execution") ||
           str_eq_ci_local(tool_name, "execute_command");
}

bool mcode_tool_is_benign_probe_failure(const char *tool_name, const char *input,
                                        const char *output, int exit_code) {
    if (!is_command_tool_name(tool_name) || !input || !input[0]) return false;

    if (exit_code < 0 && output && strncmp(output, "Exit code ", 10) == 0) {
        char *endp = NULL;
        long parsed = strtol(output + 10, &endp, 10);
        if (endp && endp != output + 10 &&
            (*endp == '\n' || *endp == '\r' || *endp == ' ' || *endp == '\0')) {
            exit_code = (int)parsed;
        }
    }

    const char *p = input;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p == '{') {
        yyjson_doc *doc = yyjson_read(p, strlen(p), 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            const char *jcmd = jstr(root, "command");
            if (!jcmd) jcmd = jstr(root, "cmd");
            if (!jcmd) jcmd = jstr(root, "CommandLine");
            bool res = jcmd ? mcode_command_is_benign_probe_exit(jcmd, output, exit_code) : false;
            yyjson_doc_free(doc);
            return res;
        }
    }

    return mcode_command_is_benign_probe_exit(input, output, exit_code);
}

/* ---- Shared Codex imageGeneration & imageView item handlers ---- */
static void handle_codex_image_gen_item(mcode_agent_run *run, yyjson_val *item,
                                        const char *iid, bool is_done) {
    if (!run || !yyjson_is_obj(item)) return;
    run->has_emitted_text = false;
    const char *tid = (iid && iid[0]) ? iid : "imggen";
    const char *status = jstr(item, "status");
    const char *revised = jstr(item, "revisedPrompt");
    if (!revised) revised = jstr(item, "revised_prompt");
    const char *saved_path = jstr(item, "savedPath");
    if (!saved_path) saved_path = jstr(item, "saved_path");
    const char *result_b64 = jstr(item, "result");
    yyjson_val *failure = yyjson_obj_get(item, "failure");

    if (!is_done) {
        emit_tool_start(run, tid, "image_gen", "", revised ? revised : "");
        emit_simple(run, EV_STATUS, "Generating image...");
        return;
    }

    bool failed = (failure && !yyjson_is_null(failure)) ||
                  (status && (strcmp(status, "failed") == 0 ||
                              strcmp(status, "declined") == 0 ||
                              strcmp(status, "cancelled") == 0 ||
                              strcmp(status, "canceled") == 0));

    char resolved_path[MCODE_PATH_MAX] = "";
    if (!failed) {
        if (saved_path && file_exists_nonempty(saved_path)) {
            snprintf(resolved_path, sizeof resolved_path, "%s", saved_path);
        } else if (result_b64 && result_b64[0]) {
            save_base64_image_to_disk(result_b64, "image/png", tid,
                                      resolved_path, sizeof resolved_path);
        }
    }

    emit_tool_start(run, tid, "image_gen", "", revised ? revised : "");
    if (failed) {
        const char *ftype = yyjson_is_obj(failure) ? jstr(failure, "type") : NULL;
        char *fjson = (!ftype && failure && !yyjson_is_null(failure))
            ? stringify_json_val(failure)
            : NULL;
        const char *err_msg = (ftype && strcmp(ftype, "usageLimitExceeded") == 0)
            ? "Image generation usage limit exceeded."
            : (fjson && fjson[0] ? fjson : "Image generation failed.");
        emit_tool_result(run, tid, err_msg, true);
        free(fjson);
        return;
    }

    emit_tool_result(run, tid, resolved_path[0] ? resolved_path : "Image generated.", false);
    if (resolved_path[0]) {
        emit_image_ev(run, resolved_path, "Generated image");
    }
}

static void handle_codex_image_view_item(mcode_agent_run *run, yyjson_val *item,
                                         const char *iid, bool is_done) {
    if (!run || !yyjson_is_obj(item)) return;
    run->has_emitted_text = false;
    const char *tid = (iid && iid[0]) ? iid : "imgview";
    const char *path = jstr(item, "path");
    emit_tool_start(run, tid, "view_image", path ? path : "", path ? path : "");
    if (!is_done) {
        emit_simple(run, EV_STATUS, "Viewing image...");
    } else {
        emit_tool_result(run, tid, path ? path : "", false);
        if (path && file_exists_nonempty(path)) {
            emit_image_ev(run, path, "");
        }
    }
}

/* ---- OpenAI Codex JSONL parser ---- */
static void parse_codex_json(mcode_agent_run *run, yyjson_val *root) {
    const char *type = jstr(root, "type");
    if (!type) return;

    if (strcmp(type, "thread.started") == 0) {
        const char *tid = jstr(root, "thread_id");
        if (tid && tid[0]) emit_simple(run, EV_SESSION, tid);
        return;
    }

    if (strcmp(type, "turn.started") == 0) {
        return;
    }

    if (strcmp(type, "item.started") == 0 || strcmp(type, "item.updated") == 0 ||
        strcmp(type, "item.completed") == 0) {
        bool is_done = strcmp(type, "item.completed") == 0;
        yyjson_val *item = yyjson_obj_get(root, "item");
        if (!yyjson_is_obj(item)) return;
        const char *itype = jstr(item, "type");
        const char *iid = jstr(item, "id");
        if (!itype) return;

        if (strcmp(itype, "agent_message") == 0 || strcmp(itype, "message") == 0) {
            const char *txt = jstr(item, "text");
            if (txt && txt[0] && is_done) {
                emit_simple(run, EV_TEXT_DELTA, txt);
                emit_simple(run, EV_TEXT_DELTA, "\n");
            }
        } else if (strcmp(itype, "reasoning") == 0) {
            const char *txt = jstr(item, "text");
            if (txt && txt[0] && is_done) {
                emit_simple(run, EV_REASONING_DELTA, txt);
                emit_simple(run, EV_REASONING_DONE, NULL);
            } else if (!is_done) {
                emit_simple(run, EV_STATUS, "Reasoning...");
            }
        } else if (strcmp(itype, "command_execution") == 0) {
            const char *cmd = jstr(item, "command");
            const char *out = jstr(item, "aggregated_output");
            yyjson_val *ec = yyjson_obj_get(item, "exit_code");
            bool has_ec = yyjson_is_num(ec);
            int exit_code = has_ec ? (int)yyjson_get_num(ec) : -1;
            bool failed = has_ec && exit_code != 0;
            const char *status = jstr(item, "status");
            if (status && strcmp(status, "failed") == 0) failed = true;
            if (failed && mcode_command_is_benign_probe_exit(cmd, out, exit_code)) {
                failed = false;
            }

            char short_cmd[128] = "";
            summarize_command_detail(cmd, short_cmd, sizeof short_cmd);

            if (!is_done) {
                emit_tool_start(run, iid ? iid : "cmd", "bash", short_cmd, cmd ? cmd : "");
                char st[160];
                snprintf(st, sizeof st, "Running: %s", short_cmd[0] ? short_cmd : "command");
                emit_simple(run, EV_STATUS, st);
            } else {
                emit_tool_start(run, iid ? iid : "cmd", "bash", short_cmd, cmd ? cmd : "");
                emit_tool_result_code(run, iid ? iid : "cmd", out ? out : "", failed, exit_code);
            }
        } else if (strcmp(itype, "file_change") == 0) {
            yyjson_val *changes = yyjson_obj_get(item, "changes");
            mcode_str summary;
            mcode_str_init(&summary);
            char first_path[256] = "";
            if (yyjson_is_arr(changes)) {
                size_t idx, max;
                yyjson_val *c;
                yyjson_arr_foreach(changes, idx, max, c) {
                    const char *p = jstr(c, "path");
                    const char *k = jstr(c, "kind");
                    if (p) {
                        if (!first_path[0]) snprintf(first_path, sizeof first_path, "%s", p);
                        if (summary.len > 0) mcode_str_append_cstr(&summary, "\n");
                        mcode_str_append_cstr(&summary, k ? k : "edit");
                        mcode_str_append_cstr(&summary, " ");
                        mcode_str_append_cstr(&summary, p);
                    }
                }
            }
            emit_tool_start(run, iid ? iid : "file", "file_change", first_path, mcode_str_cstr(&summary));
            if (is_done) {
                emit_tool_result(run, iid ? iid : "file", mcode_str_cstr(&summary), false);
            }
            mcode_str_free(&summary);
        } else if (strcmp(itype, "mcp_tool_call") == 0 || strcmp(itype, "web_search") == 0) {
            const char *tool = jstr(item, "tool");
            const char *query = jstr(item, "query");
            char *args_json = stringify_json_val(yyjson_obj_get(item, "arguments"));
            emit_tool_start(run, iid ? iid : itype, tool ? tool : itype,
                            query ? query : "", args_json ? args_json : "");
            free(args_json);
            if (is_done) {
                char *res_json = stringify_json_val(yyjson_obj_get(item, "result"));
                const char *err_s = jstr(item, "error");
                emit_tool_result(run, iid ? iid : itype,
                                 err_s ? err_s : (res_json ? res_json : ""),
                                 err_s != NULL);
                free(res_json);
            }
        } else if (strcmp(itype, "image_generation") == 0 || strcmp(itype, "imageGeneration") == 0 ||
                   (strcmp(itype, "Extension") == 0 &&
                    jstr(item, "kind") && strcmp(jstr(item, "kind"), "image_gen.generation") == 0)) {
            handle_codex_image_gen_item(run, item, iid, is_done);
        } else if (strcmp(itype, "image_view") == 0 || strcmp(itype, "imageView") == 0) {
            handle_codex_image_view_item(run, item, iid, is_done);
        }
        return;
    }

    if (strcmp(type, "turn.completed") == 0) {
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        if (yyjson_is_obj(usage)) {
            int64_t in_tok = ji64(usage, "input_tokens");
            int64_t out_tok = ji64(usage, "output_tokens");
            int64_t cached = ji64(usage, "cached_input_tokens");
            emit_usage_ev(run, in_tok, out_tok, cached, 0, 0, 0.0, false);
        }
        return;
    }

    if (strcmp(type, "turn.failed") == 0 || strcmp(type, "error") == 0) {
        const char *msg = jstr(root, "message");
        yyjson_val *err_obj = yyjson_obj_get(root, "error");
        if (!msg && yyjson_is_obj(err_obj)) msg = jstr(err_obj, "message");
        emit_simple(run, EV_ERROR, msg ? msg : "Codex turn failed.");
    }
}

/* ---- Antigravity CLI (`agy`) stream-json parser ---- */
static void emit_agy_usage(mcode_agent_run *run, yyjson_val *usage) {
    if (!yyjson_is_obj(usage)) return;
    int64_t in_tok = ji64(usage, "input_tokens");
    int64_t out_tok = ji64(usage, "output_tokens");
    int64_t cached = ji64(usage, "cached_input_tokens");
    if (cached == 0) cached = ji64(usage, "cache_read_input_tokens");
    int64_t ctx_win = ji64(usage, "context_window");
    if (in_tok > 0 || out_tok > 0 || cached > 0) {
        emit_usage_ev(run, in_tok, out_tok, cached, 0, ctx_win, 0.0, false);
    }
}

static void parse_agy_result_obj(mcode_agent_run *run, yyjson_val *res) {
    if (!yyjson_is_obj(res)) return;
    const char *cid = jstr(res, "conversation_id");
    if (!cid) cid = jstr(res, "session_id");
    if (cid && cid[0]) emit_simple(run, EV_SESSION, cid);

    emit_simple(run, EV_REASONING_DONE, NULL);

    const char *status = jstr(res, "status");
    const char *err_s = jstr(res, "error");
    const char *resp = jstr(res, "response");

    bool is_err = (err_s && err_s[0]) ||
                  (status && status[0] &&
                   strcmp(status, "SUCCESS") != 0 &&
                   strcmp(status, "CANCELED") != 0);
    if (is_err) {
        if (err_s && err_s[0]) {
            emit_simple(run, EV_ERROR, err_s);
        } else {
            char msg[128];
            snprintf(msg, sizeof msg, "Antigravity CLI ended with status: %s",
                     (status && status[0]) ? status : "ERROR");
            emit_simple(run, EV_ERROR, msg);
        }
    } else if (resp && resp[0]) {
        emit_simple(run, EV_TEXT_FALLBACK, resp);
    } else if (!run->streamed_text_deltas) {
        yyjson_val *denied = yyjson_obj_get(res, "denied_actions");
        if (yyjson_is_arr(denied) && yyjson_arr_size(denied) > 0) {
            const char *seen[16];
            int seen_n = 0;
            char actions[192] = "";
            size_t pos = 0;
            size_t idx, max;
            yyjson_val *item;
            yyjson_arr_foreach(denied, idx, max, item) {
                const char *name = jstr(item, "display_name");
                if (!name || !name[0]) name = jstr(item, "action");
                if (!name || !name[0]) continue;
                bool dup = false;
                for (int j = 0; j < seen_n; ++j) {
                    if (strcmp(seen[j], name) == 0) { dup = true; break; }
                }
                if (dup) continue;
                if (seen_n < 16) seen[seen_n++] = name;
                int w = snprintf(actions + pos, sizeof actions - pos, "%s%s",
                                 pos > 0 ? ", " : "", name);
                if (w > 0 && (size_t)w < sizeof actions - pos) pos += (size_t)w;
            }
            char msg[320];
            if (actions[0]) {
                snprintf(msg, sizeof msg,
                         "Antigravity CLI denied %s in headless mode because it cannot prompt "
                         "interactively. Enable Auto-approve for this thread to allow unsandboxed actions.",
                         actions);
            } else {
                snprintf(msg, sizeof msg,
                         "Antigravity CLI denied a tool permission in headless mode. "
                         "Enable Auto-approve for this thread to allow unsandboxed actions.");
            }
            emit_simple(run, EV_ERROR, msg);
        }
    }

    emit_agy_usage(run, yyjson_obj_get(res, "usage"));
}

static void parse_agy_json(mcode_agent_run *run, yyjson_val *root) {
    const char *cid = jstr(root, "conversation_id");
    if (!cid) cid = jstr(root, "session_id");
    if (!cid) cid = jstr(root, "conversationId");
    if (cid && cid[0]) emit_simple(run, EV_SESSION, cid);

    const char *event = jstr(root, "event");

    /* Official `agy --output-format stream-json` envelope:
     *   {"event":"init","conversation_id":"...","init":{...}}
     *   {"event":"step_update","step_update":{...}}
     *   {"event":"result","result":{...}} */
    if (event) {
        if (strcmp(event, "init") == 0) {
            return;
        }

        if (strcmp(event, "step_update") == 0) {
            yyjson_val *su = yyjson_obj_get(root, "step_update");
            if (!yyjson_is_obj(su)) return;

            const char *scid = jstr(su, "conversation_id");
            if (scid && scid[0]) emit_simple(run, EV_SESSION, scid);

            const char *step_type = jstr(su, "step_type");
            const char *state = jstr(su, "state");
            bool is_done = state && (strcmp(state, "DONE") == 0 ||
                                     strcmp(state, "ERROR") == 0 ||
                                     strcmp(state, "FAILED") == 0);
            bool state_err = state && (strcmp(state, "ERROR") == 0 ||
                                       strcmp(state, "FAILED") == 0);

            char step_id[64] = "";
            yyjson_val *si_v = yyjson_obj_get(su, "step_index");
            if (yyjson_is_int(si_v)) {
                snprintf(step_id, sizeof step_id, "agy-step-%lld",
                         (long long)yyjson_get_sint(si_v));
            } else if (yyjson_is_num(si_v)) {
                snprintf(step_id, sizeof step_id, "agy-step-%lld",
                         (long long)yyjson_get_num(si_v));
            }

            if (step_type && (strcmp(step_type, "user_input") == 0 ||
                              strcmp(step_type, "checkpoint") == 0)) {
                return;
            }

            if (step_type && strcmp(step_type, "agent_response") == 0) {
                const char *th = jstr(su, "thinking_delta");
                if (!th) th = jstr(su, "reasoning_delta");
                if (!th) th = jstr(su, "thought_delta");
                if (th && th[0]) {
                    run->streamed_thinking_deltas = true;
                    emit_simple(run, EV_REASONING_DELTA, th);
                    emit_simple(run, EV_STATUS, "Thinking...");
                }

                const char *td = jstr(su, "text_delta");
                if (!td) td = jstr(su, "delta");
                if (!td) td = jstr(su, "text");
                if (td && td[0]) {
                    run->streamed_text_deltas = true;
                    emit_simple(run, EV_REASONING_DONE, NULL);
                    emit_simple(run, EV_TEXT_DELTA, td);
                    emit_simple(run, EV_STATUS, "Responding...");
                } else if (is_done) {
                    emit_simple(run, EV_REASONING_DONE, NULL);
                } else if (state && strcmp(state, "ACTIVE") == 0) {
                    emit_simple(run, EV_STATUS, "Thinking...");
                }
                return;
            }

            yyjson_val *ti = yyjson_obj_get(su, "tool_info");
            if ((step_type && strcmp(step_type, "tool") == 0) || yyjson_is_obj(ti)) {
                const char *tname = jstr(su, "tool_name");
                if (!tname && yyjson_is_obj(ti)) tname = jstr(ti, "name");
                if (!tname || !tname[0]) tname = "tool";
                const char *tid = step_id[0] ? step_id : tname;

                yyjson_val *params = yyjson_is_obj(ti) ? yyjson_obj_get(ti, "parameters") : NULL;
                char detail[256] = "";
                summarize_tool_input(params, detail, sizeof detail);
                char *inp_json = stringify_json_val(params);
                emit_tool_start(run, tid, tname, detail, inp_json ? inp_json : "");
                free(inp_json);

                if (!is_done) {
                    char st[160];
                    if (detail[0]) snprintf(st, sizeof st, "%s: %s", tname, detail);
                    else snprintf(st, sizeof st, "Running %s...", tname);
                    emit_simple(run, EV_STATUS, st);
                } else {
                    yyjson_val *err_v = yyjson_is_obj(ti) ? yyjson_obj_get(ti, "error") : NULL;
                    yyjson_val *out_v = yyjson_is_obj(ti) ? yyjson_obj_get(ti, "output") : NULL;
                    bool failed = state_err || (err_v != NULL && !yyjson_is_null(err_v));

                    char *out_s = NULL;
                    if (err_v && !yyjson_is_null(err_v)) {
                        const char *emsg = yyjson_is_obj(err_v) ? jstr(err_v, "message") : NULL;
                        if (emsg && emsg[0]) out_s = strdup(emsg);
                        else out_s = stringify_json_val(err_v);
                    }
                    if ((!out_s || !out_s[0]) && out_v && !yyjson_is_null(out_v)) {
                        free(out_s);
                        out_s = stringify_json_val(out_v);
                    }
                    emit_tool_result(run, tid, out_s ? out_s : "", failed);
                    free(out_s);
                }
                return;
            }

            yyjson_val *sub_info = yyjson_obj_get(su, "subagent_info");
            if (yyjson_is_obj(sub_info)) {
                const char *tid = step_id[0] ? step_id : "subagent";
                char detail[256] = "";
                yyjson_val *subs = yyjson_obj_get(sub_info, "subagents");
                if (yyjson_is_arr(subs) && yyjson_arr_size(subs) > 0) {
                    yyjson_val *first = yyjson_arr_get_first(subs);
                    const char *role = jstr(first, "role");
                    if (!role || !role[0]) role = jstr(first, "type_name");
                    if (role && role[0]) snprintf(detail, sizeof detail, "%s", role);
                }
                char *sub_json = stringify_json_val(sub_info);
                emit_tool_start(run, tid, "subagent", detail, sub_json ? sub_json : "");
                if (is_done) {
                    emit_tool_result(run, tid, sub_json ? sub_json : "", state_err);
                } else {
                    char st[160];
                    if (detail[0]) snprintf(st, sizeof st, "Subagent: %s", detail);
                    else snprintf(st, sizeof st, "Running subagent...");
                    emit_simple(run, EV_STATUS, st);
                }
                free(sub_json);
            }
            return;
        }

        if (strcmp(event, "result") == 0) {
            yyjson_val *res = yyjson_obj_get(root, "result");
            if (yyjson_is_obj(res)) {
                parse_agy_result_obj(run, res);
                return;
            }
        }
    }

    const char *type = jstr(root, "type");
    if (!type) type = event;

    /* Single `--output-format json` result envelope at top level */
    if (!type && (yyjson_obj_get(root, "response") || yyjson_obj_get(root, "status"))) {
        parse_agy_result_obj(run, root);
        return;
    }

    if (type && (strcmp(type, "error") == 0 || strcmp(type, "failed") == 0)) {
        const char *msg = jstr(root, "message");
        if (!msg) msg = jstr(root, "error");
        emit_simple(run, EV_ERROR, msg ? msg : "Antigravity CLI error.");
        return;
    }

    if (type && (strcmp(type, "thinking") == 0 || strcmp(type, "reasoning") == 0 ||
                 strcmp(type, "thought") == 0)) {
        const char *th = jstr(root, "text");
        if (!th) th = jstr(root, "delta");
        if (!th) th = jstr(root, "content");
        if (th && th[0]) emit_simple(run, EV_REASONING_DELTA, th);
        emit_simple(run, EV_STATUS, "Thinking...");
        return;
    }

    if (type && (strcmp(type, "tool_use") == 0 || strcmp(type, "tool_call") == 0 ||
                 strcmp(type, "function_call") == 0)) {
        const char *tid = jstr(root, "id");
        if (!tid) tid = jstr(root, "call_id");
        const char *tname = jstr(root, "name");
        if (!tname) tname = jstr(root, "tool");
        yyjson_val *inp = yyjson_obj_get(root, "input");
        if (!inp) inp = yyjson_obj_get(root, "args");
        if (!inp) inp = yyjson_obj_get(root, "arguments");
        char detail[256] = "";
        summarize_tool_input(inp, detail, sizeof detail);
        char *inp_json = stringify_json_val(inp);
        emit_tool_start(run, tid ? tid : (tname ? tname : "tool"),
                        tname ? tname : "tool", detail, inp_json ? inp_json : "");
        free(inp_json);
        char st[160];
        snprintf(st, sizeof st, "Running %s...", tname ? tname : "tool");
        emit_simple(run, EV_STATUS, st);
        return;
    }

    if (type && (strcmp(type, "tool_result") == 0 || strcmp(type, "function_response") == 0)) {
        const char *tid = jstr(root, "tool_use_id");
        if (!tid) tid = jstr(root, "call_id");
        if (!tid) tid = jstr(root, "id");
        yyjson_val *err_v = yyjson_obj_get(root, "is_error");
        bool failed = yyjson_is_bool(err_v) && yyjson_get_bool(err_v);
        yyjson_val *out_v = yyjson_obj_get(root, "output");
        if (!out_v) out_v = yyjson_obj_get(root, "content");
        if (!out_v) out_v = yyjson_obj_get(root, "result");
        char *out_s = stringify_json_val(out_v);
        emit_tool_result(run, tid ? tid : "", out_s ? out_s : "", failed);
        free(out_s);
        return;
    }

    /* Flat text delta or final text fallback */
    const char *txt = jstr(root, "delta");
    if (!txt) txt = jstr(root, "text");
    if (!txt) txt = jstr(root, "content");
    if (!txt) txt = jstr(root, "result");
    if (!txt) txt = jstr(root, "response");
    if (txt && txt[0]) {
        emit_simple(run, EV_REASONING_DONE, NULL);
        emit_simple(run, EV_TEXT_DELTA, txt);
        emit_simple(run, EV_STATUS, "Responding...");
    }

    emit_agy_usage(run, yyjson_obj_get(root, "usage"));
}

/* ---- Codex `app-server` JSON-RPC parser & handshake ---- */
static void codex_rpc_send_initialize(mcode_agent_run *run) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", 1);
    yyjson_mut_obj_add_str(doc, root, "method", "initialize");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_val *ci = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ci, "name", "mcode");
    yyjson_mut_obj_add_str(doc, ci, "version", "0.1.0");
    yyjson_mut_obj_add_val(doc, params, "clientInfo", ci);
    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, caps, "experimentalApi", true);
    yyjson_mut_obj_add_val(doc, params, "capabilities", caps);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
}

static const char *codex_approval_policy(mcode_permission_mode mode) {
    if (mode == MCODE_PERM_FULL_ACCESS) return "never";
    if (mode == MCODE_PERM_SUPERVISED) return "untrusted";
    /* Auto-edits and Plan mode use "on-request" (Plan mode is already OS-sandboxed read-only). */
    return "on-request";
}

static void codex_append_prompt_inputs(yyjson_mut_doc *doc, yyjson_mut_val *inp,
                                       const char *text, const char *workdir) {
    char img_paths[MCODE_PROMPT_IMAGE_MAX][MCODE_PATH_MAX];
    int img_count = extract_prompt_local_images(text, workdir, img_paths, MCODE_PROMPT_IMAGE_MAX);
    for (int i = 0; i < img_count; ++i) {
        yyjson_mut_val *im = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, im, "type", "localImage");
        yyjson_mut_obj_add_strcpy(doc, im, "path", img_paths[i]);
        yyjson_mut_arr_append(inp, im);
    }
    yyjson_mut_val *part = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, part, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, part, "text", text ? text : "");
    yyjson_mut_arr_append(inp, part);
}

static void codex_rpc_send_thread_open(mcode_agent_run *run, bool resume, int rpc_id) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", rpc_id);
    yyjson_mut_obj_add_str(doc, root, "method", resume ? "thread/resume" : "thread/start");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    if (resume) {
        yyjson_mut_obj_add_strcpy(doc, params, "threadId", run->init_session_id);
    }
    if (run->init_cwd[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "cwd", run->init_cwd);
    }
    yyjson_mut_obj_add_str(doc, params, "approvalPolicy", codex_approval_policy(run->init_perm_mode));
    const char *sandbox = (run->init_perm_mode == MCODE_PERM_FULL_ACCESS) ? "danger-full-access"
                        : (run->init_perm_mode == MCODE_PERM_PLAN) ? "read-only"
                        : "workspace-write";
    yyjson_mut_obj_add_str(doc, params, "sandbox", sandbox);
    if (run->init_model[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "model", run->init_model);
    }
    if (run->init_speed == MCODE_SPEED_FAST) {
        yyjson_mut_obj_add_str(doc, params, "serviceTier", "fast");
    } else if (run->init_speed == MCODE_SPEED_STANDARD) {
        yyjson_mut_obj_add_str(doc, params, "serviceTier", "default");
    }
    yyjson_mut_obj_add_val(doc, root, "params", params);
    send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
}

static void codex_rpc_send_turn_start(mcode_agent_run *run) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", 3);
    yyjson_mut_obj_add_str(doc, root, "method", "turn/start");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "threadId", run->active_session_id);
    yyjson_mut_val *inp = yyjson_mut_arr(doc);
    codex_append_prompt_inputs(doc, inp, run->init_prompt ? run->init_prompt : "", run->init_cwd);
    yyjson_mut_obj_add_val(doc, params, "input", inp);
    if (run->init_cwd[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "cwd", run->init_cwd);
    }
    yyjson_mut_obj_add_str(doc, params, "approvalPolicy", codex_approval_policy(run->init_perm_mode));
    if (run->init_perm_mode == MCODE_PERM_FULL_ACCESS) {
        yyjson_mut_val *sp = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, sp, "type", "dangerFullAccess");
        yyjson_mut_obj_add_val(doc, params, "sandboxPolicy", sp);
    } else if (run->init_perm_mode == MCODE_PERM_PLAN) {
        yyjson_mut_val *sp = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, sp, "type", "readOnly");
        yyjson_mut_obj_add_val(doc, params, "sandboxPolicy", sp);
    }
    if (run->init_model[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "model", run->init_model);
    }
    if (run->init_effort[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "effort", run->init_effort);
    }
    const char *cm_model = run->init_model[0] ? run->init_model : run->resolved_model;
    if (cm_model && cm_model[0]) {
        yyjson_mut_val *cm = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, cm, "mode",
                               run->init_perm_mode == MCODE_PERM_PLAN ? "plan" : "default");
        yyjson_mut_val *cset = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, cset, "model", cm_model);
        if (run->init_effort[0]) {
            yyjson_mut_obj_add_strcpy(doc, cset, "reasoning_effort", run->init_effort);
        } else {
            yyjson_mut_obj_add_null(doc, cset, "reasoning_effort");
        }
        yyjson_mut_obj_add_null(doc, cset, "developer_instructions");
        yyjson_mut_obj_add_val(doc, cm, "settings", cset);
        yyjson_mut_obj_add_val(doc, params, "collaborationMode", cm);
    }
    if (run->init_speed == MCODE_SPEED_FAST) {
        yyjson_mut_obj_add_str(doc, params, "serviceTier", "fast");
    } else if (run->init_speed == MCODE_SPEED_STANDARD) {
        yyjson_mut_obj_add_str(doc, params, "serviceTier", "default");
    }
    yyjson_mut_obj_add_val(doc, root, "params", params);
    pthread_mutex_lock(&run->mu);
    run->rpc_prompt_sent = true;
    pthread_mutex_unlock(&run->mu);
    send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
}

static bool codex_rpc_send_turn_steer(mcode_agent_run *run, const char *session_id,
                                      const char *turn_id, const char *message) {
    if (!run || !run->proc || !session_id || !session_id[0] ||
        !turn_id || !turn_id[0] || !message || !message[0]) {
        return false;
    }

    int64_t rpc_id = 0;
    bool tracked = false;
    pthread_mutex_lock(&run->mu);
    if (run->next_rpc_id < 100) run->next_rpc_id = 100;
    rpc_id = run->next_rpc_id++;
    if (run->inflight_steer_count < 16) {
        char *copy = strdup(message);
        if (copy) {
            int idx = run->inflight_steer_count++;
            run->inflight_steer_ids[idx] = rpc_id;
            run->inflight_steer_texts[idx] = copy;
            tracked = true;
        }
    }
    pthread_mutex_unlock(&run->mu);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        if (tracked) {
            pthread_mutex_lock(&run->mu);
            for (int i = 0; i < run->inflight_steer_count; ++i) {
                if (run->inflight_steer_ids[i] == rpc_id) {
                    free(run->inflight_steer_texts[i]);
                    for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                        run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                        run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                    }
                    run->inflight_steer_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&run->mu);
        }
        return false;
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_sint(doc, root, "id", rpc_id);
    yyjson_mut_obj_add_str(doc, root, "method", "turn/steer");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "threadId", session_id);
    yyjson_mut_obj_add_strcpy(doc, params, "expectedTurnId", turn_id);
    yyjson_mut_val *inp = yyjson_mut_arr(doc);
    codex_append_prompt_inputs(doc, inp, message, run->init_cwd);
    yyjson_mut_obj_add_val(doc, params, "input", inp);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);

    if (!ok && tracked) {
        pthread_mutex_lock(&run->mu);
        for (int i = 0; i < run->inflight_steer_count; ++i) {
            if (run->inflight_steer_ids[i] == rpc_id) {
                free(run->inflight_steer_texts[i]);
                for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                    run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                    run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                }
                run->inflight_steer_count--;
                break;
            }
        }
        pthread_mutex_unlock(&run->mu);
    }
    return ok;
}

static bool codex_rpc_send_turn_interrupt(mcode_agent_run *run) {
    if (!run || !run->proc) return false;
    char sid[MCODE_SESSION_MAX] = "";
    char tid[128] = "";
    int64_t rpc_id = 0;
    pthread_mutex_lock(&run->mu);
    snprintf(sid, sizeof sid, "%s", run->active_session_id);
    snprintf(tid, sizeof tid, "%s", run->active_turn_id);
    if (run->next_rpc_id < 100) run->next_rpc_id = 100;
    rpc_id = run->next_rpc_id++;
    pthread_mutex_unlock(&run->mu);
    if (!sid[0] || !tid[0]) return false;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_sint(doc, root, "id", rpc_id);
    yyjson_mut_obj_add_str(doc, root, "method", "turn/interrupt");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "threadId", sid);
    yyjson_mut_obj_add_strcpy(doc, params, "turnId", tid);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

static void codex_rpc_set_active_turn(mcode_agent_run *run, const char *turn_id) {
    if (!run || !turn_id || !turn_id[0]) return;
    char sid[MCODE_SESSION_MAX] = "";
    char **queued = NULL;
    int queued_n = 0;
    bool want_interrupt = false;

    pthread_mutex_lock(&run->mu);
    snprintf(run->active_turn_id, sizeof run->active_turn_id, "%s", turn_id);
    snprintf(sid, sizeof sid, "%s", run->active_session_id);
    want_interrupt = run->deferred_interrupt;
    run->deferred_interrupt = false;
    if (run->pending_live_steer_count > 0) {
        queued = run->pending_live_steers;
        queued_n = run->pending_live_steer_count;
        run->pending_live_steers = NULL;
        run->pending_live_steer_count = 0;
        run->pending_live_steer_cap = 0;
    }
    pthread_mutex_unlock(&run->mu);

    if (want_interrupt) {
        codex_rpc_send_turn_interrupt(run);
    }
    for (int i = 0; i < queued_n; ++i) {
        if (queued[i] && queued[i][0]) {
            if (want_interrupt || !codex_rpc_send_turn_steer(run, sid, turn_id, queued[i])) {
                emit_simple(run, EV_STEER_REJECTED, queued[i]);
            }
        }
        free(queued[i]);
    }
    free(queued);
}

static void flush_unsent_live_steers(mcode_agent_run *run) {
    if (!run) return;
    char **queued = NULL;
    int queued_n = 0;
    char *unacked[16];
    int unacked_n = 0;
    claude_live_steer *cl_steers = NULL;
    int cl_n = 0;
    bool cl_can_reject = false;
    bool user_stopped = false;

    pthread_mutex_lock(&run->mu);
    user_stopped = run->stop_requested;
    if (run->pending_live_steer_count > 0) {
        queued = run->pending_live_steers;
        queued_n = run->pending_live_steer_count;
        run->pending_live_steers = NULL;
        run->pending_live_steer_count = 0;
        run->pending_live_steer_cap = 0;
    }
    for (int i = 0; i < run->inflight_steer_count && unacked_n < 16; ++i) {
        unacked[unacked_n++] = run->inflight_steer_texts[i];
        run->inflight_steer_texts[i] = NULL;
    }
    run->inflight_steer_count = 0;
    if (run->claude_steer_count > 0) {
        cl_steers = run->claude_steers;
        cl_n = run->claude_steer_count;
        cl_can_reject = (run->claude_seen_replay_support || !run->rpc_turn_done);
        run->claude_steers = NULL;
        run->claude_steer_count = 0;
        run->claude_steer_cap = 0;
    }
    pthread_mutex_unlock(&run->mu);

    for (int i = 0; i < unacked_n; ++i) {
        if (!user_stopped && unacked[i] && unacked[i][0]) {
            emit_simple(run, EV_STEER_REJECTED, unacked[i]);
        }
        free(unacked[i]);
    }
    for (int i = 0; i < queued_n; ++i) {
        if (!user_stopped && queued[i] && queued[i][0]) {
            emit_simple(run, EV_STEER_REJECTED, queued[i]);
        }
        free(queued[i]);
    }
    free(queued);
    for (int i = 0; i < cl_n; ++i) {
        if (!user_stopped && cl_can_reject &&
            !cl_steers[i].started && !cl_steers[i].completed &&
            cl_steers[i].text && cl_steers[i].text[0]) {
            emit_simple(run, EV_STEER_REJECTED, cl_steers[i].text);
        }
        free(cl_steers[i].text);
    }
    free(cl_steers);
}

static void codex_rpc_finish_turn(mcode_agent_run *run) {
    flush_unsent_live_steers(run);
    if (!run->codex_emitted_live_usage &&
        (run->codex_turn_in_tokens > 0 || run->codex_turn_out_tokens > 0)) {
        emit_usage_ev(run, run->codex_turn_in_tokens, run->codex_turn_out_tokens,
                      run->codex_turn_cached_tokens, run->codex_last_ctx_tokens,
                      run->codex_ctx_window, 0.0, true);
    }
    pthread_mutex_lock(&run->mu);
    run->rpc_turn_done = true;
    approval_queue_clear_locked(run);
    run->question_pending = false;
    pthread_mutex_unlock(&run->mu);
    emit_simple(run, EV_APPROVAL_CLEAR, NULL);
    emit_simple(run, EV_QUESTION_CLEAR, NULL);
    mcode_proc_stop(run->proc);
}

static void parse_codex_rpc_json(mcode_agent_run *run, yyjson_val *root) {
    const char *method = jstr(root, "method");
    yyjson_val *id_val = yyjson_obj_get(root, "id");

    /* Responses to our outgoing JSON-RPC requests */
    if (!method && id_val && yyjson_is_int(id_val)) {
        int64_t rid = yyjson_get_sint(id_val);
        yyjson_val *err_v = yyjson_obj_get(root, "error");
        yyjson_val *res_v = yyjson_obj_get(root, "result");

        if (rid == 1) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                emit_simple(run, EV_ERROR, emsg ? emsg : "Codex app-server initialize failed.");
                codex_rpc_finish_turn(run);
                return;
            }
            yyjson_mut_doc *ndoc = yyjson_mut_doc_new(NULL);
            if (ndoc) {
                yyjson_mut_val *nroot = yyjson_mut_obj(ndoc);
                yyjson_mut_doc_set_root(ndoc, nroot);
                yyjson_mut_obj_add_str(ndoc, nroot, "jsonrpc", "2.0");
                yyjson_mut_obj_add_str(ndoc, nroot, "method", "initialized");
                send_json_doc(run->proc, ndoc);
                yyjson_mut_doc_free(ndoc);
            }
            codex_rpc_send_thread_open(run, run->init_session_id[0] != '\0', 2);
            return;
        }

        if (rid == 2 || rid == 20) {
            if (err_v) {
                if (rid == 2 && run->init_session_id[0]) {
                    /* Stale/unknown session ID: start a fresh thread cleanly */
                    run->init_session_id[0] = '\0';
                    codex_rpc_send_thread_open(run, false, 20);
                    return;
                }
                const char *emsg = jstr(err_v, "message");
                emit_simple(run, EV_ERROR, emsg ? emsg : "Codex failed to open thread.");
                codex_rpc_finish_turn(run);
                return;
            }
            yyjson_val *thr = yyjson_is_obj(res_v) ? yyjson_obj_get(res_v, "thread") : NULL;
            const char *tid = jstr(thr, "id");
            const char *rmodel = jstr(res_v, "model");
            if (rmodel && rmodel[0]) {
                snprintf(run->resolved_model, sizeof run->resolved_model, "%s", rmodel);
            }
            if (tid && tid[0]) {
                pthread_mutex_lock(&run->mu);
                snprintf(run->active_session_id, sizeof run->active_session_id, "%s", tid);
                pthread_mutex_unlock(&run->mu);
                emit_simple(run, EV_SESSION, tid);
                codex_rpc_send_turn_start(run);
            } else {
                emit_simple(run, EV_ERROR, "Codex did not return a thread ID.");
                codex_rpc_finish_turn(run);
            }
            return;
        }

        if (rid == 3) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                emit_simple(run, EV_ERROR, emsg ? emsg : "Codex turn/start failed.");
                codex_rpc_finish_turn(run);
                return;
            }
            yyjson_val *turn = yyjson_is_obj(res_v) ? yyjson_obj_get(res_v, "turn") : NULL;
            const char *turn_id = jstr(turn, "id");
            if (turn_id && turn_id[0]) {
                codex_rpc_set_active_turn(run, turn_id);
            }
            return;
        }

        /* Check if this response matches an in-flight `turn/steer` request */
        char *steer_text = NULL;
        bool matched_steer = false;
        pthread_mutex_lock(&run->mu);
        for (int i = 0; i < run->inflight_steer_count; ++i) {
            if (run->inflight_steer_ids[i] == rid) {
                matched_steer = true;
                steer_text = run->inflight_steer_texts[i];
                for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                    run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                    run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                }
                run->inflight_steer_count--;
                break;
            }
        }
        pthread_mutex_unlock(&run->mu);
        if (matched_steer) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                app_log_warn("Codex turn/steer rejected (%s); falling back to next turn",
                             emsg ? emsg : "unknown error");
                if (steer_text && steer_text[0]) {
                    emit_simple(run, EV_STEER_REJECTED, steer_text);
                }
            } else {
                emit_simple(run, EV_STATUS, "Steered — continuing...");
            }
            free(steer_text);
        }
        return;
    }

    if (!method) return;
    yyjson_val *params = yyjson_obj_get(root, "params");

    /* Server-initiated approval requests (have both id and method) */
    if (id_val && !yyjson_is_null(id_val)) {
        if (strcmp(method, "item/commandExecution/requestApproval") == 0) {
            const char *cmd = jstr(params, "command");
            const char *reason = jstr(params, "reason");
            const char *cwd = jstr(params, "cwd");
            char short_cmd[512] = "";
            summarize_command_detail(cmd, short_cmd, sizeof short_cmd);

            bool has_session = false;
            bool use_amend = false;
            char *amend_json = NULL;
            yyjson_val *avail = yyjson_is_obj(params) ? yyjson_obj_get(params, "availableDecisions") : NULL;
            if (yyjson_is_arr(avail)) {
                size_t idx, max;
                yyjson_val *dv;
                yyjson_arr_foreach(avail, idx, max, dv) {
                    if (yyjson_is_str(dv) && strcmp(yyjson_get_str(dv), "acceptForSession") == 0) {
                        has_session = true;
                        use_amend = false;
                        break;
                    } else if (yyjson_is_obj(dv) && yyjson_obj_get(dv, "acceptWithExecpolicyAmendment") && !amend_json) {
                        amend_json = yyjson_val_write(dv, 0, NULL);
                        if (amend_json) {
                            has_session = true;
                            use_amend = true;
                        }
                    }
                }
            } else {
                has_session = true;
            }

            char details[1024] = "";
            if (cmd && cmd[0] && cwd && cwd[0]) {
                snprintf(details, sizeof details, "%s\n(cwd: %s)", cmd, cwd);
            } else if (cmd && cmd[0]) {
                snprintf(details, sizeof details, "%s", cmd);
            }

            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CODEX_CMD;
                approval_item_set_rpc_id(ait, id_val);
                ait->codex_use_amendment = use_amend;
                ait->payload_json = use_amend ? amend_json : NULL;
                if (!use_amend) free(amend_json);
                ait->title = dup_or_null("Approve command?");
                ait->tool_name = dup_or_null("command");
                ait->command = dup_or_null(short_cmd[0] ? short_cmd : (cmd ? cmd : ""));
                ait->reason = dup_or_null(reason ? reason : "");
                ait->details = dup_or_null(details);
                ait->can_allow_session = has_session;
                approval_queue_push(run, ait);
            } else {
                free(amend_json);
            }
            return;
        }

        if (strcmp(method, "item/fileChange/requestApproval") == 0) {
            const char *reason = jstr(params, "reason");
            const char *grant_root = jstr(params, "grantRoot");

            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CODEX_FILE;
                approval_item_set_rpc_id(ait, id_val);
                ait->title = dup_or_null("Approve file changes?");
                ait->tool_name = dup_or_null("file_change");
                ait->command = dup_or_null(grant_root ? grant_root : "");
                ait->reason = dup_or_null(reason ? reason : "");
                ait->details = dup_or_null("");
                ait->can_allow_session = true;
                approval_queue_push(run, ait);
            }
            return;
        }

        if (strcmp(method, "item/permissions/requestApproval") == 0) {
            const char *reason = jstr(params, "reason");
            yyjson_val *perms = yyjson_is_obj(params) ? yyjson_obj_get(params, "permissions") : NULL;
            char *compact_perms = perms ? yyjson_val_write(perms, 0, NULL) : strdup("{}");
            char *pretty_perms = stringify_json_val(perms);

            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CODEX_PERMS;
                approval_item_set_rpc_id(ait, id_val);
                ait->payload_json = compact_perms ? compact_perms : strdup("{}");
                ait->title = dup_or_null("Grant extra permissions?");
                ait->tool_name = dup_or_null("permissions");
                ait->command = dup_or_null("");
                ait->reason = dup_or_null(reason ? reason : "");
                ait->details = dup_or_null(pretty_perms ? pretty_perms : "");
                ait->can_allow_session = true;
                approval_queue_push(run, ait);
            } else {
                free(compact_perms);
            }
            free(pretty_perms);
            return;
        }

        if (strcmp(method, "item/tool/requestUserInput") == 0) {
            mcode_pending_question pq;
            yyjson_val *q_arr = yyjson_is_obj(params) ? yyjson_obj_get(params, "questions") : NULL;
            if (parse_questions_array(q_arr, false, &pq)) {
                pthread_mutex_lock(&run->mu);
                run->question_pending = true;
                run->question_kind = QUESTION_KIND_CODEX;
                if (yyjson_is_str(id_val)) {
                    run->question_rpc_id_is_str = true;
                    snprintf(run->question_rpc_id_str, sizeof run->question_rpc_id_str,
                             "%s", yyjson_get_str(id_val));
                } else {
                    run->question_rpc_id_is_str = false;
                    run->question_rpc_id_num = yyjson_get_sint(id_val);
                }
                free(run->question_payload_json);
                run->question_payload_json = NULL;
                pthread_mutex_unlock(&run->mu);
                emit_question_request_ev(run, &pq);
                return;
            }
        }

        if (strcmp(method, "execCommandApproval") == 0 ||
            strcmp(method, "applyPatchApproval") == 0) {
            bool is_patch = (strcmp(method, "applyPatchApproval") == 0);
            const char *reason = jstr(params, "reason");
            const char *cwd = jstr(params, "cwd");
            char *details = stringify_json_val(params);

            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CODEX_LEGACY;
                approval_item_set_rpc_id(ait, id_val);
                ait->title = dup_or_null(is_patch ? "Approve patch?" : "Approve command?");
                ait->tool_name = dup_or_null(is_patch ? "file_change" : "command");
                ait->command = dup_or_null(cwd ? cwd : "");
                ait->reason = dup_or_null(reason ? reason : "");
                ait->details = dup_or_null(details ? details : "");
                ait->can_allow_session = true;
                approval_queue_push(run, ait);
            }
            free(details);
            return;
        }

        if (strcmp(method, "mcpServer/elicitation/request") == 0) {
            const char *msg = jstr(params, "message");
            const char *server = jstr(params, "serverName");
            char *details = stringify_json_val(params);

            char title[160];
            snprintf(title, sizeof title, "MCP request%s%s",
                     (server && server[0]) ? " — " : "", (server && server[0]) ? server : "");
            pending_approval_item *ait = calloc(1, sizeof *ait);
            if (ait) {
                ait->kind = APPROVAL_KIND_CODEX_ELICIT;
                approval_item_set_rpc_id(ait, id_val);
                ait->title = dup_or_null(title);
                ait->tool_name = dup_or_null("mcp");
                ait->command = dup_or_null("");
                ait->reason = dup_or_null(msg ? msg : "An MCP server is requesting confirmation.");
                ait->details = dup_or_null(details ? details : "");
                ait->can_allow_session = false;
                approval_queue_push(run, ait);
            }
            free(details);
            return;
        }

        /* Unknown server request: return JSON-RPC Method not found so server never hangs */
        yyjson_mut_doc *edoc = yyjson_mut_doc_new(NULL);
        if (edoc) {
            yyjson_mut_val *eroot = yyjson_mut_obj(edoc);
            yyjson_mut_doc_set_root(edoc, eroot);
            yyjson_mut_obj_add_str(edoc, eroot, "jsonrpc", "2.0");
            if (yyjson_is_str(id_val)) yyjson_mut_obj_add_strcpy(edoc, eroot, "id", yyjson_get_str(id_val));
            else if (yyjson_is_int(id_val)) yyjson_mut_obj_add_sint(edoc, eroot, "id", yyjson_get_sint(id_val));
            yyjson_mut_val *eobj = yyjson_mut_obj(edoc);
            yyjson_mut_obj_add_int(edoc, eobj, "code", -32601);
            yyjson_mut_obj_add_str(edoc, eobj, "message", "Unsupported client request");
            yyjson_mut_obj_add_val(edoc, eroot, "error", eobj);
            send_json_doc(run->proc, edoc);
            yyjson_mut_doc_free(edoc);
        }
        return;
    }

    if (!run->rpc_prompt_sent) return;

    if (strcmp(method, "serverRequest/resolved") == 0) {
        yyjson_val *req_id = yyjson_is_obj(params) ? yyjson_obj_get(params, "requestId") : NULL;
        approval_queue_cancel(run, req_id, NULL);
        bool clear_q = false;
        pthread_mutex_lock(&run->mu);
        if (run->question_pending) {
            if (!req_id || yyjson_is_null(req_id)) {
                clear_q = true;
            } else if (yyjson_is_str(req_id)) {
                clear_q = run->question_rpc_id_is_str &&
                          strcmp(run->question_rpc_id_str, yyjson_get_str(req_id)) == 0;
            } else if (yyjson_is_int(req_id)) {
                clear_q = !run->question_rpc_id_is_str &&
                          run->question_rpc_id_num == yyjson_get_sint(req_id);
            }
            if (clear_q) run->question_pending = false;
        }
        pthread_mutex_unlock(&run->mu);
        if (clear_q) emit_simple(run, EV_QUESTION_CLEAR, NULL);
        return;
    }

    if (strcmp(method, "item/commandExecution/outputDelta") == 0 ||
        strcmp(method, "item/fileChange/outputDelta") == 0) {
        const char *iid = jstr(params, "itemId");
        const char *delta = jstr(params, "delta");
        if (iid && iid[0] && delta && delta[0]) {
            emit_tool_output_delta(run, iid, delta);
        }
        return;
    }

    if (strcmp(method, "item/mcpToolCall/progress") == 0) {
        const char *iid = jstr(params, "itemId");
        const char *msg = jstr(params, "message");
        if (msg && msg[0]) {
            emit_simple(run, EV_STATUS, msg);
            if (iid && iid[0]) {
                char line[512];
                snprintf(line, sizeof line, "%s\n", msg);
                emit_tool_output_delta(run, iid, line);
            }
        }
        return;
    }

    if (strcmp(method, "turn/plan/updated") == 0) {
        const char *expl = jstr(params, "explanation");
        yyjson_val *plan = yyjson_is_obj(params) ? yyjson_obj_get(params, "plan") : NULL;
        if (yyjson_is_arr(plan)) {
            mcode_plan_step steps[MCODE_MAX_PLAN_STEPS];
            int sc = 0;
            size_t pi, pmax;
            yyjson_val *pv;
            yyjson_arr_foreach(plan, pi, pmax, pv) {
                if (sc >= MCODE_MAX_PLAN_STEPS || !yyjson_is_obj(pv)) continue;
                const char *stxt = jstr(pv, "step");
                const char *st = jstr(pv, "status");
                if (!stxt || !stxt[0]) continue;
                memset(&steps[sc], 0, sizeof steps[sc]);
                snprintf(steps[sc].title, sizeof steps[sc].title, "%s", stxt);
                steps[sc].status = parse_plan_step_status(st);
                ++sc;
            }
            emit_plan_update_ev(run, (expl && expl[0]) ? expl : "Plan", steps, sc);
        }
        return;
    }

    if (strcmp(method, "item/plan/delta") == 0) {
        const char *delta = jstr(params, "delta");
        if (delta && delta[0]) {
            emit_simple(run, EV_REASONING_DONE, NULL);
            emit_simple(run, EV_PLAN_DELTA, delta);
            emit_simple(run, EV_STATUS, "Drafting plan...");
        }
        return;
    }

    if (strcmp(method, "thread/compacted") == 0) {
        emit_notice_ev(run, "Context compacted", CUI_TONE_NEUTRAL);
        return;
    }

    if (strcmp(method, "warning") == 0 ||
        strcmp(method, "guardianWarning") == 0 ||
        strcmp(method, "configWarning") == 0 ||
        strcmp(method, "deprecationNotice") == 0) {
        const char *msg = jstr(params, "message");
        if (!msg) msg = jstr(params, "summary");
        if (!msg) msg = jstr(params, "details");
        if (msg && msg[0]) {
            emit_notice_ev(run, msg, CUI_TONE_WARNING);
        }
        return;
    }

    if (strcmp(method, "model/rerouted") == 0) {
        const char *from_m = jstr(params, "fromModel");
        const char *to_m = jstr(params, "toModel");
        if (from_m && from_m[0] && to_m && to_m[0]) {
            char msg[256];
            snprintf(msg, sizeof msg, "Model rerouted: %s → %s", from_m, to_m);
            emit_notice_ev(run, msg, CUI_TONE_INFO);
        }
        return;
    }

    if (strcmp(method, "error") == 0) {
        yyjson_val *err_v = yyjson_is_obj(params) ? yyjson_obj_get(params, "error") : NULL;
        yyjson_val *wr_v = yyjson_is_obj(params) ? yyjson_obj_get(params, "willRetry") : NULL;
        bool will_retry = yyjson_is_bool(wr_v) && yyjson_get_bool(wr_v);
        const char *emsg = yyjson_is_obj(err_v) ? jstr(err_v, "message") : jstr(params, "message");
        if (emsg && emsg[0]) {
            if (will_retry) {
                char st[160];
                snprintf(st, sizeof st, "Retrying: %.120s", emsg);
                emit_simple(run, EV_STATUS, st);
            } else {
                emit_simple(run, EV_ERROR, emsg);
            }
        }
        return;
    }

    if (strcmp(method, "item/agentMessage/delta") == 0) {
        const char *delta = jstr(params, "delta");
        if (delta && delta[0]) {
            if (run->need_text_sep) {
                emit_simple(run, EV_TEXT_DELTA, "\n\n");
                run->need_text_sep = false;
            }
            run->streamed_text_deltas = true;
            run->has_emitted_text = true;
            emit_simple(run, EV_REASONING_DONE, NULL);
            emit_simple(run, EV_TEXT_DELTA, delta);
            emit_simple(run, EV_STATUS, "Responding...");
        }
        return;
    }

    if (strcmp(method, "item/reasoning/textDelta") == 0 ||
        strcmp(method, "item/reasoning/summaryTextDelta") == 0) {
        const char *delta = jstr(params, "delta");
        if (delta && delta[0]) {
            run->streamed_thinking_deltas = true;
            emit_simple(run, EV_REASONING_DELTA, delta);
            emit_simple(run, EV_STATUS, "Reasoning...");
        }
        return;
    }

    if (strcmp(method, "item/started") == 0 || strcmp(method, "item/completed") == 0) {
        bool is_done = (strcmp(method, "item/completed") == 0);
        yyjson_val *item = yyjson_is_obj(params) ? yyjson_obj_get(params, "item") : NULL;
        if (!yyjson_is_obj(item)) return;
        const char *itype = jstr(item, "type");
        const char *iid = jstr(item, "id");
        if (!itype) return;

        if (strcmp(itype, "agentMessage") == 0) {
            if (!is_done) {
                run->streamed_text_deltas = false;
                run->need_text_sep = run->has_emitted_text;
            } else {
                const char *txt = jstr(item, "text");
                if (txt && txt[0] && !run->streamed_text_deltas) {
                    if (run->need_text_sep) {
                        emit_simple(run, EV_TEXT_DELTA, "\n\n");
                        run->need_text_sep = false;
                    }
                    run->has_emitted_text = true;
                    emit_simple(run, EV_REASONING_DONE, NULL);
                    emit_simple(run, EV_TEXT_DELTA, txt);
                }
                run->streamed_text_deltas = false;
            }
        } else if (strcmp(itype, "reasoning") == 0) {
            if (!is_done) {
                emit_simple(run, EV_STATUS, "Reasoning...");
            } else {
                emit_simple(run, EV_REASONING_DONE, NULL);
            }
        } else if (strcmp(itype, "commandExecution") == 0) {
            run->has_emitted_text = false;
            const char *cmd = jstr(item, "command");
            const char *out = jstr(item, "aggregatedOutput");
            yyjson_val *ec = yyjson_obj_get(item, "exitCode");
            bool has_ec = yyjson_is_num(ec);
            int exit_code = has_ec ? (int)yyjson_get_num(ec) : -1;
            const char *status = jstr(item, "status");
            bool failed = (has_ec && exit_code != 0) ||
                          (status && (strcmp(status, "failed") == 0 || strcmp(status, "declined") == 0));
            if (failed && !(status && strcmp(status, "declined") == 0) &&
                mcode_command_is_benign_probe_exit(cmd, out, exit_code)) {
                failed = false;
            }
            char short_cmd[128] = "";
            summarize_command_detail(cmd, short_cmd, sizeof short_cmd);
            if (!is_done) {
                emit_tool_start(run, iid ? iid : "cmd", "bash", short_cmd, cmd ? cmd : "");
                char st[160];
                snprintf(st, sizeof st, "Running: %s", short_cmd[0] ? short_cmd : "command");
                emit_simple(run, EV_STATUS, st);
            } else {
                emit_tool_start(run, iid ? iid : "cmd", "bash", short_cmd, cmd ? cmd : "");
                const char *res_out = out;
                if ((!res_out || !res_out[0]) && status && strcmp(status, "declined") == 0) {
                    res_out = "Permission denied by user.";
                }
                emit_tool_result_code(run, iid ? iid : "cmd", res_out ? res_out : "", failed, exit_code);
            }
        } else if (strcmp(itype, "fileChange") == 0) {
            run->has_emitted_text = false;
            yyjson_val *changes = yyjson_obj_get(item, "changes");
            mcode_str summary;
            mcode_str_init(&summary);
            char first_path[256] = "";
            if (yyjson_is_arr(changes)) {
                size_t idx, max;
                yyjson_val *c;
                yyjson_arr_foreach(changes, idx, max, c) {
                    const char *p = jstr(c, "path");
                    const char *k = jstr(c, "kind");
                    if (p) {
                        if (!first_path[0]) snprintf(first_path, sizeof first_path, "%s", p);
                        if (summary.len > 0) mcode_str_append_cstr(&summary, "\n");
                        mcode_str_append_cstr(&summary, k ? k : "edit");
                        mcode_str_append_cstr(&summary, " ");
                        mcode_str_append_cstr(&summary, p);
                    }
                }
            }
            const char *status = jstr(item, "status");
            bool failed = status && (strcmp(status, "failed") == 0 || strcmp(status, "declined") == 0);
            emit_tool_start(run, iid ? iid : "file", "file_change", first_path, mcode_str_cstr(&summary));
            if (is_done) {
                emit_tool_result(run, iid ? iid : "file",
                                 (failed && summary.len == 0) ? "File change declined or failed."
                                                              : mcode_str_cstr(&summary),
                                 failed);
            }
            mcode_str_free(&summary);
        } else if (strcmp(itype, "mcpToolCall") == 0 || strcmp(itype, "webSearch") == 0) {
            run->has_emitted_text = false;
            const char *tool = jstr(item, "tool");
            const char *query = jstr(item, "query");
            char *args_json = stringify_json_val(yyjson_obj_get(item, "arguments"));
            emit_tool_start(run, iid ? iid : itype, tool ? tool : itype,
                            query ? query : "", args_json ? args_json : "");
            free(args_json);
            if (is_done) {
                char *res_json = stringify_json_val(yyjson_obj_get(item, "result"));
                const char *err_s = jstr(item, "error");
                emit_tool_result(run, iid ? iid : itype,
                                 err_s ? err_s : (res_json ? res_json : ""),
                                 err_s != NULL);
                free(res_json);
            }
        } else if (strcmp(itype, "imageGeneration") == 0 || strcmp(itype, "image_generation") == 0 ||
                   (strcmp(itype, "Extension") == 0 &&
                    jstr(item, "kind") && strcmp(jstr(item, "kind"), "image_gen.generation") == 0)) {
            handle_codex_image_gen_item(run, item, iid, is_done);
        } else if (strcmp(itype, "imageView") == 0 || strcmp(itype, "image_view") == 0) {
            handle_codex_image_view_item(run, item, iid, is_done);
        } else if (strcmp(itype, "plan") == 0) {
            run->has_emitted_text = false;
            if (!is_done) {
                emit_simple(run, EV_STATUS, "Drafting plan...");
            } else {
                const char *ptxt = jstr(item, "text");
                if (ptxt && ptxt[0]) {
                    emit_simple(run, EV_REASONING_DONE, NULL);
                    emit_simple(run, EV_PLAN_SET, ptxt);
                }
            }
        } else if (strcmp(itype, "dynamicToolCall") == 0) {
            run->has_emitted_text = false;
            const char *ns = jstr(item, "namespace");
            const char *tool = jstr(item, "tool");
            char tname[96];
            if (ns && ns[0] && tool && tool[0]) snprintf(tname, sizeof tname, "%s.%s", ns, tool);
            else snprintf(tname, sizeof tname, "%s", (tool && tool[0]) ? tool : "tool");
            const char *tid = (iid && iid[0]) ? iid : tname;
            char *args_json = stringify_json_val(yyjson_obj_get(item, "arguments"));
            char detail[256] = "";
            summarize_tool_input(yyjson_obj_get(item, "arguments"), detail, sizeof detail);
            emit_tool_start(run, tid, tname, detail, args_json ? args_json : "");
            free(args_json);
            if (!is_done) {
                char st[160];
                snprintf(st, sizeof st, "Running %s...", tname);
                emit_simple(run, EV_STATUS, st);
            } else {
                const char *status = jstr(item, "status");
                yyjson_val *succ_v = yyjson_obj_get(item, "success");
                bool failed = (yyjson_is_bool(succ_v) && !yyjson_get_bool(succ_v)) ||
                              (status && strcmp(status, "failed") == 0);
                yyjson_val *citems = yyjson_obj_get(item, "contentItems");
                mcode_str joined;
                mcode_str_init(&joined);
                if (yyjson_is_arr(citems)) {
                    size_t ci, cmax;
                    yyjson_val *cv;
                    yyjson_arr_foreach(citems, ci, cmax, cv) {
                        const char *ct = jstr(cv, "text");
                        if (ct && ct[0]) {
                            if (joined.len > 0) mcode_str_append_cstr(&joined, "\n");
                            mcode_str_append_cstr(&joined, ct);
                        } else if (!failed) {
                            emit_json_image_content(run, cv, tid);
                        }
                    }
                }
                emit_tool_result(run, tid, mcode_str_cstr(&joined), failed);
                mcode_str_free(&joined);
            }
        } else if (strcmp(itype, "collabAgentToolCall") == 0) {
            run->has_emitted_text = false;
            const char *tool = jstr(item, "tool");
            const char *prompt = jstr(item, "prompt");
            const char *model = jstr(item, "model");
            const char *status = jstr(item, "status");
            const char *tid = (iid && iid[0]) ? iid : "collab";
            char tname[96];
            snprintf(tname, sizeof tname, "subagent_%s", (tool && tool[0]) ? tool : "call");
            char detail[256] = "";
            if (prompt && prompt[0]) snprintf(detail, sizeof detail, "%.120s", prompt);
            else if (model && model[0]) snprintf(detail, sizeof detail, "%s", model);
            emit_tool_start(run, tid, tname, detail, prompt ? prompt : "");
            if (!is_done) {
                char st[160];
                snprintf(st, sizeof st, "Sub-agent: %s...", (tool && tool[0]) ? tool : "working");
                emit_simple(run, EV_STATUS, st);
            } else {
                bool failed = status && strcmp(status, "failed") == 0;
                char *states = stringify_json_val(yyjson_obj_get(item, "agentsStates"));
                emit_tool_result(run, tid, states ? states : (status ? status : "completed"), failed);
                free(states);
            }
        } else if (strcmp(itype, "subAgentActivity") == 0) {
            run->has_emitted_text = false;
            const char *apath = jstr(item, "agentPath");
            const char *model = jstr(item, "model");
            yyjson_val *kind_v = yyjson_obj_get(item, "kind");
            const char *kind_s = yyjson_is_str(kind_v) ? yyjson_get_str(kind_v)
                               : (yyjson_is_obj(kind_v) ? jstr(kind_v, "type") : NULL);
            const char *tid = (iid && iid[0]) ? iid : "subagent";
            char detail[256] = "";
            snprintf(detail, sizeof detail, "%s%s%s%s%s",
                     (apath && apath[0]) ? apath : "subagent",
                     (kind_s && kind_s[0]) ? " · " : "",
                     (kind_s && kind_s[0]) ? kind_s : "",
                     (model && model[0]) ? " (" : "",
                     (model && model[0]) ? model : "");
            if (model && model[0]) {
                size_t dl = strlen(detail);
                if (dl + 1 < sizeof detail) { detail[dl] = ')'; detail[dl + 1] = '\0'; }
            }
            emit_tool_start(run, tid, "subagent", detail, "");
            if (is_done) {
                emit_tool_result(run, tid, "", false);
            }
        } else if (strcmp(itype, "functionCallOutput") == 0) {
            if (is_done) {
                yyjson_val *out_v = yyjson_obj_get(item, "output");
                if (yyjson_is_arr(out_v)) {
                    size_t oi, omax;
                    yyjson_val *ov;
                    yyjson_arr_foreach(out_v, oi, omax, ov) {
                        emit_json_image_content(run, ov, iid ? iid : "fn");
                    }
                }
            }
        } else if (strcmp(itype, "sleep") == 0) {
            run->has_emitted_text = false;
            int64_t ms = ji64(item, "durationMs");
            char dur[48];
            if (ms >= 1000) snprintf(dur, sizeof dur, "%llds", (long long)(ms / 1000));
            else snprintf(dur, sizeof dur, "%lldms", (long long)ms);
            const char *tid = (iid && iid[0]) ? iid : "sleep";
            emit_tool_start(run, tid, "sleep", dur, "");
            if (!is_done) {
                char st[96];
                snprintf(st, sizeof st, "Waiting %s...", dur);
                emit_simple(run, EV_STATUS, st);
            } else {
                emit_tool_result(run, tid, "", false);
            }
        } else if (strcmp(itype, "enteredReviewMode") == 0) {
            if (is_done) {
                const char *rev = jstr(item, "review");
                char msg[256];
                if (rev && rev[0]) snprintf(msg, sizeof msg, "Entered review mode: %s", rev);
                else snprintf(msg, sizeof msg, "Entered review mode");
                emit_notice_ev(run, msg, CUI_TONE_INFO);
            }
        } else if (strcmp(itype, "exitedReviewMode") == 0) {
            if (is_done) {
                emit_notice_ev(run, "Exited review mode", CUI_TONE_NEUTRAL);
            }
        } else if (strcmp(itype, "contextCompaction") == 0) {
            if (!is_done) {
                emit_simple(run, EV_STATUS, "Compacting context...");
            } else {
                emit_notice_ev(run, "Context compacted", CUI_TONE_NEUTRAL);
            }
        }
        return;
    }

    if (strcmp(method, "account/rateLimits/updated") == 0) {
        yyjson_val *rl = yyjson_is_obj(params) ? yyjson_obj_get(params, "rateLimits") : NULL;
        mcode_provider_quota q;
        memset(&q, 0, sizeof q);
        if (parse_codex_rate_snapshot(rl, &q)) {
            emit_rate_limit_ev(run, &q);
        }
        return;
    }

    if (strcmp(method, "thread/tokenUsage/updated") == 0) {
        yyjson_val *tu = yyjson_is_obj(params) ? yyjson_obj_get(params, "tokenUsage") : NULL;
        yyjson_val *tot = yyjson_is_obj(tu) ? yyjson_obj_get(tu, "total") : NULL;
        yyjson_val *last = yyjson_is_obj(tu) ? yyjson_obj_get(tu, "last") : NULL;
        if (yyjson_is_obj(tot)) {
            run->codex_turn_in_tokens = ji64(tot, "inputTokens");
            run->codex_turn_out_tokens = ji64(tot, "outputTokens");
            run->codex_turn_cached_tokens = ji64(tot, "cachedInputTokens");
        }
        if (yyjson_is_obj(last)) {
            int64_t lt = ji64(last, "totalTokens");
            if (lt <= 0) lt = ji64(last, "inputTokens") + ji64(last, "outputTokens");
            if (lt > 0) run->codex_last_ctx_tokens = lt;
        }
        int64_t cw = ji64(tu, "modelContextWindow");
        if (cw > 0) run->codex_ctx_window = cw;
        if (run->codex_turn_in_tokens > 0 || run->codex_turn_out_tokens > 0 ||
            run->codex_last_ctx_tokens > 0) {
            run->codex_emitted_live_usage = true;
            emit_usage_ev(run, run->codex_turn_in_tokens, run->codex_turn_out_tokens,
                          run->codex_turn_cached_tokens, run->codex_last_ctx_tokens,
                          run->codex_ctx_window, 0.0, true);
        }
        return;
    }

    if (strcmp(method, "turn/started") == 0) {
        yyjson_val *turn = yyjson_is_obj(params) ? yyjson_obj_get(params, "turn") : NULL;
        const char *turn_id = jstr(turn, "id");
        if (turn_id && turn_id[0]) {
            codex_rpc_set_active_turn(run, turn_id);
        }
        return;
    }

    if (strcmp(method, "turn/completed") == 0) {
        pthread_mutex_lock(&run->mu);
        bool steer_int = run->steer_interrupting;
        pthread_mutex_unlock(&run->mu);
        yyjson_val *turn = yyjson_is_obj(params) ? yyjson_obj_get(params, "turn") : NULL;
        yyjson_val *err_v = yyjson_is_obj(turn) ? yyjson_obj_get(turn, "error") : NULL;
        if (err_v && !yyjson_is_null(err_v) && !steer_int) {
            const char *emsg = yyjson_is_obj(err_v) ? jstr(err_v, "message") : NULL;
            char *s = emsg ? strdup(emsg) : stringify_json_val(err_v);
            if (s && s[0]) emit_simple(run, EV_ERROR, s);
            free(s);
        }
        codex_rpc_finish_turn(run);
    }
}

/* ---- Antigravity `agy acp` JSON-RPC parser & handshake ---- */
static void agy_acp_send_initialize(mcode_agent_run *run) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", 1);
    yyjson_mut_obj_add_str(doc, root, "method", "initialize");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, params, "protocolVersion", 1);
    yyjson_mut_obj_add_val(doc, params, "clientCapabilities", yyjson_mut_obj(doc));
    yyjson_mut_obj_add_val(doc, root, "params", params);
    send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
}

static void agy_acp_send_session_open(mcode_agent_run *run, bool load, int rpc_id) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", rpc_id);
    yyjson_mut_obj_add_str(doc, root, "method", load ? "session/load" : "session/new");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    if (load) {
        yyjson_mut_obj_add_strcpy(doc, params, "sessionId", run->init_session_id);
    }
    yyjson_mut_obj_add_strcpy(doc, params, "cwd", run->init_cwd[0] ? run->init_cwd : ".");
    yyjson_mut_obj_add_val(doc, params, "mcpServers", yyjson_mut_arr(doc));
    yyjson_mut_obj_add_val(doc, root, "params", params);
    send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
}

static bool agy_acp_send_cancel(mcode_agent_run *run) {
    if (!run || !run->proc) return false;
    char sid[MCODE_SESSION_MAX] = "";
    pthread_mutex_lock(&run->mu);
    snprintf(sid, sizeof sid, "%s", run->active_session_id);
    pthread_mutex_unlock(&run->mu);
    if (!sid[0]) return false;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, root, "method", "session/cancel");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "sessionId", sid);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

static void agy_append_prompt_parts(yyjson_mut_doc *doc, yyjson_mut_val *prompt_arr,
                                    const char *text, const char *workdir) {
    char img_paths[MCODE_PROMPT_IMAGE_MAX][MCODE_PATH_MAX];
    int img_count = extract_prompt_local_images(text, workdir, img_paths, MCODE_PROMPT_IMAGE_MAX);
    for (int i = 0; i < img_count; ++i) {
        const char *mime = NULL;
        char *b64 = NULL;
        if (encode_file_base64(img_paths[i], &mime, &b64)) {
            yyjson_mut_val *im = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, im, "type", "image");
            yyjson_mut_obj_add_strcpy(doc, im, "mimeType", mime);
            yyjson_mut_obj_add_strcpy(doc, im, "data", b64);
            yyjson_mut_arr_append(prompt_arr, im);
            free(b64);
        }
    }
    yyjson_mut_val *part = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, part, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, part, "text", text ? text : "");
    yyjson_mut_arr_append(prompt_arr, part);
}

static bool agy_acp_send_steer_prompt(mcode_agent_run *run, const char *session_id,
                                      const char *message) {
    if (!run || !run->proc || !session_id || !session_id[0] || !message || !message[0]) {
        return false;
    }

    int64_t rpc_id = 0;
    bool tracked_open = false;
    bool tracked_steer = false;
    pthread_mutex_lock(&run->mu);
    if (run->next_rpc_id < 100) run->next_rpc_id = 100;
    rpc_id = run->next_rpc_id++;
    if (run->agy_open_prompt_count < 16) {
        run->agy_open_prompts[run->agy_open_prompt_count++] = rpc_id;
        tracked_open = true;
    }
    if (run->inflight_steer_count < 16) {
        char *copy = strdup(message);
        if (copy) {
            int idx = run->inflight_steer_count++;
            run->inflight_steer_ids[idx] = rpc_id;
            run->inflight_steer_texts[idx] = copy;
            tracked_steer = true;
        }
    }
    pthread_mutex_unlock(&run->mu);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        pthread_mutex_lock(&run->mu);
        if (tracked_open) {
            for (int i = 0; i < run->agy_open_prompt_count; ++i) {
                if (run->agy_open_prompts[i] == rpc_id) {
                    for (int j = i + 1; j < run->agy_open_prompt_count; ++j) {
                        run->agy_open_prompts[j - 1] = run->agy_open_prompts[j];
                    }
                    run->agy_open_prompt_count--;
                    break;
                }
            }
        }
        if (tracked_steer) {
            for (int i = 0; i < run->inflight_steer_count; ++i) {
                if (run->inflight_steer_ids[i] == rpc_id) {
                    free(run->inflight_steer_texts[i]);
                    for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                        run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                        run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                    }
                    run->inflight_steer_count--;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&run->mu);
        return false;
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_sint(doc, root, "id", rpc_id);
    yyjson_mut_obj_add_str(doc, root, "method", "session/prompt");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "sessionId", session_id);
    yyjson_mut_val *prompt_arr = yyjson_mut_arr(doc);
    agy_append_prompt_parts(doc, prompt_arr, message, run->init_cwd);
    yyjson_mut_obj_add_val(doc, params, "prompt", prompt_arr);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);

    if (!ok) {
        pthread_mutex_lock(&run->mu);
        if (tracked_open) {
            for (int i = 0; i < run->agy_open_prompt_count; ++i) {
                if (run->agy_open_prompts[i] == rpc_id) {
                    for (int j = i + 1; j < run->agy_open_prompt_count; ++j) {
                        run->agy_open_prompts[j - 1] = run->agy_open_prompts[j];
                    }
                    run->agy_open_prompt_count--;
                    break;
                }
            }
        }
        if (tracked_steer) {
            for (int i = 0; i < run->inflight_steer_count; ++i) {
                if (run->inflight_steer_ids[i] == rpc_id) {
                    free(run->inflight_steer_texts[i]);
                    for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                        run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                        run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                    }
                    run->inflight_steer_count--;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&run->mu);
    }
    return ok;
}

static void agy_acp_finish_turn(mcode_agent_run *run);

static void agy_acp_send_configure_and_prompt(mcode_agent_run *run) {
    pthread_mutex_lock(&run->mu);
    bool already_interrupted = run->deferred_interrupt;
    pthread_mutex_unlock(&run->mu);
    if (already_interrupted) {
        agy_acp_finish_turn(run);
        return;
    }

    /* 1. Configure permission mode unless running with --dangerously-skip-permissions */
    if (run->init_perm_mode != MCODE_PERM_FULL_ACCESS) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (doc) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            yyjson_mut_obj_add_int(doc, root, "id", 3);
            yyjson_mut_obj_add_str(doc, root, "method", "session/set_mode");
            yyjson_mut_val *params = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, params, "sessionId", run->active_session_id);
            const char *mode_id = (run->init_perm_mode == MCODE_PERM_PLAN) ? "plan"
                                : (run->init_perm_mode == MCODE_PERM_SUPERVISED) ? "default"
                                : "accept-edits";
            yyjson_mut_obj_add_str(doc, params, "modeId", mode_id);
            yyjson_mut_obj_add_val(doc, root, "params", params);
            send_json_doc(run->proc, doc);
            yyjson_mut_doc_free(doc);
        }
    }
    /* 2. Apply selected model if configured */
    if (run->init_model[0]) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (doc) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            yyjson_mut_obj_add_int(doc, root, "id", 4);
            yyjson_mut_obj_add_str(doc, root, "method", "session/set_model");
            yyjson_mut_val *params = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, params, "sessionId", run->active_session_id);
            yyjson_mut_obj_add_strcpy(doc, params, "modelId", run->init_model);
            yyjson_mut_obj_add_val(doc, root, "params", params);
            send_json_doc(run->proc, doc);
            yyjson_mut_doc_free(doc);
        }
    }
    /* 3. Send the initial prompt and flush any early buffered live steers */
    char sid[MCODE_SESSION_MAX] = "";
    char **queued = NULL;
    int queued_n = 0;
    {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (doc) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            yyjson_mut_obj_add_int(doc, root, "id", 5);
            yyjson_mut_obj_add_str(doc, root, "method", "session/prompt");
            yyjson_mut_val *params = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, params, "sessionId", run->active_session_id);
            yyjson_mut_val *prompt_arr = yyjson_mut_arr(doc);
            agy_append_prompt_parts(doc, prompt_arr,
                                    run->init_prompt ? run->init_prompt : "", run->init_cwd);
            yyjson_mut_obj_add_val(doc, params, "prompt", prompt_arr);
            yyjson_mut_obj_add_val(doc, root, "params", params);
            pthread_mutex_lock(&run->mu);
            run->rpc_prompt_sent = true;
            run->agy_open_prompts[0] = 5;
            run->agy_open_prompt_count = 1;
            snprintf(sid, sizeof sid, "%s", run->active_session_id);
            if (run->pending_live_steer_count > 0) {
                queued = run->pending_live_steers;
                queued_n = run->pending_live_steer_count;
                run->pending_live_steers = NULL;
                run->pending_live_steer_count = 0;
                run->pending_live_steer_cap = 0;
            }
            pthread_mutex_unlock(&run->mu);
            send_json_doc(run->proc, doc);
            yyjson_mut_doc_free(doc);
        }
    }
    for (int i = 0; i < queued_n; ++i) {
        if (queued[i] && queued[i][0]) {
            if (!agy_acp_send_steer_prompt(run, sid, queued[i])) {
                emit_simple(run, EV_STEER_REJECTED, queued[i]);
            }
        }
        free(queued[i]);
    }
    free(queued);
}

static void agy_acp_finish_turn(mcode_agent_run *run) {
    flush_unsent_live_steers(run);
    pthread_mutex_lock(&run->mu);
    run->rpc_turn_done = true;
    approval_queue_clear_locked(run);
    pthread_mutex_unlock(&run->mu);
    emit_simple(run, EV_APPROVAL_CLEAR, NULL);
    emit_simple(run, EV_REASONING_DONE, NULL);
    mcode_proc_stop(run->proc);
}

static char *extract_acp_tool_output(yyjson_val *upd) {
    yyjson_val *raw_out = yyjson_obj_get(upd, "rawOutput");
    if (raw_out && !yyjson_is_null(raw_out)) {
        char *s = stringify_json_val(raw_out);
        if (s && s[0]) return s;
        free(s);
    }
    yyjson_val *content = yyjson_obj_get(upd, "content");
    if (yyjson_is_arr(content)) {
        mcode_str joined;
        mcode_str_init(&joined);
        size_t i, max;
        yyjson_val *item;
        yyjson_arr_foreach(content, i, max, item) {
            const char *itype = jstr(item, "type");
            if (itype && strcmp(itype, "diff") == 0) {
                const char *dp = jstr(item, "path");
                const char *nt = jstr(item, "newText");
                if (joined.len > 0) mcode_str_append_cstr(&joined, "\n");
                if (dp && dp[0]) {
                    mcode_str_append_cstr(&joined, "diff ");
                    mcode_str_append_cstr(&joined, dp);
                    if (nt && nt[0]) mcode_str_append_cstr(&joined, "\n");
                }
                if (nt && nt[0]) mcode_str_append_cstr(&joined, nt);
                continue;
            }
            if (itype && strcmp(itype, "resource_link") == 0) {
                const char *uri = jstr(item, "uri");
                const char *name = jstr(item, "name");
                if ((uri && uri[0]) || (name && name[0])) {
                    if (joined.len > 0) mcode_str_append_cstr(&joined, "\n");
                    if (name && name[0]) {
                        mcode_str_append_cstr(&joined, name);
                        if (uri && uri[0]) mcode_str_append_cstr(&joined, " — ");
                    }
                    if (uri && uri[0]) mcode_str_append_cstr(&joined, uri);
                }
                continue;
            }
            const char *txt = jstr(item, "text");
            if (!txt) {
                yyjson_val *inner = yyjson_obj_get(item, "content");
                txt = jstr(inner, "text");
            }
            if (txt && txt[0]) {
                if (joined.len > 0) mcode_str_append_cstr(&joined, "\n");
                mcode_str_append_cstr(&joined, txt);
            }
        }
        if (joined.len > 0) return joined.data;
        mcode_str_free(&joined);
    }
    return NULL;
}

static void parse_agy_acp_json(mcode_agent_run *run, yyjson_val *root) {
    const char *method = jstr(root, "method");
    yyjson_val *id_val = yyjson_obj_get(root, "id");

    /* Responses to our outgoing JSON-RPC requests */
    if (!method && id_val && yyjson_is_int(id_val)) {
        int64_t rid = yyjson_get_sint(id_val);
        yyjson_val *err_v = yyjson_obj_get(root, "error");
        yyjson_val *res_v = yyjson_obj_get(root, "result");

        if (rid == 1) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                emit_simple(run, EV_ERROR, emsg ? emsg : "Antigravity ACP initialize failed.");
                agy_acp_finish_turn(run);
                return;
            }
            agy_acp_send_session_open(run, run->init_session_id[0] != '\0', 2);
            return;
        }

        if (rid == 2 || rid == 20) {
            if (err_v) {
                if (rid == 2 && run->init_session_id[0]) {
                    /* Session could not be loaded: fall back to a fresh session */
                    run->init_session_id[0] = '\0';
                    agy_acp_send_session_open(run, false, 20);
                    return;
                }
                const char *emsg = jstr(err_v, "message");
                emit_simple(run, EV_ERROR, emsg ? emsg : "Antigravity ACP session open failed.");
                agy_acp_finish_turn(run);
                return;
            }
            const char *sid = jstr(res_v, "sessionId");
            if (!sid || !sid[0]) sid = run->init_session_id;
            if (sid && sid[0]) {
                pthread_mutex_lock(&run->mu);
                snprintf(run->active_session_id, sizeof run->active_session_id, "%s", sid);
                pthread_mutex_unlock(&run->mu);
                emit_simple(run, EV_SESSION, sid);
                agy_acp_send_configure_and_prompt(run);
            } else {
                emit_simple(run, EV_ERROR, "Antigravity ACP did not return a session ID.");
                agy_acp_finish_turn(run);
            }
            return;
        }

        /* Responses to the initial `session/prompt` (id 5) or any live-steer `session/prompt` (id >= 100).
         * When a live steer joins a running turn, `agy acp` immediately resolves the earlier prompt ID
         * and transfers turn ownership to the new prompt ID — so we only finish once all open prompts close. */
        bool matched_prompt = false;
        bool all_prompts_closed = false;
        bool matched_steer = false;
        bool steer_int = false;
        char *steer_text = NULL;
        pthread_mutex_lock(&run->mu);
        steer_int = run->steer_interrupting;
        for (int i = 0; i < run->agy_open_prompt_count; ++i) {
            if (run->agy_open_prompts[i] == rid) {
                matched_prompt = true;
                for (int j = i + 1; j < run->agy_open_prompt_count; ++j) {
                    run->agy_open_prompts[j - 1] = run->agy_open_prompts[j];
                }
                run->agy_open_prompt_count--;
                break;
            }
        }
        if (!matched_prompt && rid == 5) {
            matched_prompt = true;
        }
        all_prompts_closed = (run->agy_open_prompt_count == 0);
        for (int i = 0; i < run->inflight_steer_count; ++i) {
            if (run->inflight_steer_ids[i] == rid) {
                matched_steer = true;
                steer_text = run->inflight_steer_texts[i];
                for (int j = i + 1; j < run->inflight_steer_count; ++j) {
                    run->inflight_steer_ids[j - 1] = run->inflight_steer_ids[j];
                    run->inflight_steer_texts[j - 1] = run->inflight_steer_texts[j];
                }
                run->inflight_steer_count--;
                break;
            }
        }
        pthread_mutex_unlock(&run->mu);

        if (matched_prompt) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                if (matched_steer && !steer_int && steer_text && steer_text[0]) {
                    app_log_warn("Antigravity ACP live steer rejected (%s); falling back to next turn",
                                 emsg ? emsg : "unknown error");
                    emit_simple(run, EV_STEER_REJECTED, steer_text);
                } else if (!steer_int) {
                    emit_simple(run, EV_ERROR, emsg ? emsg : "Antigravity ACP prompt failed.");
                }
            } else if (yyjson_is_obj(res_v)) {
                yyjson_val *usage = yyjson_obj_get(res_v, "usage");
                if (yyjson_is_obj(usage)) {
                    int64_t in_tok = ji64(usage, "inputTokens");
                    int64_t out_tok = ji64(usage, "outputTokens");
                    int64_t cached = ji64(usage, "cachedReadTokens");
                    int64_t ctx_win = ji64(usage, "contextWindow");
                    if (in_tok > 0 || out_tok > 0 || cached > 0) {
                        emit_usage_ev(run, in_tok, out_tok, cached, 0, ctx_win, 0.0, false);
                    }
                }
                if (!all_prompts_closed) {
                    /* Earlier prompt resolved because a live steer prompt took over the active turn */
                    emit_simple(run, EV_STATUS, "Steered — continuing...");
                }
            }
            free(steer_text);
            if (all_prompts_closed) {
                agy_acp_finish_turn(run);
            }
        } else {
            free(steer_text);
        }
        return;
    }

    if (!method) return;
    yyjson_val *params = yyjson_obj_get(root, "params");

    /* Interactive permission request from Antigravity ACP */
    if (strcmp(method, "session/request_permission") == 0 && id_val && !yyjson_is_null(id_val)) {
        yyjson_val *tc = yyjson_is_obj(params) ? yyjson_obj_get(params, "toolCall") : NULL;
        const char *tname = jstr(tc, "name");
        const char *tc_title = jstr(tc, "title");
        if (!tname || !tname[0]) tname = "tool";
        yyjson_val *raw_in = yyjson_is_obj(tc) ? yyjson_obj_get(tc, "rawInput") : NULL;
        if (!raw_in && yyjson_is_obj(tc)) raw_in = yyjson_obj_get(tc, "args");

        char detail[256] = "";
        summarize_tool_input(raw_in, detail, sizeof detail);
        if (!detail[0] && tc_title && tc_title[0]) {
            snprintf(detail, sizeof detail, "%s", tc_title);
        }
        const char *summary = jstr(raw_in, "toolSummary");
        char *pretty_in = stringify_json_val(raw_in);

        char allow_opt[64] = "";
        char allow_always_opt[64] = "";
        char deny_opt[64] = "";
        yyjson_val *opts = yyjson_is_obj(params) ? yyjson_obj_get(params, "options") : NULL;
        if (yyjson_is_arr(opts)) {
            size_t i, max;
            yyjson_val *ov;
            yyjson_arr_foreach(opts, i, max, ov) {
                const char *oid = jstr(ov, "optionId");
                const char *kind = jstr(ov, "kind");
                if (!oid || !oid[0]) continue;
                if (kind && strcmp(kind, "allow_always") == 0 && !allow_always_opt[0]) {
                    snprintf(allow_always_opt, sizeof allow_always_opt, "%s", oid);
                } else if (kind && strncmp(kind, "allow", 5) == 0 && !allow_opt[0]) {
                    snprintf(allow_opt, sizeof allow_opt, "%s", oid);
                } else if (kind && strncmp(kind, "reject", 6) == 0 && !deny_opt[0]) {
                    snprintf(deny_opt, sizeof deny_opt, "%s", oid);
                }
            }
        }
        if (!allow_opt[0]) snprintf(allow_opt, sizeof allow_opt, "allow");
        if (!deny_opt[0]) snprintf(deny_opt, sizeof deny_opt, "deny");

        char title[160];
        if (strcmp(tname, "run_command") == 0) {
            snprintf(title, sizeof title, "Approve command?");
        } else {
            snprintf(title, sizeof title, "Allow %s?", tname);
        }
        pending_approval_item *ait = calloc(1, sizeof *ait);
        if (ait) {
            ait->kind = APPROVAL_KIND_AGY_ACP;
            approval_item_set_rpc_id(ait, id_val);
            snprintf(ait->allow_opt, sizeof ait->allow_opt, "%s", allow_opt);
            snprintf(ait->allow_always_opt, sizeof ait->allow_always_opt, "%s", allow_always_opt);
            snprintf(ait->deny_opt, sizeof ait->deny_opt, "%s", deny_opt);
            ait->title = dup_or_null(title);
            ait->tool_name = dup_or_null(tname);
            ait->command = dup_or_null(detail);
            ait->reason = dup_or_null(summary ? summary : "");
            ait->details = dup_or_null(pretty_in ? pretty_in : "");
            ait->can_allow_session = (allow_always_opt[0] != '\0');
            approval_queue_push(run, ait);
        }
        free(pretty_in);
        return;
    }

    if (strcmp(method, "session/update") != 0 || !run->rpc_prompt_sent) return;
    yyjson_val *upd = yyjson_is_obj(params) ? yyjson_obj_get(params, "update") : NULL;
    if (!yyjson_is_obj(upd)) return;
    const char *su = jstr(upd, "sessionUpdate");
    if (!su) return;

    if (strcmp(su, "agent_message_chunk") == 0) {
        yyjson_val *content = yyjson_obj_get(upd, "content");
        const char *txt = jstr(content, "text");
        if (txt && txt[0]) {
            run->streamed_text_deltas = true;
            emit_simple(run, EV_REASONING_DONE, NULL);
            emit_simple(run, EV_TEXT_DELTA, txt);
            emit_simple(run, EV_STATUS, "Responding...");
        } else if (yyjson_is_obj(content)) {
            emit_json_image_content(run, content, "acp");
        }
    } else if (strcmp(su, "agent_thought_chunk") == 0) {
        yyjson_val *content = yyjson_obj_get(upd, "content");
        const char *txt = jstr(content, "text");
        if (txt && txt[0]) {
            run->streamed_thinking_deltas = true;
            emit_simple(run, EV_REASONING_DELTA, txt);
            emit_simple(run, EV_STATUS, "Thinking...");
        }
    } else if (strcmp(su, "tool_call") == 0 || strcmp(su, "tool_call_update") == 0) {
        const char *tid = jstr(upd, "toolCallId");
        const char *tname = jstr(upd, "name");
        const char *tc_title = jstr(upd, "title");
        if (!tname || !tname[0]) tname = (tc_title && tc_title[0]) ? tc_title : "tool";
        if (!tid || !tid[0]) tid = tname;
        const char *status = jstr(upd, "status");
        yyjson_val *raw_in = yyjson_obj_get(upd, "rawInput");
        if (!raw_in) raw_in = yyjson_obj_get(upd, "args");

        char detail[256] = "";
        summarize_tool_input(raw_in, detail, sizeof detail);
        if (!detail[0] && tc_title && tc_title[0] && strcmp(tc_title, tname) != 0) {
            snprintf(detail, sizeof detail, "%s", tc_title);
        }
        char *inp_json = stringify_json_val(raw_in);
        emit_tool_start(run, tid, tname, detail, inp_json ? inp_json : "");
        free(inp_json);

        bool done = status && (strcmp(status, "completed") == 0 || strcmp(status, "failed") == 0);
        bool failed = status && strcmp(status, "failed") == 0;
        if (done) {
            char *out_s = extract_acp_tool_output(upd);
            emit_tool_result(run, tid, out_s ? out_s : "", failed);
            free(out_s);
            if (!failed) {
                yyjson_val *tc_content = yyjson_obj_get(upd, "content");
                if (yyjson_is_arr(tc_content)) {
                    size_t ci, cmax;
                    yyjson_val *citem;
                    yyjson_arr_foreach(tc_content, ci, cmax, citem) {
                        if (!emit_json_image_content(run, citem, tid)) {
                            yyjson_val *inner = yyjson_is_obj(citem) ? yyjson_obj_get(citem, "content") : NULL;
                            emit_json_image_content(run, inner, tid);
                        }
                    }
                }
                /* Surface Antigravity Plan-mode implementation_plan.md artifact as a Proposed Plan card */
                const char *wpath = jstr(raw_in, "Path");
                if (!wpath) wpath = jstr(raw_in, "path");
                if (!wpath) {
                    yyjson_val *locs = yyjson_obj_get(upd, "locations");
                    if (yyjson_is_arr(locs)) {
                        yyjson_val *l0 = yyjson_arr_get_first(locs);
                        wpath = jstr(l0, "path");
                    }
                }
                if (!wpath && detail[0]) wpath = detail;
                if (wpath && strstr(wpath, "implementation_plan.md") != NULL &&
                    (strcmp(tname, "write_file") == 0 || strcmp(tname, "edit_file") == 0 ||
                     strstr(wpath, "/antigravity") != NULL)) {
                    const char *plan_txt = jstr(raw_in, "Content");
                    if (!plan_txt) plan_txt = jstr(raw_in, "content");
                    if (plan_txt && plan_txt[0]) {
                        emit_simple(run, EV_PLAN_SET, plan_txt);
                    } else {
                        FILE *pf = fopen(wpath, "rb");
                        if (pf) {
                            if (fseek(pf, 0, SEEK_END) == 0) {
                                long sz = ftell(pf);
                                if (sz > 0 && sz <= 256 * 1024 && fseek(pf, 0, SEEK_SET) == 0) {
                                    char *buf = malloc((size_t)sz + 1);
                                    if (buf) {
                                        size_t nr = fread(buf, 1, (size_t)sz, pf);
                                        buf[nr] = '\0';
                                        if (nr > 0) emit_simple(run, EV_PLAN_SET, buf);
                                        free(buf);
                                    }
                                }
                            }
                            fclose(pf);
                        }
                    }
                }
            }
        } else if (status && strcmp(status, "pending") == 0) {
            emit_simple(run, EV_STATUS, "Waiting for approval...");
        } else {
            char st[160];
            if (detail[0]) snprintf(st, sizeof st, "%s: %s", tname, detail);
            else snprintf(st, sizeof st, "Running %s...", tname);
            emit_simple(run, EV_STATUS, st);
        }
    } else if (strcmp(su, "plan") == 0) {
        yyjson_val *entries = yyjson_obj_get(upd, "entries");
        if (yyjson_is_arr(entries)) {
            mcode_plan_step steps[MCODE_MAX_PLAN_STEPS];
            int sc = 0;
            size_t ei, emax;
            yyjson_val *ev_item;
            yyjson_arr_foreach(entries, ei, emax, ev_item) {
                if (sc >= MCODE_MAX_PLAN_STEPS || !yyjson_is_obj(ev_item)) continue;
                const char *ct = jstr(ev_item, "content");
                const char *st = jstr(ev_item, "status");
                if (!ct || !ct[0]) continue;
                memset(&steps[sc], 0, sizeof steps[sc]);
                snprintf(steps[sc].title, sizeof steps[sc].title, "%s", ct);
                steps[sc].status = parse_plan_step_status(st);
                ++sc;
            }
            emit_plan_update_ev(run, "Plan", steps, sc);
        }
    } else if (strcmp(su, "current_mode_update") == 0) {
        const char *mid = jstr(upd, "currentModeId");
        if (!mid) mid = jstr(upd, "modeId");
        if (mid && mid[0]) {
            char msg[128];
            snprintf(msg, sizeof msg, "Mode changed to %s", mid);
            emit_notice_ev(run, msg, CUI_TONE_INFO);
        }
    } else if (strcmp(su, "usage_update") == 0) {
        int64_t used = ji64(upd, "used");
        int64_t size = ji64(upd, "size");
        if (used > 0 || size > 0) {
            emit_usage_ev(run, 0, 0, 0, used, size, 0.0, false);
        }
    }
}

static void handle_line(mcode_agent_run *run, const char *line, bool is_stderr) {
    if (!run || !line) return;
    /* Trim trailing \r */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    if (len == 0) return;

    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, line, len);
    copy[len] = '\0';

    if (is_stderr) {
        /* Surface meaningful stderr lines and keep a compact tail for non-zero exits */
        const char *p = copy;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p) {
            if (run->stderr_tail.len < 2048) {
                if (run->stderr_tail.len > 0) mcode_str_append_cstr(&run->stderr_tail, "\n");
                mcode_str_append_cstr(&run->stderr_tail, p);
            }
            /* Structured RPC servers report errors via JSON-RPC; avoid surfacing
             * internal tracing logs (e.g. Codex router rejection logs) as fatal turn errors. */
            if (run->proto != AGENT_PROTO_CODEX_RPC && run->proto != AGENT_PROTO_AGY_ACP) {
                if (strstr(p, "Error") || strstr(p, "error") || strstr(p, "FATAL") || strstr(p, "not found")) {
                    emit_simple(run, EV_ERROR, p);
                }
            }
        }
        free(copy);
        return;
    }

    /* Try parsing as JSON object */
    const char *trim = copy;
    while (*trim && isspace((unsigned char)*trim)) ++trim;
    if (*trim == '{') {
        yyjson_doc *doc = yyjson_read(trim, strlen(trim), 0);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (yyjson_is_obj(root)) {
                if (run->proto == AGENT_PROTO_CODEX_RPC) {
                    parse_codex_rpc_json(run, root);
                } else if (run->proto == AGENT_PROTO_AGY_ACP) {
                    parse_agy_acp_json(run, root);
                } else {
                    switch (run->agent) {
                    case APP_AGENT_CLAUDE: parse_claude_json(run, root); break;
                    case APP_AGENT_CODEX:  parse_codex_json(run, root); break;
                    case APP_AGENT_AGY:    parse_agy_json(run, root); break;
                    default: break;
                    }
                }
            }
            yyjson_doc_free(doc);
            free(copy);
            return;
        }
    }

    /* Ignore non-JSON chatter on stdout for structured JSON-RPC servers */
    if (run->proto == AGENT_PROTO_CODEX_RPC || run->proto == AGENT_PROTO_AGY_ACP) {
        free(copy);
        return;
    }

    /* Plain text fallback */
    emit_simple(run, EV_TEXT_DELTA, copy);
    emit_simple(run, EV_TEXT_DELTA, "\n");
    free(copy);
}

static void on_proc_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    mcode_agent_run *run = (mcode_agent_run *)user;
    mcode_str *buf = is_stderr ? &run->err_line : &run->out_line;

    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(data + pos, '\n', len - pos);
        if (!nl) {
            mcode_str_append(buf, data + pos, len - pos);
            break;
        }
        size_t chunk = (size_t)(nl - (data + pos));
        if (chunk > 0) mcode_str_append(buf, data + pos, chunk);
        handle_line(run, mcode_str_cstr(buf), is_stderr);
        mcode_str_clear(buf);
        pos += chunk + 1;
    }
}

static void on_proc_exit(int exit_code, bool killed, void *user) {
    mcode_agent_run *run = (mcode_agent_run *)user;
    if (run->out_line.len > 0) {
        handle_line(run, mcode_str_cstr(&run->out_line), false);
        mcode_str_clear(&run->out_line);
    }
    if (run->err_line.len > 0) {
        handle_line(run, mcode_str_cstr(&run->err_line), true);
        mcode_str_clear(&run->err_line);
    }
    flush_unsent_live_steers(run);
    pthread_mutex_lock(&run->mu);
    bool clean_rpc_done = run->rpc_turn_done;
    bool steer_int = run->steer_interrupting;
    pthread_mutex_unlock(&run->mu);
    if (clean_rpc_done || steer_int) {
        exit_code = 0;
        killed = false;
    }

    agent_ev *ev = calloc(1, sizeof *ev);
    if (ev) {
        ev->kind = EV_DONE;
        ev->int_a = exit_code;
        ev->flag = killed;
        ev->flag_b = steer_int;
        if (exit_code != 0 && !killed && !steer_int && run->stderr_tail.len > 0) {
            ev->text = dup_or_null(mcode_str_cstr(&run->stderr_tail));
        }
        push_ev(run, ev);
    }
    pthread_mutex_lock(&run->mu);
    run->finished = true;
    pthread_mutex_unlock(&run->mu);
}

static void merge_provider_quota(mcode_provider_quota *dst, const mcode_provider_quota *src) {
    if (!dst || !src) return;
    if (src->auth_checked) {
        dst->auth_checked = true;
        dst->logged_in = src->logged_in;
        if (src->auth_method[0]) {
            snprintf(dst->auth_method, sizeof dst->auth_method, "%s", src->auth_method);
        }
    }
    if (src->plan[0]) snprintf(dst->plan, sizeof dst->plan, "%s", src->plan);
    if (src->primary.available) dst->primary = src->primary;
    if (src->secondary.available) dst->secondary = src->secondary;
    if (src->group_count > 0) {
        int n = src->group_count;
        if (n > MCODE_MAX_QUOTA_GROUPS) n = MCODE_MAX_QUOTA_GROUPS;
        memcpy(dst->groups, src->groups, (size_t)n * sizeof dst->groups[0]);
        dst->group_count = n;
    }
    if (src->reset_credits_checked) {
        dst->reset_credits_checked = true;
        dst->reset_credits_available = src->reset_credits_available;
        int n = src->reset_credit_count;
        if (n < 0) n = 0;
        if (n > MCODE_MAX_RESET_CREDITS) n = MCODE_MAX_RESET_CREDITS;
        if (n > 0) {
            memcpy(dst->reset_credits, src->reset_credits, (size_t)n * sizeof dst->reset_credits[0]);
        }
        dst->reset_credit_count = n;
    }
    if (src->has_credits || src->unlimited_credits || src->credits_balance[0]) {
        dst->has_credits = src->has_credits;
        dst->unlimited_credits = src->unlimited_credits;
        if (src->credits_balance[0]) {
            snprintf(dst->credits_balance, sizeof dst->credits_balance, "%s", src->credits_balance);
        }
    }
    if (src->account_today_tokens > 0) dst->account_today_tokens = src->account_today_tokens;
    if (src->account_lifetime_tokens > 0) dst->account_lifetime_tokens = src->account_lifetime_tokens;
    if (src->account_cost_usd > 0.0) dst->account_cost_usd = src->account_cost_usd;
    if (src->current_streak_days > 0) dst->current_streak_days = src->current_streak_days;
    if (src->available || dst->primary.available || dst->secondary.available || dst->group_count > 0) {
        dst->available = true;
        dst->error[0] = '\0';
        if (src->updated_mono > 0.0) dst->updated_mono = src->updated_mono;
        if (src->updated_epoch > 0) dst->updated_epoch = src->updated_epoch;
    }
}

static void apply_event(mcode_thread *thread, mcode_turn *turn,
                        mcode_usage_tracker *usage, const agent_ev *ev) {
    if (!thread || !turn || !ev) return;

    switch (ev->kind) {
    case EV_SESSION:
        if (ev->text && ev->text[0]) {
            snprintf(thread->session_id, sizeof thread->session_id, "%s", ev->text);
        }
        break;
    case EV_TEXT_DELTA: {
        if (!ev->text || !ev->text[0]) break;
        mcode_block *b = (turn->block_count > 0 &&
                          turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_TEXT)
            ? &turn->blocks[turn->block_count - 1]
            : mcode_turn_add_block(turn, MCODE_BLOCK_TEXT);
        if (b) mcode_str_append_cstr(&b->text, ev->text);
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_TEXT_FALLBACK: {
        if (!ev->text || !ev->text[0]) break;
        bool has_text = false;
        for (int i = 0; i < turn->block_count; ++i) {
            if (turn->blocks[i].kind == MCODE_BLOCK_TEXT && turn->blocks[i].text.len > 0) {
                has_text = true;
                break;
            }
        }
        if (!has_text) {
            mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_TEXT);
            if (b) mcode_str_set(&b->text, ev->text);
            thread->scroll_to_bottom_frames = 2;
        }
        break;
    }
    case EV_REASONING_DELTA: {
        if (!ev->text || !ev->text[0]) break;
        mcode_block *b = (turn->block_count > 0 &&
                          turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_REASONING &&
                          turn->blocks[turn->block_count - 1].thinking)
            ? &turn->blocks[turn->block_count - 1]
            : mcode_turn_add_block(turn, MCODE_BLOCK_REASONING);
        if (b) {
            b->thinking = true;
            mcode_str_append_cstr(&b->text, ev->text);
            b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
        }
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_REASONING_DONE: {
        for (int i = turn->block_count - 1; i >= 0; --i) {
            if (turn->blocks[i].kind == MCODE_BLOCK_REASONING && turn->blocks[i].thinking) {
                turn->blocks[i].thinking = false;
                turn->blocks[i].duration_sec = (float)(mcode_now_mono() - turn->blocks[i].started_mono);
            }
        }
        break;
    }
    case EV_TOOL_START: {
        mcode_block *b = (ev->tool_id && ev->tool_id[0]) ? mcode_turn_find_tool(turn, ev->tool_id) : NULL;
        if (!b) {
            b = mcode_turn_add_block(turn, MCODE_BLOCK_TOOL);
            if (!b) break;
            b->status = CUI_TASK_RUNNING;
        }
        if (ev->tool_id && ev->tool_id[0]) snprintf(b->tool_id, sizeof b->tool_id, "%s", ev->tool_id);
        if (ev->tool_name && ev->tool_name[0]) snprintf(b->tool_name, sizeof b->tool_name, "%s", ev->tool_name);
        if (ev->tool_detail && ev->tool_detail[0]) snprintf(b->tool_detail, sizeof b->tool_detail, "%s", ev->tool_detail);
        if (ev->text && ev->text[0]) mcode_str_set(&b->input, ev->text);
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_TOOL_INPUT_DELTA: {
        mcode_block *b = (ev->tool_id && ev->tool_id[0]) ? mcode_turn_find_tool(turn, ev->tool_id) : NULL;
        if (b && ev->text) mcode_str_append_cstr(&b->input, ev->text);
        break;
    }
    case EV_TOOL_OUTPUT_DELTA: {
        mcode_block *b = (ev->tool_id && ev->tool_id[0]) ? mcode_turn_find_tool(turn, ev->tool_id) : NULL;
        if (b && ev->text && ev->text[0]) {
            /* Cap live streaming tool output at 256 KB to keep UI responsive */
            if (b->output.len < 256 * 1024) {
                mcode_str_append_cstr(&b->output, ev->text);
            }
        }
        break;
    }
    case EV_TOOL_RESULT: {
        mcode_block *b = (ev->tool_id && ev->tool_id[0]) ? mcode_turn_find_tool(turn, ev->tool_id) : NULL;
        if (!b) {
            /* Match most recent running tool if no ID */
            for (int i = turn->block_count - 1; i >= 0; --i) {
                if (turn->blocks[i].kind == MCODE_BLOCK_TOOL && turn->blocks[i].status == CUI_TASK_RUNNING) {
                    b = &turn->blocks[i];
                    break;
                }
            }
        }
        if (b) {
            bool failed = ev->flag;
            if (failed && mcode_tool_is_benign_probe_failure(b->tool_name,
                                                             mcode_str_cstr(&b->input),
                                                             ev->text ? ev->text : "",
                                                             ev->int_a)) {
                failed = false;
            }
            b->status = failed ? CUI_TASK_FAILED : CUI_TASK_DONE;
            b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            if (ev->text) mcode_str_set(&b->output, ev->text);
        }
        break;
    }
    case EV_IMAGE: {
        if (!ev->extra_a || !ev->extra_a[0]) break;
        if (turn->block_count > 0 &&
            turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_IMAGE &&
            strcmp(turn->blocks[turn->block_count - 1].image_path, ev->extra_a) == 0) {
            break;
        }
        mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_IMAGE);
        if (b) {
            snprintf(b->image_path, sizeof b->image_path, "%s", ev->extra_a);
            if (ev->text && ev->text[0]) mcode_str_set(&b->text, ev->text);
        }
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_PLAN_UPDATE: {
        free(turn->plan_steps);
        turn->plan_steps = NULL;
        turn->plan_step_count = 0;
        snprintf(turn->plan_title, sizeof turn->plan_title, "%s",
                 (ev->text && ev->text[0]) ? ev->text : "Plan");
        if (ev->int_a > 0 && ev->plan_steps) {
            int cnt = ev->int_a > MCODE_MAX_PLAN_STEPS ? MCODE_MAX_PLAN_STEPS : ev->int_a;
            turn->plan_steps = calloc((size_t)cnt, sizeof *turn->plan_steps);
            if (turn->plan_steps) {
                memcpy(turn->plan_steps, ev->plan_steps, (size_t)cnt * sizeof *turn->plan_steps);
                turn->plan_step_count = cnt;
            }
        }
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_PLAN_DELTA: {
        if (!ev->text || !ev->text[0]) break;
        mcode_block *b = (turn->block_count > 0 &&
                          turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_PLAN)
            ? &turn->blocks[turn->block_count - 1]
            : mcode_turn_add_block(turn, MCODE_BLOCK_PLAN);
        if (b) mcode_str_append_cstr(&b->text, ev->text);
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_PLAN_SET: {
        if (!ev->text || !ev->text[0]) break;
        mcode_block *b = (turn->block_count > 0 &&
                          turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_PLAN)
            ? &turn->blocks[turn->block_count - 1]
            : mcode_turn_add_block(turn, MCODE_BLOCK_PLAN);
        if (b) mcode_str_set(&b->text, ev->text);
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_NOTICE: {
        if (!ev->text || !ev->text[0]) break;
        if (turn->block_count > 0 &&
            turn->blocks[turn->block_count - 1].kind == MCODE_BLOCK_NOTICE &&
            strcmp(mcode_str_cstr(&turn->blocks[turn->block_count - 1].text), ev->text) == 0) {
            break;
        }
        mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_NOTICE);
        if (b) {
            mcode_str_set(&b->text, ev->text);
            b->notice_tone = (cui_tone)ev->int_a;
        }
        thread->scroll_to_bottom_frames = 2;
        break;
    }
    case EV_APPROVAL_REQUEST: {
        mcode_pending_approval *pa = &thread->pending_approval;
        memset(pa, 0, sizeof *pa);
        pa->active = true;
        if (ev->text) snprintf(pa->title, sizeof pa->title, "%s", ev->text);
        if (ev->tool_name) snprintf(pa->tool_name, sizeof pa->tool_name, "%s", ev->tool_name);
        if (ev->tool_detail) snprintf(pa->command, sizeof pa->command, "%s", ev->tool_detail);
        if (ev->extra_a) snprintf(pa->reason, sizeof pa->reason, "%s", ev->extra_a);
        if (ev->extra_b) snprintf(pa->details, sizeof pa->details, "%s", ev->extra_b);
        pa->can_allow_session = ev->flag;
        thread->scroll_to_bottom_frames = 3;
        break;
    }
    case EV_APPROVAL_CLEAR:
        memset(&thread->pending_approval, 0, sizeof thread->pending_approval);
        break;
    case EV_QUESTION_REQUEST:
        if (ev->question) {
            thread->pending_question = *ev->question;
            thread->pending_question.active = true;
            thread->scroll_to_bottom_frames = 3;
        }
        break;
    case EV_QUESTION_CLEAR:
        memset(&thread->pending_question, 0, sizeof thread->pending_question);
        break;
    case EV_STATUS:
        if (ev->text && ev->text[0]) {
            snprintf(thread->status_text, sizeof thread->status_text, "%s", ev->text);
        }
        break;
    case EV_USAGE:
        if (ev->flag) {
            if (ev->tok_in > 0) turn->input_tokens = ev->tok_in;
            if (ev->tok_out > 0) turn->output_tokens = ev->tok_out;
            if (ev->tok_cached > 0) turn->cached_tokens = ev->tok_cached;
        } else {
            turn->input_tokens += ev->tok_in;
            turn->output_tokens += ev->tok_out;
            turn->cached_tokens += ev->tok_cached;
        }
        if (ev->tok_ctx > 0) turn->context_tokens = ev->tok_ctx;
        if (ev->tok_ctx_win > 0) turn->context_window = ev->tok_ctx_win;
        if (ev->dbl_a > 0.0) turn->cost_usd = ev->dbl_a;
        break;
    case EV_RATE_LIMIT:
        if (usage && ev->quota && thread->agent >= 0 && thread->agent < APP_AGENT_COUNT) {
            merge_provider_quota(&usage->quotas[thread->agent], ev->quota);
        }
        break;
    case EV_STEER_REJECTED:
        if (ev->text && ev->text[0]) {
            /* Remove optimistic inline steer block if it is still the trailing block */
            if (turn->block_count > 0) {
                mcode_block *last = &turn->blocks[turn->block_count - 1];
                if (last->kind == MCODE_BLOCK_STEER &&
                    strcmp(mcode_str_cstr(&last->text), ev->text) == 0) {
                    mcode_str_free(&last->text);
                    mcode_str_free(&last->input);
                    mcode_str_free(&last->output);
                    turn->block_count--;
                }
            }
            if (thread->steer_pending && thread->pending_steer.len > 0) {
                mcode_str_append_cstr(&thread->pending_steer, "\n\n");
                mcode_str_append_cstr(&thread->pending_steer, ev->text);
            } else {
                mcode_str_set(&thread->pending_steer, ev->text);
                thread->steer_pending = true;
            }
            snprintf(thread->status_text, sizeof thread->status_text, "Steer queued next...");
        }
        break;
    case EV_ERROR: {
        if (ev->text && ev->text[0]) {
            const char *new_norm = ev->text;
            while (*new_norm == ' ' || *new_norm == '\t') ++new_norm;
            bool new_has_prefix = false;
            if ((strncmp(new_norm, "error:", 6) == 0 || strncmp(new_norm, "Error:", 6) == 0)) {
                new_has_prefix = true;
                new_norm += 6;
                while (*new_norm == ' ' || *new_norm == '\t') ++new_norm;
            }
            size_t new_nlen = strlen(new_norm);

            /* Deduplicate when both stderr ("error: ...") and stdout JSON report the same error. */
            mcode_block *dup = NULL;
            for (int i = turn->block_count - 1; i >= 0; --i) {
                if (turn->blocks[i].kind != MCODE_BLOCK_ERROR) continue;
                const char *old_s = mcode_str_cstr(&turn->blocks[i].text);
                const char *old_norm = old_s;
                while (*old_norm == ' ' || *old_norm == '\t') ++old_norm;
                bool old_has_prefix = false;
                if ((strncmp(old_norm, "error:", 6) == 0 || strncmp(old_norm, "Error:", 6) == 0)) {
                    old_has_prefix = true;
                    old_norm += 6;
                    while (*old_norm == ' ' || *old_norm == '\t') ++old_norm;
                }
                size_t old_nlen = strlen(old_norm);
                if (old_nlen > 0 && new_nlen > 0 &&
                    (strcmp(old_norm, new_norm) == 0 ||
                     (new_nlen > old_nlen && strncmp(new_norm, old_norm, old_nlen) == 0) ||
                     (old_nlen > new_nlen && strncmp(old_norm, new_norm, new_nlen) == 0))) {
                    dup = &turn->blocks[i];
                    if (new_nlen > old_nlen || (new_nlen == old_nlen && old_has_prefix && !new_has_prefix)) {
                        mcode_str_set(&dup->text, ev->text);
                    }
                }
                break;
            }
            if (!dup) {
                mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_ERROR);
                if (b) mcode_str_set(&b->text, ev->text);
            }
            turn->failed = true;
            thread->scroll_to_bottom_frames = 2;
        }
        break;
    }
    case EV_DONE: {
        turn->running = false;
        memset(&thread->pending_approval, 0, sizeof thread->pending_approval);
        memset(&thread->pending_question, 0, sizeof thread->pending_question);
        turn->duration_sec = (float)(mcode_now_mono() - turn->started_mono);
        /* Finalize any still-running blocks */
        for (int i = 0; i < turn->block_count; ++i) {
            mcode_block *b = &turn->blocks[i];
            if (b->kind == MCODE_BLOCK_REASONING && b->thinking) {
                b->thinking = false;
                b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            } else if (b->kind == MCODE_BLOCK_TOOL && b->status == CUI_TASK_RUNNING) {
                if (ev->flag_b) {
                    b->status = CUI_TASK_SKIPPED;
                } else {
                    b->status = (ev->flag || ev->int_a != 0) ? CUI_TASK_FAILED : CUI_TASK_DONE;
                }
                b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            }
        }
        if (ev->flag_b) {
            snprintf(thread->status_text, sizeof thread->status_text, "Steered");
        } else if (ev->flag) {
            snprintf(thread->status_text, sizeof thread->status_text, "Stopped");
        } else if (ev->int_a != 0) {
            turn->failed = true;
            snprintf(thread->status_text, sizeof thread->status_text, "Exited (%d)", ev->int_a);
            bool has_err = false;
            for (int i = 0; i < turn->block_count; ++i) {
                if (turn->blocks[i].kind == MCODE_BLOCK_ERROR) { has_err = true; break; }
            }
            if (!has_err) {
                char msg[256];
                if (ev->text && ev->text[0]) {
                    snprintf(msg, sizeof msg, "%s", ev->text);
                } else {
                    snprintf(msg, sizeof msg, "Agent exited with code %d.", ev->int_a);
                }
                mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_ERROR);
                if (b) mcode_str_set(&b->text, msg);
            }
        } else {
            snprintf(thread->status_text, sizeof thread->status_text, "Done");
        }
        thread->updated_at = mcode_now_epoch();
        break;
    }
    }
}

void mcode_agent_detect_clis(const app_settings *settings, mcode_cli_status *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    const char *names[APP_AGENT_COUNT] = {
        (settings && settings->mcode.claude_bin[0]) ? settings->mcode.claude_bin : "claude",
        (settings && settings->mcode.codex_bin[0])  ? settings->mcode.codex_bin  : "codex",
        (settings && settings->mcode.agy_bin[0])    ? settings->mcode.agy_bin    : "agy",
    };
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        out->available[i] = mcode_find_executable(names[i], out->resolved_path[i],
                                                  sizeof out->resolved_path[i]);
    }
}

/* ---- Model catalog & dynamic CLI model discovery ---- */

struct mcode_model_fetch {
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str stdout_buf;
    bool finished;
    int exit_code;
};

static void set_model_opt(mcode_model_option *opt, const char *id, const char *label, const char *detail) {
    memset(opt, 0, sizeof *opt);
    snprintf(opt->id, sizeof opt->id, "%s", id ? id : "");
    snprintf(opt->label, sizeof opt->label, "%s", (label && label[0]) ? label : (id ? id : ""));
    snprintf(opt->detail, sizeof opt->detail, "%s", detail ? detail : "");
}

static void load_claude_default_models(mcode_model_catalog *cat) {
    static const struct { const char *id, *label, *detail; } k_claude[] = {
        { "opus",   "Claude Opus",   "Most capable model for complex reasoning and architecture" },
        { "sonnet", "Claude Sonnet", "Balanced everyday coding and implementation model" },
        { "haiku",  "Claude Haiku",  "Fast, lightweight model for quick edits and questions" },
        { "fable",  "Claude Fable",  "Latest Claude Fable alias" },
    };
    static const char *const k_claude_efforts[] = { "low", "medium", "high", "xhigh", "max" };
    int n = 0;
    for (size_t i = 0; i < sizeof k_claude / sizeof k_claude[0] && n < MCODE_MAX_MODELS_PER_AGENT; ++i) {
        mcode_model_option *opt = &cat->items[APP_AGENT_CLAUDE][n++];
        set_model_opt(opt, k_claude[i].id, k_claude[i].label, k_claude[i].detail);
        opt->context_window = 200000;
        for (size_t j = 0; j < sizeof k_claude_efforts / sizeof k_claude_efforts[0]; ++j) {
            snprintf(opt->efforts[opt->effort_count++], sizeof opt->efforts[0], "%s", k_claude_efforts[j]);
        }
    }
    cat->count[APP_AGENT_CLAUDE] = n;
}

static void load_codex_fallback_models(mcode_model_catalog *cat) {
    static const struct { const char *id, *label, *detail; } k_codex[] = {
        { "gpt-5.4",       "GPT-5.4",       "Frontier coding and reasoning model" },
        { "gpt-5.3-codex", "GPT-5.3-Codex", "Agentic coding model" },
        { "o3",            "o3",            "Deep reasoning model" },
        { "o4-mini",       "o4-mini",       "Fast, affordable reasoning model" },
    };
    static const char *const k_codex_efforts[] = { "low", "medium", "high", "xhigh" };
    int n = 0;
    for (size_t i = 0; i < sizeof k_codex / sizeof k_codex[0] && n < MCODE_MAX_MODELS_PER_AGENT; ++i) {
        mcode_model_option *opt = &cat->items[APP_AGENT_CODEX][n++];
        set_model_opt(opt, k_codex[i].id, k_codex[i].label, k_codex[i].detail);
        opt->context_window = 272000;
        snprintf(opt->default_effort, sizeof opt->default_effort, "medium");
        for (size_t j = 0; j < sizeof k_codex_efforts / sizeof k_codex_efforts[0]; ++j) {
            snprintf(opt->efforts[opt->effort_count++], sizeof opt->efforts[0], "%s", k_codex_efforts[j]);
        }
    }
    cat->count[APP_AGENT_CODEX] = n;
}

static void load_agy_fallback_models(mcode_model_catalog *cat) {
    static const struct { const char *id, *label, *detail; } k_agy[] = {
        { "gemini-4-neon-low",         "Gemini 4 Neon (Low)",         "gemini-4-neon-low" },
        { "gemini-4-neon-medium",      "Gemini 4 Neon (Medium)",      "gemini-4-neon-medium" },
        { "gemini-4-neon-high",        "Gemini 4 Neon (High)",        "gemini-4-neon-high" },
        { "gemini-4-neon-xhigh",       "Gemini 4 Neon (XHigh)",       "gemini-4-neon-xhigh" },
        { "gemini-4-carbon-low",       "Gemini 4 Carbon (Low)",       "gemini-4-carbon-low" },
        { "gemini-4-carbon-medium",    "Gemini 4 Carbon (Medium)",    "gemini-4-carbon-medium" },
        { "gemini-4-carbon-high",      "Gemini 4 Carbon (High)",      "gemini-4-carbon-high" },
        { "gemini-4-argon-low",        "Gemini 4 Argon (Low)",        "gemini-4-argon-low" },
        { "gemini-4-argon-medium",     "Gemini 4 Argon (Medium)",     "gemini-4-argon-medium" },
        { "gemini-4-argon-high",       "Gemini 4 Argon (High)",       "gemini-4-argon-high" },
        { "gemini-4-hydrogen-low",     "Gemini 4 Hydrogen (Low)",     "gemini-4-hydrogen-low" },
        { "gemini-4-hydrogen-medium",  "Gemini 4 Hydrogen (Medium)",  "gemini-4-hydrogen-medium" },
        { "gemini-4-hydrogen-high",    "Gemini 4 Hydrogen (High)",    "gemini-4-hydrogen-high" },
        { "gemini-4-hydrogen-xhigh",   "Gemini 4 Hydrogen (XHigh)",   "gemini-4-hydrogen-xhigh" },
        { "gemini-3.8-flash-low",      "Gemini 3.8 Flash (Low)",      "gemini-3.8-flash-low" },
        { "gemini-3.8-flash-medium",   "Gemini 3.8 Flash (Medium)",   "gemini-3.8-flash-medium" },
        { "gemini-3.8-flash-high",     "Gemini 3.8 Flash (High)",     "gemini-3.8-flash-high" },
        { "claude-opus-5-5-low",       "Claude Opus 5.5 (Low)",       "claude-opus-5-5-low" },
        { "claude-opus-5-5-medium",    "Claude Opus 5.5 (Medium)",    "claude-opus-5-5-medium" },
        { "claude-opus-5-5-high",      "Claude Opus 5.5 (High)",      "claude-opus-5-5-high" },
        { "claude-sonnet-5-5-low",     "Claude Sonnet 5.5 (Low)",     "claude-sonnet-5-5-low" },
        { "claude-sonnet-5-5-medium",  "Claude Sonnet 5.5 (Medium)",  "claude-sonnet-5-5-medium" },
        { "claude-sonnet-5-5-high",    "Claude Sonnet 5.5 (High)",    "claude-sonnet-5-5-high" },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof k_agy / sizeof k_agy[0] && n < MCODE_MAX_MODELS_PER_AGENT; ++i) {
        set_model_opt(&cat->items[APP_AGENT_AGY][n++],
                      k_agy[i].id, k_agy[i].label, k_agy[i].detail);
    }
    cat->count[APP_AGENT_AGY] = n;
}

int mcode_agent_parse_codex_models_json(const char *json, size_t len,
                                        mcode_model_option *out, int max_out) {
    if (!json || len == 0 || !out || max_out <= 0) return 0;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) return 0;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *models = yyjson_is_obj(root) ? yyjson_obj_get(root, "models") : NULL;
    int count = 0;
    if (yyjson_is_arr(models)) {
        size_t idx, max;
        yyjson_val *m;
        yyjson_arr_foreach(models, idx, max, m) {
            if (count >= max_out) break;
            if (!yyjson_is_obj(m)) continue;
            const char *slug = jstr(m, "slug");
            if (!slug || !slug[0]) continue;
            const char *vis = jstr(m, "visibility");
            if (vis && strcmp(vis, "hide") == 0) continue;
            const char *disp = jstr(m, "display_name");
            const char *desc = jstr(m, "description");
            mcode_model_option *opt = &out[count++];
            set_model_opt(opt, slug, (disp && disp[0]) ? disp : slug, desc ? desc : "");
            int64_t cw = ji64(m, "context_window");
            if (cw <= 0) cw = ji64(m, "max_context_window");
            opt->context_window = cw;
            const char *def_eff = jstr(m, "default_reasoning_level");
            if (def_eff && def_eff[0]) {
                snprintf(opt->default_effort, sizeof opt->default_effort, "%s", def_eff);
            }
            yyjson_val *levels = yyjson_obj_get(m, "supported_reasoning_levels");
            if (yyjson_is_arr(levels)) {
                size_t li, lmax;
                yyjson_val *lv;
                yyjson_arr_foreach(levels, li, lmax, lv) {
                    if (opt->effort_count >= MCODE_MAX_EFFORTS_PER_MODEL) break;
                    const char *eff = jstr(lv, "effort");
                    if (eff && eff[0]) {
                        snprintf(opt->efforts[opt->effort_count++], sizeof opt->efforts[0], "%s", eff);
                    }
                }
            }
            yyjson_val *speed_tiers = yyjson_obj_get(m, "additional_speed_tiers");
            if (yyjson_is_arr(speed_tiers)) {
                size_t si, smax;
                yyjson_val *sv;
                yyjson_arr_foreach(speed_tiers, si, smax, sv) {
                    if (yyjson_is_str(sv) && strcmp(yyjson_get_str(sv), "fast") == 0) {
                        opt->supports_fast = true;
                        break;
                    }
                }
            }
            yyjson_val *svc_tiers = yyjson_obj_get(m, "service_tiers");
            if (yyjson_is_arr(svc_tiers)) {
                size_t si, smax;
                yyjson_val *sv;
                yyjson_arr_foreach(svc_tiers, si, smax, sv) {
                    const char *tid = jstr(sv, "id");
                    const char *tname = jstr(sv, "name");
                    if ((tid && (strcmp(tid, "priority") == 0 || strcmp(tid, "fast") == 0)) ||
                        (tname && strcmp(tname, "Fast") == 0)) {
                        opt->supports_fast = true;
                        const char *tdesc = jstr(sv, "description");
                        if (tdesc && tdesc[0]) {
                            snprintf(opt->fast_desc, sizeof opt->fast_desc, "%s", tdesc);
                        }
                        break;
                    }
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return count;
}

static bool is_model_slug_char(unsigned char ch) {
    return isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':' || ch == '/';
}

static void strip_ansi_into(const char *src, size_t len, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < cap; ++i) {
        if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len && !((src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= 'a' && src[i] <= 'z'))) {
                ++i;
            }
            continue;
        }
        dst[w++] = src[i];
    }
    dst[w] = '\0';
}

int mcode_agent_parse_agy_models_text(const char *text,
                                      mcode_model_option *out, int max_out) {
    if (!text || !text[0] || !out || max_out <= 0) return 0;
    int count = 0;
    const char *p = text;
    while (*p && count < max_out) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        const char *seg = p;
        size_t seg_len = line_len;
        /* Spinner output precedes the first model line using carriage returns ('\r'). */
        for (size_t i = 0; i < line_len; ++i) {
            if (p[i] == '\r') {
                seg = p + i + 1;
                seg_len = line_len - (i + 1);
            }
        }
        char clean[512];
        strip_ansi_into(seg, seg_len, clean, sizeof clean);

        char *s = clean;
        while (*s && isspace((unsigned char)*s)) ++s;
        size_t clen = strlen(s);
        while (clen > 0 && isspace((unsigned char)s[clen - 1])) s[--clen] = '\0';

        if (clen > 0 && isalnum((unsigned char)s[0])) {
            char *sp = s;
            while (*sp && !isspace((unsigned char)*sp)) ++sp;
            size_t id_len = (size_t)(sp - s);
            bool valid_id = (id_len >= 2 && id_len < APP_MODEL_NAME_MAX);
            for (size_t i = 0; valid_id && i < id_len; ++i) {
                if (!is_model_slug_char((unsigned char)s[i])) valid_id = false;
            }
            /* Skip prose lines such as "Fetching available models..." or "Error: ...".
             * Piped `agy models` separates ID and label with a single '\t'; TTY uses 2+ spaces. */
            if (valid_id && memchr(s, '-', id_len) != NULL && *sp != '\0') {
                bool has_tab = (*sp == '\t');
                char *lbl = sp;
                while (*lbl && isspace((unsigned char)*lbl)) ++lbl;
                if (*lbl && (has_tab || (lbl - sp) >= 2)) {
                    char id_buf[APP_MODEL_NAME_MAX];
                    snprintf(id_buf, sizeof id_buf, "%.*s", (int)id_len, s);
                    set_model_opt(&out[count++], id_buf, lbl, id_buf);
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return count;
}

static void parse_toml_string_val(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!raw) return;
    while (*raw && (*raw == ' ' || *raw == '\t')) ++raw;
    if (*raw == '"' || *raw == '\'') {
        char q = *raw++;
        size_t w = 0;
        while (*raw && *raw != q && *raw != '\r' && *raw != '\n') {
            if (q == '"' && *raw == '\\' && raw[1]) ++raw;
            if (w + 1 < cap) out[w++] = *raw;
            ++raw;
        }
        out[w] = '\0';
    } else {
        size_t w = 0;
        while (*raw && *raw != '#' && *raw != '\r' && *raw != '\n') {
            if (w + 1 < cap) out[w++] = *raw;
            ++raw;
        }
        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) --w;
        out[w] = '\0';
    }
}

void mcode_agent_parse_codex_config_toml(const char *toml,
                                         char *model_out, size_t model_cap,
                                         char *effort_out, size_t effort_cap,
                                         bool *fast_out) {
    if (model_out && model_cap > 0) model_out[0] = '\0';
    if (effort_out && effort_cap > 0) effort_out[0] = '\0';
    if (fast_out) *fast_out = false;
    if (!toml || !toml[0]) return;

    const char *p = toml;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) --len;
        const char *s = p;
        while (len > 0 && (*s == ' ' || *s == '\t')) { ++s; --len; }

        if (len > 0 && s[0] == '[') {
            /* Only inspect top-level keys before the first [section]. */
            break;
        }
        if (len > 0 && s[0] != '#') {
            const char *eq = memchr(s, '=', len);
            if (eq) {
                size_t klen = (size_t)(eq - s);
                while (klen > 0 && (s[klen - 1] == ' ' || s[klen - 1] == '\t')) --klen;
                char val_line[256];
                size_t vlen = len - (size_t)((eq + 1) - s);
                if (vlen >= sizeof val_line) vlen = sizeof val_line - 1;
                memcpy(val_line, eq + 1, vlen);
                val_line[vlen] = '\0';

                if (klen == 5 && memcmp(s, "model", 5) == 0 && model_out && model_cap > 0) {
                    parse_toml_string_val(val_line, model_out, model_cap);
                } else if (klen == 22 && memcmp(s, "model_reasoning_effort", 22) == 0 &&
                           effort_out && effort_cap > 0) {
                    parse_toml_string_val(val_line, effort_out, effort_cap);
                } else if (klen == 12 && memcmp(s, "service_tier", 12) == 0 && fast_out) {
                    char tier[32] = "";
                    parse_toml_string_val(val_line, tier, sizeof tier);
                    *fast_out = (strcmp(tier, "fast") == 0 || strcmp(tier, "priority") == 0);
                }
            }
        }

        if (!nl) break;
        p = nl + 1;
    }
}

void mcode_agent_parse_claude_settings_json(const char *json, size_t len,
                                            char *model_out, size_t model_cap,
                                            char *effort_out, size_t effort_cap) {
    if (model_out && model_cap > 0) model_out[0] = '\0';
    if (effort_out && effort_cap > 0) effort_out[0] = '\0';
    if (!json || len == 0) return;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        const char *m = jstr(root, "model");
        const char *e = jstr(root, "effortLevel");
        if (m && m[0] && model_out && model_cap > 0) snprintf(model_out, model_cap, "%s", m);
        if (e && e[0] && effort_out && effort_cap > 0) snprintf(effort_out, effort_cap, "%s", e);
    }
    yyjson_doc_free(doc);
}

static char *read_home_file(const char *rel_path, size_t *out_len) {
    if (out_len) *out_len = 0;
    const char *home = getenv("HOME");
    if (!home || !home[0] || !rel_path || !rel_path[0]) return NULL;
    char path[MCODE_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", home, rel_path);
    if (n <= 0 || (size_t)n >= sizeof path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static void load_cli_config_defaults(mcode_model_catalog *cat) {
    if (!cat) return;
    size_t len = 0;
    char *toml = read_home_file(".codex/config.toml", &len);
    if (toml) {
        mcode_agent_parse_codex_config_toml(toml,
            cat->cli_default_model[APP_AGENT_CODEX], sizeof cat->cli_default_model[APP_AGENT_CODEX],
            cat->cli_default_effort[APP_AGENT_CODEX], sizeof cat->cli_default_effort[APP_AGENT_CODEX],
            &cat->cli_default_fast[APP_AGENT_CODEX]);
        free(toml);
    }
    char *claude_json = read_home_file(".claude/settings.json", &len);
    if (claude_json) {
        mcode_agent_parse_claude_settings_json(claude_json, len,
            cat->cli_default_model[APP_AGENT_CLAUDE], sizeof cat->cli_default_model[APP_AGENT_CLAUDE],
            cat->cli_default_effort[APP_AGENT_CLAUDE], sizeof cat->cli_default_effort[APP_AGENT_CLAUDE]);
        free(claude_json);
    }
}

static bool load_codex_models_from_cache(mcode_model_catalog *cat) {
    size_t got = 0;
    char *buf = read_home_file(".codex/models_cache.json", &got);
    if (!buf) return false;

    mcode_model_option parsed[MCODE_MAX_MODELS_PER_AGENT];
    int cnt = mcode_agent_parse_codex_models_json(buf, got, parsed, MCODE_MAX_MODELS_PER_AGENT);
    free(buf);
    if (cnt <= 0) return false;
    memcpy(cat->items[APP_AGENT_CODEX], parsed, (size_t)cnt * sizeof parsed[0]);
    cat->count[APP_AGENT_CODEX] = cnt;
    return true;
}

static void on_agy_models_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    if (is_stderr) return;
    mcode_model_fetch *fetch = (mcode_model_fetch *)user;
    pthread_mutex_lock(&fetch->mu);
    if (fetch->stdout_buf.len < 256 * 1024) {
        mcode_str_append(&fetch->stdout_buf, data, len);
    }
    pthread_mutex_unlock(&fetch->mu);
}

static void on_agy_models_exit(int exit_code, bool killed, void *user) {
    (void)killed;
    mcode_model_fetch *fetch = (mcode_model_fetch *)user;
    pthread_mutex_lock(&fetch->mu);
    fetch->exit_code = exit_code;
    fetch->finished = true;
    pthread_mutex_unlock(&fetch->mu);
}

static void agy_fetch_destroy(mcode_model_fetch *fetch) {
    if (!fetch) return;
    if (fetch->proc) {
        mcode_proc_destroy(fetch->proc);
        fetch->proc = NULL;
    }
    mcode_str_free(&fetch->stdout_buf);
    pthread_mutex_destroy(&fetch->mu);
    free(fetch);
}

void mcode_model_catalog_init(mcode_model_catalog *cat) {
    if (!cat) return;
    memset(cat, 0, sizeof *cat);
    load_claude_default_models(cat);
    if (!load_codex_models_from_cache(cat)) {
        load_codex_fallback_models(cat);
    }
    load_agy_fallback_models(cat);
    load_cli_config_defaults(cat);
}

void mcode_model_catalog_refresh(mcode_model_catalog *cat, const mcode_cli_status *clis) {
    if (!cat) return;
    load_cli_config_defaults(cat);
    if (!load_codex_models_from_cache(cat) && cat->count[APP_AGENT_CODEX] == 0) {
        load_codex_fallback_models(cat);
    }
    if (clis && clis->available[APP_AGENT_AGY] && clis->resolved_path[APP_AGENT_AGY][0] &&
        !cat->agy_fetch) {
        mcode_model_fetch *fetch = calloc(1, sizeof *fetch);
        if (!fetch) return;
        pthread_mutex_init(&fetch->mu, NULL);
        mcode_str_init(&fetch->stdout_buf);
        const char *argv[] = { clis->resolved_path[APP_AGENT_AGY], "models", NULL };
        fetch->proc = mcode_proc_spawn(&(mcode_proc_desc){
            .argv = argv,
            .cwd = NULL,
            .merge_stderr = false,
            .on_bytes = on_agy_models_bytes,
            .on_exit = on_agy_models_exit,
            .user = fetch,
        });
        if (!fetch->proc) {
            agy_fetch_destroy(fetch);
            return;
        }
        cat->agy_fetch = fetch;
        cat->loading_agy = true;
    }
}

bool mcode_model_catalog_poll(mcode_model_catalog *cat) {
    if (!cat || !cat->agy_fetch) return false;
    mcode_model_fetch *fetch = cat->agy_fetch;
    pthread_mutex_lock(&fetch->mu);
    bool finished = fetch->finished;
    pthread_mutex_unlock(&fetch->mu);
    if (!finished) return false;

    bool updated = false;
    if (fetch->exit_code == 0 && fetch->stdout_buf.len > 0) {
        mcode_model_option parsed[MCODE_MAX_MODELS_PER_AGENT];
        int cnt = mcode_agent_parse_agy_models_text(mcode_str_cstr(&fetch->stdout_buf),
                                                    parsed, MCODE_MAX_MODELS_PER_AGENT);
        if (cnt > 0) {
            memcpy(cat->items[APP_AGENT_AGY], parsed, (size_t)cnt * sizeof parsed[0]);
            cat->count[APP_AGENT_AGY] = cnt;
            cat->loaded_agy = true;
            updated = true;
        }
    }
    agy_fetch_destroy(fetch);
    cat->agy_fetch = NULL;
    cat->loading_agy = false;
    return updated;
}

void mcode_model_catalog_begin_shutdown(mcode_model_catalog *cat) {
    if (!cat) return;
    if (cat->agy_fetch && cat->agy_fetch->proc) {
        mcode_proc_kill(cat->agy_fetch->proc);
    }
}

void mcode_model_catalog_free(mcode_model_catalog *cat) {
    if (!cat) return;
    if (cat->agy_fetch) {
        agy_fetch_destroy(cat->agy_fetch);
        cat->agy_fetch = NULL;
    }
    cat->loading_agy = false;
}

const char *mcode_agent_default_model(const app_settings *settings, app_agent_kind agent) {
    if (!settings) return "";
    switch (agent) {
    case APP_AGENT_CLAUDE: return settings->mcode.default_claude_model;
    case APP_AGENT_CODEX:  return settings->mcode.default_codex_model;
    case APP_AGENT_AGY:    return settings->mcode.default_agy_model;
    default:               return "";
    }
}

const char *mcode_agent_effective_model(const mcode_thread *thread, const app_settings *settings) {
    if (!thread) return "";
    if (thread->model[0]) return thread->model;
    return mcode_agent_default_model(settings, thread->agent);
}

const mcode_model_option *mcode_model_find(const mcode_model_catalog *cat, app_agent_kind agent,
                                           const char *model_id) {
    if (!cat || !model_id || !model_id[0] || agent < 0 || agent >= APP_AGENT_COUNT) return NULL;
    for (int i = 0; i < cat->count[agent]; ++i) {
        if (strcmp(cat->items[agent][i].id, model_id) == 0) {
            return &cat->items[agent][i];
        }
    }
    return NULL;
}

const char *mcode_model_display_label(const mcode_model_catalog *cat, app_agent_kind agent,
                                      const char *model_id) {
    if (!model_id || !model_id[0]) return "Default";
    const mcode_model_option *opt = mcode_model_find(cat, agent, model_id);
    if (opt && opt->label[0]) return opt->label;
    return model_id;
}

const char *mcode_effort_display_label(const char *effort) {
    if (!effort || !effort[0]) return "";
    if (strcmp(effort, "minimal") == 0 || strcmp(effort, "Minimal") == 0) return "Minimal";
    if (strcmp(effort, "low") == 0 || strcmp(effort, "Low") == 0) return "Low";
    if (strcmp(effort, "medium") == 0 || strcmp(effort, "Medium") == 0) return "Medium";
    if (strcmp(effort, "high") == 0 || strcmp(effort, "High") == 0) return "High";
    if (strcmp(effort, "xhigh") == 0 || strcmp(effort, "XHigh") == 0) return "XHigh";
    if (strcmp(effort, "max") == 0 || strcmp(effort, "Max") == 0) return "Max";
    if (strcmp(effort, "ultra") == 0 || strcmp(effort, "Ultra") == 0) return "Ultra";
    return effort;
}

static void prettify_model_display_name(const char *src, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!src || !src[0]) return;
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < cap; ++i) {
        char ch = src[i];
        if (ch == '-' && i > 0 && isdigit((unsigned char)src[i - 1]) &&
            isupper((unsigned char)src[i + 1])) {
            ch = ' ';
        }
        out[w++] = ch;
    }
    out[w] = '\0';
}

void mcode_model_format_display(const mcode_model_catalog *cat, const mcode_thread *thread,
                                const app_settings *settings,
                                char *name_out, size_t name_cap,
                                char *effort_out, size_t effort_cap) {
    if (name_out && name_cap > 0) name_out[0] = '\0';
    if (effort_out && effort_cap > 0) effort_out[0] = '\0';
    if (!thread) return;

    app_agent_kind ag = thread->agent;
    const char *model_id = mcode_agent_effective_model(thread, settings);
    if (!model_id[0] && cat && ag >= 0 && ag < APP_AGENT_COUNT) {
        model_id = cat->cli_default_model[ag];
    }

    const mcode_model_option *opt = mcode_model_find(cat, ag, model_id);
    const char *raw_label = (opt && opt->label[0]) ? opt->label : model_id;

    char base_buf[128] = "";
    char paren_tag[32] = "";
    if (raw_label && raw_label[0]) {
        size_t len = strlen(raw_label);
        if (len >= 4 && raw_label[len - 1] == ')') {
            const char *lp = strrchr(raw_label, '(');
            if (lp && lp > raw_label && lp[-1] == ' ') {
                size_t tag_len = (size_t)((raw_label + len - 1) - (lp + 1));
                size_t base_len = (size_t)((lp - 1) - raw_label);
                if (tag_len > 0 && tag_len < sizeof paren_tag && base_len > 0) {
                    memcpy(paren_tag, lp + 1, tag_len);
                    paren_tag[tag_len] = '\0';
                    if (base_len >= sizeof base_buf) base_len = sizeof base_buf - 1;
                    memcpy(base_buf, raw_label, base_len);
                    base_buf[base_len] = '\0';
                }
            }
        }
        if (!base_buf[0]) {
            snprintf(base_buf, sizeof base_buf, "%s", raw_label);
        }
    }

    if (name_out && name_cap > 0) {
        if (base_buf[0]) {
            prettify_model_display_name(base_buf, name_out, name_cap);
        } else {
            snprintf(name_out, name_cap, "Default model");
        }
    }

    if (effort_out && effort_cap > 0) {
        const char *eff = "";
        if (thread->reasoning_effort[0]) {
            eff = mcode_effort_display_label(thread->reasoning_effort);
        } else if (paren_tag[0]) {
            eff = mcode_effort_display_label(paren_tag);
        } else if (cat && ag >= 0 && ag < APP_AGENT_COUNT && cat->cli_default_effort[ag][0]) {
            eff = mcode_effort_display_label(cat->cli_default_effort[ag]);
        } else if (opt && opt->default_effort[0]) {
            eff = mcode_effort_display_label(opt->default_effort);
        }
        snprintf(effort_out, effort_cap, "%s", eff);
    }
}

bool mcode_model_supports_fast(const mcode_model_catalog *cat, const mcode_thread *thread,
                               const app_settings *settings, const char **out_desc) {
    if (out_desc) *out_desc = "";
    if (!cat || !thread) return false;
    app_agent_kind ag = thread->agent;
    if (ag < 0 || ag >= APP_AGENT_COUNT) return false;
    const char *model_id = mcode_agent_effective_model(thread, settings);
    if (!model_id[0]) model_id = cat->cli_default_model[ag];
    const mcode_model_option *opt = mcode_model_find(cat, ag, model_id);
    if (!opt || !opt->supports_fast) return false;
    if (out_desc) *out_desc = opt->fast_desc;
    return true;
}

bool mcode_thread_is_fast_mode(const mcode_model_catalog *cat, const mcode_thread *thread) {
    if (!thread) return false;
    if (thread->speed_mode == MCODE_SPEED_FAST) return true;
    if (thread->speed_mode == MCODE_SPEED_STANDARD) return false;
    if (cat && thread->agent >= 0 && thread->agent < APP_AGENT_COUNT) {
        return cat->cli_default_fast[thread->agent];
    }
    return false;
}

int64_t mcode_model_context_window(const mcode_model_catalog *cat, const mcode_thread *thread,
                                   const app_settings *settings) {
    if (!thread) return 0;
    app_agent_kind ag = thread->agent;
    if (ag < 0 || ag >= APP_AGENT_COUNT) return 0;
    const char *model_id = mcode_agent_effective_model(thread, settings);
    if (!model_id[0] && cat) model_id = cat->cli_default_model[ag];
    const mcode_model_option *opt = mcode_model_find(cat, ag, model_id);
    if (opt && opt->context_window > 0) return opt->context_window;
    if (ag == APP_AGENT_CLAUDE) return 200000;
    if (ag == APP_AGENT_CODEX) return 272000;
    if (ag == APP_AGENT_AGY) {
        if (model_id && strstr(model_id, "claude") != NULL) return 200000;
        return 1000000;
    }
    return 0;
}

/* ---- Live Provider Usage & Quota Tracker ---- */

static bool reset_credit_is_available(const mcode_reset_credit *c) {
    if (!c) return false;
    return !c->status[0] || strcmp(c->status, "available") == 0;
}

static bool reset_credit_comes_before(const mcode_reset_credit *a, const mcode_reset_credit *b) {
    bool a_avail = reset_credit_is_available(a);
    bool b_avail = reset_credit_is_available(b);
    if (a_avail != b_avail) return a_avail;
    bool a_exp = a->expires_at > 0;
    bool b_exp = b->expires_at > 0;
    if (a_exp != b_exp) return a_exp;
    if (a_exp && a->expires_at != b->expires_at) return a->expires_at < b->expires_at;
    if (a->granted_at != b->granted_at) return a->granted_at < b->granted_at;
    return strcmp(a->id, b->id) < 0;
}

static bool parse_codex_reset_credits_val(yyjson_val *res_obj, mcode_provider_quota *out) {
    if (!yyjson_is_obj(res_obj) || !out) return false;
    yyjson_val *rc = yyjson_obj_get(res_obj, "rateLimitResetCredits");
    if (!rc) return false;

    out->reset_credits_checked = true;
    out->reset_credits_available = 0;
    out->reset_credit_count = 0;
    if (yyjson_is_null(rc) || !yyjson_is_obj(rc)) return true;

    yyjson_val *avail_v = yyjson_obj_get(rc, "availableCount");
    bool has_avail_count = yyjson_is_num(avail_v);
    if (has_avail_count) {
        int64_t avail = ji64(rc, "availableCount");
        out->reset_credits_available = avail > 0 ? avail : 0;
    }

    yyjson_val *arr = yyjson_obj_get(rc, "credits");
    int avail_seen = 0;
    if (yyjson_is_arr(arr)) {
        size_t idx, max;
        yyjson_val *cv;
        yyjson_arr_foreach(arr, idx, max, cv) {
            if (!yyjson_is_obj(cv)) continue;
            const char *cid = jstr(cv, "id");
            if (!cid || !cid[0]) continue;

            mcode_reset_credit item;
            memset(&item, 0, sizeof item);
            snprintf(item.id, sizeof item.id, "%s", cid);
            const char *rtype = jstr(cv, "resetType");
            if (rtype && rtype[0]) snprintf(item.reset_type, sizeof item.reset_type, "%s", rtype);
            const char *st = jstr(cv, "status");
            if (st && st[0]) snprintf(item.status, sizeof item.status, "%s", st);
            item.granted_at = ji64(cv, "grantedAt");
            item.expires_at = ji64(cv, "expiresAt");
            const char *title = jstr(cv, "title");
            if (title && title[0]) snprintf(item.title, sizeof item.title, "%s", title);
            const char *desc = jstr(cv, "description");
            if (desc && desc[0]) snprintf(item.description, sizeof item.description, "%s", desc);

            if (reset_credit_is_available(&item)) ++avail_seen;

            if (out->reset_credit_count < MCODE_MAX_RESET_CREDITS) {
                out->reset_credits[out->reset_credit_count++] = item;
            } else {
                /* Keep the highest-priority (available, soonest-expiring) entries if list exceeds cap. */
                int worst = 0;
                for (int i = 1; i < out->reset_credit_count; ++i) {
                    if (reset_credit_comes_before(&out->reset_credits[worst], &out->reset_credits[i])) {
                        worst = i;
                    }
                }
                if (reset_credit_comes_before(&item, &out->reset_credits[worst])) {
                    out->reset_credits[worst] = item;
                }
            }
        }
    }

    for (int i = 1; i < out->reset_credit_count; ++i) {
        mcode_reset_credit key = out->reset_credits[i];
        int j = i - 1;
        while (j >= 0 && reset_credit_comes_before(&key, &out->reset_credits[j])) {
            out->reset_credits[j + 1] = out->reset_credits[j];
            --j;
        }
        out->reset_credits[j + 1] = key;
    }

    if (!has_avail_count) {
        out->reset_credits_available = avail_seen;
    }
    return true;
}

static bool parse_codex_rate_limits_result(yyjson_val *src, mcode_provider_quota *out) {
    if (!yyjson_is_obj(src) || !out) return false;
    bool got_rc = parse_codex_reset_credits_val(src, out);
    yyjson_val *rl = yyjson_obj_get(src, "rateLimits");
    if (!yyjson_is_obj(rl)) {
        yyjson_val *by_id = yyjson_obj_get(src, "rateLimitsByLimitId");
        if (yyjson_is_obj(by_id)) {
            yyjson_val *codex_rl = yyjson_obj_get(by_id, "codex");
            if (yyjson_is_obj(codex_rl)) {
                rl = codex_rl;
            } else {
                size_t idx, max;
                yyjson_val *k, *v;
                yyjson_obj_foreach(by_id, idx, max, k, v) {
                    (void)k;
                    if (yyjson_is_obj(v)) { rl = v; break; }
                }
            }
        }
    }
    if (!yyjson_is_obj(rl)) rl = src;
    bool got_rl = parse_codex_rate_snapshot(rl, out);
    return got_rl || got_rc;
}

bool mcode_agent_parse_codex_rate_limits_json(const char *json, size_t len,
                                              mcode_provider_quota *out) {
    if (!json || len == 0 || !out) return false;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *src = root;
    if (yyjson_is_obj(root)) {
        yyjson_val *res = yyjson_obj_get(root, "result");
        if (yyjson_is_obj(res)) src = res;
    }
    bool ok = parse_codex_rate_limits_result(src, out);
    yyjson_doc_free(doc);
    return ok;
}

static mcode_reset_credit_outcome parse_codex_reset_consume_result_val(yyjson_val *res_v,
                                                                       char *err_out,
                                                                       size_t err_cap) {
    if (err_out && err_cap) err_out[0] = '\0';
    if (!yyjson_is_obj(res_v)) {
        if (err_out && err_cap) {
            snprintf(err_out, err_cap, "Unexpected reset credit response from Codex.");
        }
        return MCODE_RESET_OUTCOME_ERROR;
    }
    const char *outcome = jstr(res_v, "outcome");
    if (!outcome || !outcome[0]) {
        if (err_out && err_cap) {
            snprintf(err_out, err_cap, "Codex response did not include a reset outcome.");
        }
        return MCODE_RESET_OUTCOME_ERROR;
    }
    if (strcmp(outcome, "reset") == 0) return MCODE_RESET_OUTCOME_RESET;
    if (strcmp(outcome, "nothingToReset") == 0) return MCODE_RESET_OUTCOME_NOTHING_TO_RESET;
    if (strcmp(outcome, "noCredit") == 0) return MCODE_RESET_OUTCOME_NO_CREDIT;
    if (strcmp(outcome, "alreadyRedeemed") == 0) return MCODE_RESET_OUTCOME_ALREADY_REDEEMED;
    if (err_out && err_cap) {
        snprintf(err_out, err_cap, "Unknown reset outcome: %s", outcome);
    }
    return MCODE_RESET_OUTCOME_ERROR;
}

mcode_reset_credit_outcome mcode_agent_parse_codex_reset_consume_json(const char *json, size_t len,
                                                                      char *err_out, size_t err_cap) {
    if (err_out && err_cap) err_out[0] = '\0';
    if (!json || len == 0) return MCODE_RESET_OUTCOME_ERROR;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        if (err_out && err_cap) snprintf(err_out, err_cap, "Invalid JSON from Codex.");
        return MCODE_RESET_OUTCOME_ERROR;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    mcode_reset_credit_outcome out = MCODE_RESET_OUTCOME_ERROR;
    if (yyjson_is_obj(root)) {
        yyjson_val *err_v = yyjson_obj_get(root, "error");
        if (yyjson_is_obj(err_v)) {
            const char *emsg = jstr(err_v, "message");
            if (err_out && err_cap) {
                snprintf(err_out, err_cap, "%s",
                         (emsg && emsg[0]) ? emsg : "Codex failed to redeem reset credit.");
            }
        } else {
            yyjson_val *res_v = yyjson_obj_get(root, "result");
            out = parse_codex_reset_consume_result_val(yyjson_is_obj(res_v) ? res_v : root,
                                                       err_out, err_cap);
        }
    } else if (err_out && err_cap) {
        snprintf(err_out, err_cap, "Unexpected reset credit response from Codex.");
    }
    yyjson_doc_free(doc);
    return out;
}

static bool parse_codex_account_usage_val(yyjson_val *src, const char *today_ymd,
                                          mcode_provider_quota *out) {
    if (!yyjson_is_obj(src) || !out) return false;
    bool got = false;
    yyjson_val *sum = yyjson_obj_get(src, "summary");
    if (yyjson_is_obj(sum)) {
        int64_t life = ji64(sum, "lifetimeTokens");
        int streak = (int)ji64(sum, "currentStreakDays");
        if (life > 0) {
            out->account_lifetime_tokens = life;
            got = true;
        }
        if (streak > 0) out->current_streak_days = streak;
    }
    yyjson_val *buckets = yyjson_obj_get(src, "dailyUsageBuckets");
    if (yyjson_is_arr(buckets) && today_ymd && today_ymd[0]) {
        size_t idx, max;
        yyjson_val *b;
        yyjson_arr_foreach(buckets, idx, max, b) {
            const char *sd = jstr(b, "startDate");
            if (sd && strncmp(sd, today_ymd, 10) == 0) {
                int64_t toks = ji64(b, "tokens");
                if (toks >= 0) {
                    out->account_today_tokens = toks;
                    got = true;
                }
                break;
            }
        }
    }
    return got;
}

bool mcode_agent_parse_codex_account_usage_json(const char *json, size_t len,
                                                const char *today_ymd,
                                                mcode_provider_quota *out) {
    if (!json || len == 0 || !out) return false;
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *src = root;
    if (yyjson_is_obj(root)) {
        yyjson_val *res = yyjson_obj_get(root, "result");
        if (yyjson_is_obj(res)) src = res;
    }
    bool ok = parse_codex_account_usage_val(src, today_ymd, out);
    yyjson_doc_free(doc);
    return ok;
}

struct mcode_usage_fetch {
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str out_line;
    char today_ymd[16];
    double started_mono;
    bool sent_queries;
    bool got_rate_limits;
    bool got_account_usage;
    bool timed_out;
    bool finished;
    mcode_provider_quota result;
};

static void usage_fetch_send_queries(mcode_usage_fetch *f) {
    if (!f || !f->proc) return;
    static const char k_queries[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"account/rateLimits/read\","
        "\"params\":{\"excludeResetCreditDetails\":false}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"account/usage/read\",\"params\":{}}\n";
    mcode_proc_write(f->proc, k_queries, sizeof k_queries - 1);
}

static void usage_fetch_handle_line(mcode_usage_fetch *f, const char *line) {
    if (!f || !line) return;
    while (*line && isspace((unsigned char)*line)) ++line;
    if (*line != '{') return;

    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *id_v = yyjson_obj_get(root, "id");
    if (yyjson_is_int(id_v)) {
        int64_t rid = yyjson_get_sint(id_v);
        yyjson_val *err_v = yyjson_obj_get(root, "error");
        yyjson_val *res_v = yyjson_obj_get(root, "result");

        bool should_send = false;
        bool should_stop = false;

        pthread_mutex_lock(&f->mu);
        if (rid == 1) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                snprintf(f->result.error, sizeof f->result.error, "%s",
                         emsg ? emsg : "Codex initialize failed");
                should_stop = true;
            } else if (!f->sent_queries) {
                f->sent_queries = true;
                should_send = true;
            }
        } else if (rid == 2) {
            f->got_rate_limits = true;
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                if (emsg && emsg[0]) {
                    snprintf(f->result.error, sizeof f->result.error, "%s", emsg);
                }
            } else if (yyjson_is_obj(res_v)) {
                parse_codex_rate_limits_result(res_v, &f->result);
            }
            if (f->got_account_usage) should_stop = true;
        } else if (rid == 3) {
            f->got_account_usage = true;
            if (!err_v && yyjson_is_obj(res_v)) {
                parse_codex_account_usage_val(res_v, f->today_ymd, &f->result);
            }
            if (f->got_rate_limits) should_stop = true;
        }
        pthread_mutex_unlock(&f->mu);

        if (should_send) {
            usage_fetch_send_queries(f);
        } else if (should_stop && f->proc) {
            mcode_proc_close_stdin(f->proc);
            mcode_proc_stop(f->proc);
        }
    }

    yyjson_doc_free(doc);
}

static void on_usage_fetch_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    if (is_stderr) return;
    mcode_usage_fetch *f = (mcode_usage_fetch *)user;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            usage_fetch_handle_line(f, mcode_str_cstr(&f->out_line));
            mcode_str_clear(&f->out_line);
        } else if (f->out_line.len < 256 * 1024) {
            mcode_str_append(&f->out_line, &data[i], 1);
        }
    }
}

static void on_usage_fetch_exit(int exit_code, bool killed, void *user) {
    (void)exit_code;
    (void)killed;
    mcode_usage_fetch *f = (mcode_usage_fetch *)user;
    if (f->out_line.len > 0) {
        usage_fetch_handle_line(f, mcode_str_cstr(&f->out_line));
        mcode_str_clear(&f->out_line);
    }
    pthread_mutex_lock(&f->mu);
    f->finished = true;
    pthread_mutex_unlock(&f->mu);
}

static void usage_fetch_destroy(mcode_usage_fetch *f) {
    if (!f) return;
    if (f->proc) {
        mcode_proc_destroy(f->proc);
        f->proc = NULL;
    }
    mcode_str_free(&f->out_line);
    pthread_mutex_destroy(&f->mu);
    free(f);
}

struct mcode_reset_consume {
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str out_line;
    char idempotency_key[40];
    char credit_id[128];
    double started_mono;
    bool sent_consume;
    bool got_consume;
    bool sent_refresh;
    bool got_refresh;
    bool timed_out;
    bool finished;
    mcode_reset_credit_outcome outcome;
    char error[160];
    mcode_provider_quota refreshed_quota;
};

static void reset_consume_send_consume(mcode_reset_consume *c) {
    if (!c || !c->proc) return;
    static const char k_initialized[] = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}\n";
    mcode_proc_write(c->proc, k_initialized, sizeof k_initialized - 1);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", 2);
    yyjson_mut_obj_add_str(doc, root, "method", "account/rateLimitResetCredit/consume");
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "idempotencyKey", c->idempotency_key);
    if (c->credit_id[0]) {
        yyjson_mut_obj_add_strcpy(doc, params, "creditId", c->credit_id);
    }
    yyjson_mut_obj_add_val(doc, root, "params", params);
    send_json_doc(c->proc, doc);
    yyjson_mut_doc_free(doc);
}

static void reset_consume_send_refresh(mcode_reset_consume *c) {
    if (!c || !c->proc) return;
    static const char k_refresh[] =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"account/rateLimits/read\","
        "\"params\":{\"excludeResetCreditDetails\":false}}\n";
    mcode_proc_write(c->proc, k_refresh, sizeof k_refresh - 1);
}

static void reset_consume_handle_line(mcode_reset_consume *c, const char *line) {
    if (!c || !line) return;
    while (*line && isspace((unsigned char)*line)) ++line;
    if (*line != '{') return;

    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *id_v = yyjson_obj_get(root, "id");
    if (yyjson_is_int(id_v)) {
        int64_t rid = yyjson_get_sint(id_v);
        yyjson_val *err_v = yyjson_obj_get(root, "error");
        yyjson_val *res_v = yyjson_obj_get(root, "result");

        bool should_send_consume = false;
        bool should_send_refresh = false;
        bool should_stop = false;

        pthread_mutex_lock(&c->mu);
        if (rid == 1) {
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                snprintf(c->error, sizeof c->error, "%s",
                         (emsg && emsg[0]) ? emsg : "Codex initialize failed.");
                c->outcome = MCODE_RESET_OUTCOME_ERROR;
                should_stop = true;
            } else if (!c->sent_consume) {
                c->sent_consume = true;
                should_send_consume = true;
            }
        } else if (rid == 2) {
            c->got_consume = true;
            if (err_v) {
                const char *emsg = jstr(err_v, "message");
                snprintf(c->error, sizeof c->error, "%s",
                         (emsg && emsg[0]) ? emsg : "Codex failed to redeem reset credit.");
                c->outcome = MCODE_RESET_OUTCOME_ERROR;
                should_stop = true;
            } else {
                c->outcome = parse_codex_reset_consume_result_val(res_v, c->error, sizeof c->error);
                if (c->outcome != MCODE_RESET_OUTCOME_ERROR && !c->sent_refresh) {
                    c->sent_refresh = true;
                    should_send_refresh = true;
                } else {
                    should_stop = true;
                }
            }
        } else if (rid == 3) {
            if (!err_v && yyjson_is_obj(res_v)) {
                if (parse_codex_rate_limits_result(res_v, &c->refreshed_quota)) {
                    c->got_refresh = true;
                }
            }
            should_stop = true;
        }
        pthread_mutex_unlock(&c->mu);

        if (should_send_consume) {
            reset_consume_send_consume(c);
        } else if (should_send_refresh) {
            reset_consume_send_refresh(c);
        } else if (should_stop && c->proc) {
            mcode_proc_close_stdin(c->proc);
            mcode_proc_stop(c->proc);
        }
    }

    yyjson_doc_free(doc);
}

static void on_reset_consume_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    if (is_stderr) return;
    mcode_reset_consume *c = (mcode_reset_consume *)user;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            reset_consume_handle_line(c, mcode_str_cstr(&c->out_line));
            mcode_str_clear(&c->out_line);
        } else if (c->out_line.len < 256 * 1024) {
            mcode_str_append(&c->out_line, &data[i], 1);
        }
    }
}

static void on_reset_consume_exit(int exit_code, bool killed, void *user) {
    (void)exit_code;
    (void)killed;
    mcode_reset_consume *c = (mcode_reset_consume *)user;
    if (c->out_line.len > 0) {
        reset_consume_handle_line(c, mcode_str_cstr(&c->out_line));
        mcode_str_clear(&c->out_line);
    }
    pthread_mutex_lock(&c->mu);
    c->finished = true;
    pthread_mutex_unlock(&c->mu);
}

static void reset_consume_destroy(mcode_reset_consume *c) {
    if (!c) return;
    if (c->proc) {
        mcode_proc_destroy(c->proc);
        c->proc = NULL;
    }
    mcode_str_free(&c->out_line);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

static void format_local_today_ymd(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    time_t now = time(NULL);
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &now) != 0) return;
#else
    if (!localtime_r(&now, &lt)) return;
#endif
    snprintf(out, cap, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static int64_t days_from_civil_utc(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t parse_iso8601_utc_epoch(const char *s) {
    if (!s || strlen(s) < 19) return 0;
    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) return 0;
    if (yr <= 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31) return 0;
    if (hr < 0 || hr > 23 || mn < 0 || mn > 59 || sc < 0 || sc > 60) return 0;
    int64_t days = days_from_civil_utc(yr, (unsigned)mo, (unsigned)dy);
    return days * 86400LL + (int64_t)hr * 3600LL + (int64_t)mn * 60LL + (int64_t)sc;
}

bool mcode_agent_parse_agy_usage_json(const char *json, size_t len,
                                      mcode_provider_quota *out) {
    if (!json || len == 0 || !out) return false;
    /* Find the JSON object in stdout (skipping any leading ANSI/spinner chatter). */
    const char *start = memchr(json, '{', len);
    if (!start) return false;
    size_t rem_len = len - (size_t)(start - json);
    yyjson_doc *doc = yyjson_read(start, rem_len, YYJSON_READ_STOP_WHEN_DONE);
    if (!doc) return false;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cmd = yyjson_is_obj(root) ? yyjson_obj_get(root, "command") : NULL;
    yyjson_val *data = yyjson_is_obj(cmd) ? yyjson_obj_get(cmd, "data") : NULL;
    yyjson_val *groups = yyjson_is_obj(data) ? yyjson_obj_get(data, "groups") : NULL;

    bool got_any = false;
    if (yyjson_is_arr(groups)) {
        out->group_count = 0;
        size_t gi, gmax;
        yyjson_val *gv;
        yyjson_arr_foreach(groups, gi, gmax, gv) {
            if (out->group_count >= MCODE_MAX_QUOTA_GROUPS || !yyjson_is_obj(gv)) break;
            const char *gname = jstr(gv, "name");
            yyjson_val *buckets = yyjson_obj_get(gv, "buckets");
            if (!yyjson_is_arr(buckets)) continue;

            mcode_quota_group grp;
            memset(&grp, 0, sizeof grp);
            snprintf(grp.name, sizeof grp.name, "%s", (gname && gname[0]) ? gname : "Models");

            size_t bi, bmax;
            yyjson_val *bv;
            yyjson_arr_foreach(buckets, bi, bmax, bv) {
                if (!yyjson_is_obj(bv)) continue;
                yyjson_val *rem_v = yyjson_obj_get(bv, "remaining_fraction");
                if (!yyjson_is_num(rem_v)) continue;
                double rem = yyjson_get_num(rem_v);
                if (rem < 0.0) rem = 0.0;
                if (rem > 1.0) rem = 1.0;
                /* Match `agy -p "/usage"` terminal display by flooring remaining_fraction * 100. */
                int rem_pct = clamp_percent((int)(rem * 100.0 + 1e-9));
                int used_pct = clamp_percent(100 - rem_pct);
                int64_t resets = parse_iso8601_utc_epoch(jstr(bv, "reset_time"));
                const char *win = jstr(bv, "window");
                const char *bid = jstr(bv, "id");
                const char *bname = jstr(bv, "name");

                bool is_weekly = (win && (strcmp(win, "weekly") == 0 || strcmp(win, "7d") == 0)) ||
                                 (bid && strstr(bid, "weekly") != NULL) ||
                                 (bname && strstr(bname, "Weekly") != NULL);
                mcode_rate_window *dst_w = is_weekly ? &grp.secondary : &grp.primary;
                dst_w->available = true;
                dst_w->used_percent = used_pct;
                dst_w->window_mins = is_weekly ? 10080 : 300;
                dst_w->resets_at = resets;
            }

            if (grp.primary.available || grp.secondary.available) {
                out->groups[out->group_count++] = grp;
                got_any = true;
            }
        }
    }

    if (got_any) {
        out->primary = out->groups[0].primary;
        out->secondary = out->groups[0].secondary;
        out->available = true;
        out->auth_checked = true;
        out->logged_in = true;
        out->updated_mono = mcode_now_mono();
        out->updated_epoch = mcode_now_epoch();
        out->error[0] = '\0';
    }
    yyjson_doc_free(doc);
    return got_any;
}

bool mcode_agent_parse_claude_auth_json(const char *json, size_t len,
                                        mcode_provider_quota *out) {
    if (!json || len == 0 || !out) return false;
    const char *start = memchr(json, '{', len);
    if (!start) return false;
    size_t rem_len = len - (size_t)(start - json);
    yyjson_doc *doc = yyjson_read(start, rem_len, YYJSON_READ_STOP_WHEN_DONE);
    if (!doc) return false;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val *li = yyjson_obj_get(root, "loggedIn");
    if (yyjson_is_bool(li)) {
        out->auth_checked = true;
        out->logged_in = yyjson_get_bool(li);
    }
    const char *method = jstr(root, "authMethod");
    if (method && method[0]) {
        snprintf(out->auth_method, sizeof out->auth_method, "%s", method);
    }
    const char *plan = jstr(root, "subscriptionType");
    if (!plan || !plan[0]) plan = jstr(root, "billingType");
    if (!plan || !plan[0]) plan = jstr(root, "plan");
    if (plan && plan[0]) {
        snprintf(out->plan, sizeof out->plan, "%s", plan);
    }
    out->updated_mono = mcode_now_mono();
    out->updated_epoch = mcode_now_epoch();
    yyjson_doc_free(doc);
    return out->auth_checked;
}

#ifndef _WIN32
static void scan_claude_jsonl_file(const char *path, const char *today_ymd,
                                   int64_t *today_tok, int64_t *life_tok, double *cost_usd) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *line = malloc(65536);
    if (!line) { fclose(f); return; }
    double max_session_cost = 0.0;
    while (fgets(line, 65536, f)) {
        bool has_usage = (strstr(line, "\"usage\"") != NULL);
        bool has_cost = (!has_usage && strstr(line, "\"cost-state\"") != NULL);
        if (!has_usage && !has_cost) continue;
        size_t llen = strlen(line);
        yyjson_doc *doc = yyjson_read(line, llen, 0);
        if (!doc) continue;
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (yyjson_is_obj(root)) {
            const char *type = jstr(root, "type");
            if (has_cost && type && strcmp(type, "cost-state") == 0) {
                yyjson_val *cv = yyjson_obj_get(root, "totalCostUSD");
                if (yyjson_is_num(cv)) {
                    double c = yyjson_get_num(cv);
                    if (c > max_session_cost) max_session_cost = c;
                }
            } else if (has_usage) {
                yyjson_val *msg = yyjson_obj_get(root, "message");
                yyjson_val *u = yyjson_is_obj(msg) ? yyjson_obj_get(msg, "usage") : NULL;
                if (yyjson_is_obj(u)) {
                    int64_t t = ji64(u, "input_tokens")
                              + ji64(u, "output_tokens")
                              + ji64(u, "cache_read_input_tokens")
                              + ji64(u, "cache_creation_input_tokens");
                    if (t > 0) {
                        *life_tok += t;
                        const char *ts = jstr(root, "timestamp");
                        if (ts && today_ymd && today_ymd[0] && strncmp(ts, today_ymd, 10) == 0) {
                            *today_tok += t;
                        }
                    }
                }
            }
        }
        yyjson_doc_free(doc);
    }
    *cost_usd += max_session_cost;
    free(line);
    fclose(f);
}

static void scan_claude_local_usage(const char *today_ymd,
                                    int64_t *out_today, int64_t *out_life, double *out_cost) {
    *out_today = 0;
    *out_life = 0;
    *out_cost = 0.0;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char root_dir[MCODE_PATH_MAX];
    int rn = snprintf(root_dir, sizeof root_dir, "%s/.claude/projects", home);
    if (rn <= 0 || (size_t)rn >= sizeof root_dir) return;

    DIR *d = opendir(root_dir);
    if (!d) return;
    int scanned = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && scanned < 256) {
        if (ent->d_name[0] == '.') continue;
        char sub[MCODE_PATH_MAX];
        int sn = snprintf(sub, sizeof sub, "%s/%s", root_dir, ent->d_name);
        if (sn <= 0 || (size_t)sn >= sizeof sub) continue;
        DIR *sd = opendir(sub);
        if (!sd) continue;
        struct dirent *fent;
        while ((fent = readdir(sd)) != NULL && scanned < 256) {
            size_t flen = strlen(fent->d_name);
            if (flen < 7 || strcmp(fent->d_name + flen - 6, ".jsonl") != 0) continue;
            char fp[MCODE_PATH_MAX];
            int fn = snprintf(fp, sizeof fp, "%s/%s", sub, fent->d_name);
            if (fn <= 0 || (size_t)fn >= sizeof fp) continue;
            scan_claude_jsonl_file(fp, today_ymd, out_today, out_life, out_cost);
            ++scanned;
        }
        closedir(sd);
    }
    closedir(d);
}
#else
static void scan_claude_local_usage(const char *today_ymd,
                                    int64_t *out_today, int64_t *out_life, double *out_cost) {
    (void)today_ymd;
    *out_today = 0;
    *out_life = 0;
    *out_cost = 0.0;
}
#endif

struct mcode_simple_fetch {
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str stdout_buf;
    mcode_str stderr_buf;
    double started_mono;
    double timeout_sec;
    bool timed_out;
    bool finished;
    int exit_code;
};

static void on_simple_fetch_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    mcode_simple_fetch *f = (mcode_simple_fetch *)user;
    pthread_mutex_lock(&f->mu);
    mcode_str *dst = is_stderr ? &f->stderr_buf : &f->stdout_buf;
    if (dst->len < 256 * 1024) {
        mcode_str_append(dst, data, len);
    }
    pthread_mutex_unlock(&f->mu);
}

static void on_simple_fetch_exit(int exit_code, bool killed, void *user) {
    (void)killed;
    mcode_simple_fetch *f = (mcode_simple_fetch *)user;
    pthread_mutex_lock(&f->mu);
    f->exit_code = exit_code;
    f->finished = true;
    pthread_mutex_unlock(&f->mu);
}

static mcode_simple_fetch *simple_fetch_spawn(const char *const *argv, double timeout_sec) {
    mcode_simple_fetch *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    pthread_mutex_init(&f->mu, NULL);
    mcode_str_init(&f->stdout_buf);
    mcode_str_init(&f->stderr_buf);
    f->started_mono = mcode_now_mono();
    f->timeout_sec = timeout_sec > 0.0 ? timeout_sec : 12.0;
    f->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = false,
        .open_stdin = false,
        .on_bytes = on_simple_fetch_bytes,
        .on_exit = on_simple_fetch_exit,
        .user = f,
    });
    if (!f->proc) {
        mcode_str_free(&f->stdout_buf);
        mcode_str_free(&f->stderr_buf);
        pthread_mutex_destroy(&f->mu);
        free(f);
        return NULL;
    }
    return f;
}

static bool simple_fetch_poll(mcode_simple_fetch *f) {
    if (!f) return false;
    pthread_mutex_lock(&f->mu);
    bool finished = f->finished;
    pthread_mutex_unlock(&f->mu);
    if (!finished && !f->timed_out && (mcode_now_mono() - f->started_mono) > f->timeout_sec) {
        f->timed_out = true;
        if (f->proc) mcode_proc_stop(f->proc);
    }
    return finished;
}

static void simple_fetch_destroy(mcode_simple_fetch *f) {
    if (!f) return;
    if (f->proc) {
        mcode_proc_destroy(f->proc);
        f->proc = NULL;
    }
    mcode_str_free(&f->stdout_buf);
    mcode_str_free(&f->stderr_buf);
    pthread_mutex_destroy(&f->mu);
    free(f);
}

void mcode_usage_tracker_init(mcode_usage_tracker *tracker) {
    if (!tracker) return;
    memset(tracker, 0, sizeof *tracker);
}

static void start_codex_usage_fetch(mcode_usage_tracker *tracker, const mcode_cli_status *clis) {
    if (!clis->available[APP_AGENT_CODEX] || !clis->resolved_path[APP_AGENT_CODEX][0]) return;
    if (tracker->codex_fetch) return;

    mcode_usage_fetch *f = calloc(1, sizeof *f);
    if (!f) return;
    pthread_mutex_init(&f->mu, NULL);
    mcode_str_init(&f->out_line);
    f->started_mono = mcode_now_mono();
    format_local_today_ymd(f->today_ymd, sizeof f->today_ymd);

    const char *argv[] = {
        clis->resolved_path[APP_AGENT_CODEX], "app-server", "--listen", "stdio://", NULL
    };
    f->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = false,
        .open_stdin = true,
        .on_bytes = on_usage_fetch_bytes,
        .on_exit = on_usage_fetch_exit,
        .user = f,
    });
    if (!f->proc) {
        usage_fetch_destroy(f);
        return;
    }

    static const char k_init[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"clientInfo\":{\"name\":\"mcode\",\"version\":\"0.1.0\"}}}\n";
    mcode_proc_write(f->proc, k_init, sizeof k_init - 1);

    tracker->codex_fetch = f;
    tracker->quotas[APP_AGENT_CODEX].loading = true;
}

static void start_agy_usage_fetch(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                                  bool agy_busy) {
    if (!clis->available[APP_AGENT_AGY] || !clis->resolved_path[APP_AGENT_AGY][0]) return;
    if (tracker->agy_fetch) return;
    if (agy_busy) {
        tracker->agy_refresh_pending = true;
        return;
    }
    tracker->agy_refresh_pending = false;
    const char *argv[] = {
        clis->resolved_path[APP_AGENT_AGY], "-p", "/usage",
        "--output-format", "json", "--print-timeout", "15s", NULL
    };
    mcode_simple_fetch *f = simple_fetch_spawn(argv, 18.0);
    if (!f) return;
    tracker->agy_fetch = f;
    tracker->quotas[APP_AGENT_AGY].loading = true;
}

static void start_claude_usage_fetch(mcode_usage_tracker *tracker, const mcode_cli_status *clis) {
    if (!clis->available[APP_AGENT_CLAUDE] || !clis->resolved_path[APP_AGENT_CLAUDE][0]) return;
    if (tracker->claude_fetch) return;
    const char *argv[] = {
        clis->resolved_path[APP_AGENT_CLAUDE], "auth", "status", NULL
    };
    mcode_simple_fetch *f = simple_fetch_spawn(argv, 8.0);
    if (!f) return;
    tracker->claude_fetch = f;
    tracker->quotas[APP_AGENT_CLAUDE].loading = true;
}

void mcode_usage_tracker_refresh_agent(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                                       app_agent_kind agent, bool agy_busy) {
    if (!tracker || !clis) return;
    switch (agent) {
    case APP_AGENT_CLAUDE: start_claude_usage_fetch(tracker, clis); break;
    case APP_AGENT_CODEX:  start_codex_usage_fetch(tracker, clis); break;
    case APP_AGENT_AGY:    start_agy_usage_fetch(tracker, clis, agy_busy); break;
    default: break;
    }
}

void mcode_usage_tracker_refresh(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                                 bool agy_busy) {
    if (!tracker || !clis) return;
    start_codex_usage_fetch(tracker, clis);
    start_agy_usage_fetch(tracker, clis, agy_busy);
    start_claude_usage_fetch(tracker, clis);
}

bool mcode_usage_tracker_is_loading(const mcode_usage_tracker *tracker) {
    if (!tracker) return false;
    return tracker->codex_fetch != NULL ||
           tracker->codex_reset != NULL ||
           tracker->agy_fetch != NULL ||
           tracker->claude_fetch != NULL;
}

bool mcode_usage_tracker_is_resetting_codex(const mcode_usage_tracker *tracker) {
    return tracker && tracker->codex_reset != NULL;
}

bool mcode_usage_tracker_consume_codex_reset(mcode_usage_tracker *tracker,
                                             const mcode_cli_status *clis,
                                             const char *credit_id) {
    if (!tracker || !clis) return false;
    if (!clis->available[APP_AGENT_CODEX] || !clis->resolved_path[APP_AGENT_CODEX][0]) return false;
    if (tracker->codex_reset) return false;

    mcode_reset_consume *c = calloc(1, sizeof *c);
    if (!c) return false;
    pthread_mutex_init(&c->mu, NULL);
    mcode_str_init(&c->out_line);
    c->started_mono = mcode_now_mono();

    static uint64_t s_reset_seq = 0;
    make_uuid_v4(++s_reset_seq, c, c->idempotency_key);
    if (credit_id && credit_id[0]) {
        snprintf(c->credit_id, sizeof c->credit_id, "%s", credit_id);
    }

    const char *argv[] = {
        clis->resolved_path[APP_AGENT_CODEX], "app-server", "--listen", "stdio://", NULL
    };
    c->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = NULL,
        .merge_stderr = false,
        .open_stdin = true,
        .on_bytes = on_reset_consume_bytes,
        .on_exit = on_reset_consume_exit,
        .user = c,
    });
    if (!c->proc) {
        reset_consume_destroy(c);
        return false;
    }

    static const char k_init[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"clientInfo\":{\"name\":\"mcode\",\"version\":\"0.1.0\"}}}\n";
    mcode_proc_write(c->proc, k_init, sizeof k_init - 1);

    tracker->codex_reset = c;
    tracker->codex_reset_done = false;
    tracker->codex_reset_outcome = MCODE_RESET_OUTCOME_NONE;
    tracker->codex_reset_error[0] = '\0';
    return true;
}

bool mcode_usage_tracker_take_codex_reset_result(mcode_usage_tracker *tracker,
                                                 mcode_reset_credit_outcome *out_outcome,
                                                 char *err_out, size_t err_cap) {
    if (err_out && err_cap) err_out[0] = '\0';
    if (!tracker || !tracker->codex_reset_done) return false;
    if (out_outcome) *out_outcome = tracker->codex_reset_outcome;
    if (err_out && err_cap) {
        snprintf(err_out, err_cap, "%s", tracker->codex_reset_error);
    }
    tracker->codex_reset_done = false;
    tracker->codex_reset_outcome = MCODE_RESET_OUTCOME_NONE;
    tracker->codex_reset_error[0] = '\0';
    return true;
}

bool mcode_usage_tracker_poll(mcode_usage_tracker *tracker, const mcode_cli_status *clis,
                              bool agy_busy) {
    if (!tracker) return false;
    bool updated = false;

    /* Launch deferred Antigravity usage refresh once no `agy` turn/title is running. */
    if (tracker->agy_refresh_pending && !agy_busy && !tracker->agy_fetch && clis) {
        start_agy_usage_fetch(tracker, clis, false);
    }

    /* 1. Codex app-server RPC fetch */
    if (tracker->codex_fetch) {
        mcode_usage_fetch *f = tracker->codex_fetch;
        pthread_mutex_lock(&f->mu);
        bool finished = f->finished;
        pthread_mutex_unlock(&f->mu);

        if (!finished) {
            if (!f->timed_out && (mcode_now_mono() - f->started_mono) > 10.0) {
                f->timed_out = true;
                if (f->proc) {
                    mcode_proc_close_stdin(f->proc);
                    mcode_proc_stop(f->proc);
                }
            }
        } else {
            mcode_provider_quota *dst = &tracker->quotas[APP_AGENT_CODEX];
            dst->loading = false;
            if (f->result.available || f->result.reset_credits_checked ||
                f->result.account_lifetime_tokens > 0 ||
                f->result.account_today_tokens > 0) {
                f->result.auth_checked = true;
                f->result.logged_in = true;
                merge_provider_quota(dst, &f->result);
                dst->updated_mono = mcode_now_mono();
                dst->updated_epoch = mcode_now_epoch();
                updated = true;
            } else if (f->result.error[0] && !dst->available) {
                snprintf(dst->error, sizeof dst->error, "%s", f->result.error);
                updated = true;
            } else if (f->timed_out && !dst->available) {
                snprintf(dst->error, sizeof dst->error, "Timed out checking Codex limits");
                updated = true;
            }
            usage_fetch_destroy(f);
            tracker->codex_fetch = NULL;
        }
    }

    /* 1b. Codex rate-limit reset credit redemption */
    if (tracker->codex_reset) {
        mcode_reset_consume *c = tracker->codex_reset;
        pthread_mutex_lock(&c->mu);
        bool finished = c->finished;
        pthread_mutex_unlock(&c->mu);

        if (!finished) {
            if (!c->timed_out && (mcode_now_mono() - c->started_mono) > 15.0) {
                c->timed_out = true;
                if (c->proc) {
                    mcode_proc_close_stdin(c->proc);
                    mcode_proc_stop(c->proc);
                }
            }
        } else {
            mcode_reset_credit_outcome final_outcome = MCODE_RESET_OUTCOME_ERROR;
            char final_err[160] = "";
            if (c->got_consume) {
                final_outcome = c->outcome;
                snprintf(final_err, sizeof final_err, "%s", c->error);
            } else if (c->outcome == MCODE_RESET_OUTCOME_ERROR && c->error[0]) {
                final_outcome = MCODE_RESET_OUTCOME_ERROR;
                snprintf(final_err, sizeof final_err, "%s", c->error);
            } else if (c->timed_out) {
                snprintf(final_err, sizeof final_err, "Timed out redeeming Codex reset credit.");
            } else {
                snprintf(final_err, sizeof final_err, "%s",
                         c->error[0] ? c->error : "Codex exited before completing reset.");
            }

            if (c->got_refresh) {
                mcode_provider_quota *dst = &tracker->quotas[APP_AGENT_CODEX];
                c->refreshed_quota.auth_checked = true;
                c->refreshed_quota.logged_in = true;
                merge_provider_quota(dst, &c->refreshed_quota);
                dst->updated_mono = mcode_now_mono();
                dst->updated_epoch = mcode_now_epoch();
            } else if (final_outcome != MCODE_RESET_OUTCOME_ERROR && clis) {
                start_codex_usage_fetch(tracker, clis);
            }

            tracker->codex_reset_done = true;
            tracker->codex_reset_outcome = final_outcome;
            snprintf(tracker->codex_reset_error, sizeof tracker->codex_reset_error, "%s", final_err);

            reset_consume_destroy(c);
            tracker->codex_reset = NULL;
            updated = true;
        }
    }

    /* 2. Antigravity `agy -p /usage --output-format json` fetch */
    if (tracker->agy_fetch && simple_fetch_poll(tracker->agy_fetch)) {
        mcode_simple_fetch *f = tracker->agy_fetch;
        mcode_provider_quota *dst = &tracker->quotas[APP_AGENT_AGY];
        dst->loading = false;

        mcode_provider_quota parsed;
        memset(&parsed, 0, sizeof parsed);
        if (f->stdout_buf.len > 0 &&
            mcode_agent_parse_agy_usage_json(mcode_str_cstr(&f->stdout_buf),
                                             f->stdout_buf.len, &parsed)) {
            merge_provider_quota(dst, &parsed);
            updated = true;
        } else if (!dst->available) {
            if (f->timed_out) {
                snprintf(dst->error, sizeof dst->error, "Timed out checking Antigravity limits");
            } else {
                snprintf(dst->error, sizeof dst->error, "Antigravity limits unavailable");
            }
            updated = true;
        }
        simple_fetch_destroy(f);
        tracker->agy_fetch = NULL;
    }

    /* 3. Claude `claude auth status` + local session token scan */
    if (tracker->claude_fetch && simple_fetch_poll(tracker->claude_fetch)) {
        mcode_simple_fetch *f = tracker->claude_fetch;
        mcode_provider_quota *dst = &tracker->quotas[APP_AGENT_CLAUDE];
        dst->loading = false;

        mcode_provider_quota parsed;
        memset(&parsed, 0, sizeof parsed);
        if (f->stdout_buf.len > 0 &&
            mcode_agent_parse_claude_auth_json(mcode_str_cstr(&f->stdout_buf),
                                               f->stdout_buf.len, &parsed)) {
            merge_provider_quota(dst, &parsed);
            dst->updated_mono = mcode_now_mono();
            dst->updated_epoch = mcode_now_epoch();
            dst->error[0] = '\0';
            updated = true;
        } else if (f->timed_out && !dst->auth_checked) {
            snprintf(dst->error, sizeof dst->error, "Timed out checking Claude status");
            updated = true;
        }

        char today_ymd[16] = "";
        format_local_today_ymd(today_ymd, sizeof today_ymd);
        int64_t today_tok = 0, life_tok = 0;
        double local_cost = 0.0;
        scan_claude_local_usage(today_ymd, &today_tok, &life_tok, &local_cost);
        if (today_tok != dst->account_today_tokens || life_tok != dst->account_lifetime_tokens ||
            local_cost > dst->account_cost_usd) {
            dst->account_today_tokens = today_tok;
            dst->account_lifetime_tokens = life_tok;
            if (local_cost > 0.0) dst->account_cost_usd = local_cost;
            updated = true;
        }

        simple_fetch_destroy(f);
        tracker->claude_fetch = NULL;
    }

    return updated;
}

void mcode_usage_tracker_begin_shutdown(mcode_usage_tracker *tracker) {
    if (!tracker) return;
    if (tracker->codex_fetch && tracker->codex_fetch->proc) {
        mcode_proc_kill(tracker->codex_fetch->proc);
    }
    if (tracker->codex_reset && tracker->codex_reset->proc) {
        mcode_proc_kill(tracker->codex_reset->proc);
    }
    if (tracker->agy_fetch && tracker->agy_fetch->proc) {
        mcode_proc_kill(tracker->agy_fetch->proc);
    }
    if (tracker->claude_fetch && tracker->claude_fetch->proc) {
        mcode_proc_kill(tracker->claude_fetch->proc);
    }
}

void mcode_usage_tracker_free(mcode_usage_tracker *tracker) {
    if (!tracker) return;
    if (tracker->codex_fetch) {
        usage_fetch_destroy(tracker->codex_fetch);
        tracker->codex_fetch = NULL;
    }
    if (tracker->codex_reset) {
        reset_consume_destroy(tracker->codex_reset);
        tracker->codex_reset = NULL;
    }
    if (tracker->agy_fetch) {
        simple_fetch_destroy(tracker->agy_fetch);
        tracker->agy_fetch = NULL;
    }
    if (tracker->claude_fetch) {
        simple_fetch_destroy(tracker->claude_fetch);
        tracker->claude_fetch = NULL;
    }
    tracker->agy_refresh_pending = false;
    tracker->codex_reset_done = false;
    tracker->codex_reset_outcome = MCODE_RESET_OUTCOME_NONE;
    tracker->codex_reset_error[0] = '\0';
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        tracker->quotas[i].loading = false;
    }
}

const char *mcode_agent_label(app_agent_kind kind) {
    switch (kind) {
    case APP_AGENT_CLAUDE: return "Claude Code";
    case APP_AGENT_CODEX:  return "Codex";
    case APP_AGENT_AGY:    return "Antigravity CLI";
    default:               return "Agent";
    }
}

const char *mcode_agent_short_label(app_agent_kind kind) {
    switch (kind) {
    case APP_AGENT_CLAUDE: return "Claude";
    case APP_AGENT_CODEX:  return "Codex";
    case APP_AGENT_AGY:    return "Agy";
    default:               return "AI";
    }
}

const char *mcode_agent_icon(app_agent_kind kind) {
    switch (kind) {
    case APP_AGENT_CLAUDE: return CUI_ICON_SPARKLES;
    case APP_AGENT_CODEX:  return CUI_ICON_TERMINAL;
    case APP_AGENT_AGY:    return CUI_ICON_ROCKET;
    default:               return CUI_ICON_CPU;
    }
}

mcode_agent_run *mcode_agent_start(mcode_thread *thread, const app_settings *settings,
                                   const char *prompt, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!thread || !prompt || !prompt[0]) return NULL;

    mcode_cli_status status;
    mcode_agent_detect_clis(settings, &status);
    if (!status.available[thread->agent]) {
        if (err && err_cap) {
            snprintf(err, err_cap, "%s CLI not found. Configure its path in Settings.",
                     mcode_agent_label(thread->agent));
        }
        return NULL;
    }

    const char *bin = status.resolved_path[thread->agent];
    const char *model = mcode_agent_effective_model(thread, settings);
    const char *argv[32];
    int n = 0;
    agent_proto_kind proto = AGENT_PROTO_STREAM;
    mcode_permission_mode perm_mode = thread->perm_mode;
    if (thread->auto_approve) perm_mode = MCODE_PERM_FULL_ACCESS;
    bool full_access = (perm_mode == MCODE_PERM_FULL_ACCESS);

    if (thread->agent == APP_AGENT_CLAUDE) {
        proto = AGENT_PROTO_CLAUDE_CTL;
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = "--input-format";
        argv[n++] = "stream-json";
        argv[n++] = "--output-format";
        argv[n++] = "stream-json";
        argv[n++] = "--verbose";
        argv[n++] = "--include-partial-messages";
        argv[n++] = "--replay-user-messages";
        if (thread->session_id[0]) {
            argv[n++] = "--resume";
            argv[n++] = thread->session_id;
        }
        if (model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
        if (thread->reasoning_effort[0]) {
            argv[n++] = "--effort";
            argv[n++] = thread->reasoning_effort;
        }
        if (full_access) {
            argv[n++] = "--dangerously-skip-permissions";
        } else {
            argv[n++] = "--permission-prompts";
            argv[n++] = "host";
            if (perm_mode == MCODE_PERM_ACCEPT_EDITS) {
                argv[n++] = "--permission-mode";
                argv[n++] = "acceptEdits";
            } else if (perm_mode == MCODE_PERM_PLAN) {
                argv[n++] = "--permission-mode";
                argv[n++] = "plan";
            }
        }
    } else if (thread->agent == APP_AGENT_CODEX) {
        proto = AGENT_PROTO_CODEX_RPC;
        argv[n++] = bin;
        argv[n++] = "app-server";
        argv[n++] = "--listen";
        argv[n++] = "stdio://";
    } else { /* APP_AGENT_AGY */
        proto = AGENT_PROTO_AGY_ACP;
        argv[n++] = bin;
        argv[n++] = "acp";
        argv[n++] = "--stdio";
        if (full_access) {
            argv[n++] = "--dangerously-skip-permissions";
        } else {
            argv[n++] = "--sandbox";
        }
    }
    argv[n] = NULL;

    mcode_agent_run *run = calloc(1, sizeof *run);
    if (!run) return NULL;
    run->agent = thread->agent;
    run->proto = proto;
    run->next_rpc_id = 100;
    run->init_prompt = dup_or_null(prompt);
    if (thread->workdir[0]) snprintf(run->init_cwd, sizeof run->init_cwd, "%s", thread->workdir);
    if (model[0]) snprintf(run->init_model, sizeof run->init_model, "%s", model);
    if (thread->reasoning_effort[0]) {
        snprintf(run->init_effort, sizeof run->init_effort, "%s", thread->reasoning_effort);
    }
    run->init_speed = thread->speed_mode;
    run->init_perm_mode = perm_mode;
    if (thread->session_id[0]) {
        snprintf(run->init_session_id, sizeof run->init_session_id, "%s", thread->session_id);
    }
    mcode_str_init(&run->out_line);
    mcode_str_init(&run->err_line);
    mcode_str_init(&run->stderr_tail);
    pthread_mutex_init(&run->mu, NULL);

    mcode_turn *turn = mcode_thread_add_turn(thread, prompt);
    if (!turn) {
        free(run->init_prompt);
        pthread_mutex_destroy(&run->mu);
        free(run);
        return NULL;
    }
    turn->running = true;
    turn->started_mono = mcode_now_mono();
    thread->run_started_mono = turn->started_mono;
    thread->updated_at = mcode_now_epoch();
    thread->scroll_to_bottom_frames = 2;
    thread->status_text[0] = '\0';
    memset(&thread->pending_approval, 0, sizeof thread->pending_approval);
    memset(&thread->pending_question, 0, sizeof thread->pending_question);

    run->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = thread->workdir[0] ? thread->workdir : NULL,
        .merge_stderr = false,
        .open_stdin = (proto != AGENT_PROTO_STREAM),
        .on_bytes = on_proc_bytes,
        .on_exit = on_proc_exit,
        .user = run,
    });
    if (!run->proc) {
        turn->running = false;
        turn->failed = true;
        snprintf(thread->status_text, sizeof thread->status_text, "Launch failed");
        if (err && err_cap) snprintf(err, err_cap, "Failed to spawn %s.", bin);
        free(run->init_prompt);
        pthread_mutex_destroy(&run->mu);
        free(run);
        return NULL;
    }

    if (proto == AGENT_PROTO_CLAUDE_CTL) {
        pthread_mutex_lock(&run->mu);
        claude_make_uuid_locked(run, run->claude_init_uuid);
        run->rpc_prompt_sent = true;
        pthread_mutex_unlock(&run->mu);
        claude_ctl_send_user_msg(run, run->claude_init_uuid, NULL, prompt);
    } else if (proto == AGENT_PROTO_CODEX_RPC) {
        codex_rpc_send_initialize(run);
    } else if (proto == AGENT_PROTO_AGY_ACP) {
        agy_acp_send_initialize(run);
    }

    thread->active_run = run;
    app_log_info("Started %s turn in %s", mcode_agent_label(thread->agent), thread->workdir);
    return run;
}

static bool claude_ctl_send_interrupt(mcode_agent_run *run) {
    if (!run || !run->proc) return false;
    int64_t rpc_id = 0;
    pthread_mutex_lock(&run->mu);
    if (run->next_rpc_id < 100) run->next_rpc_id = 100;
    rpc_id = run->next_rpc_id++;
    pthread_mutex_unlock(&run->mu);

    char req_id[48];
    snprintf(req_id, sizeof req_id, "int_%lld", (long long)rpc_id);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "control_request");
    yyjson_mut_obj_add_strcpy(doc, root, "request_id", req_id);
    yyjson_mut_val *req = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, req, "subtype", "interrupt");
    yyjson_mut_obj_add_val(doc, root, "request", req);
    bool ok = send_json_doc(run->proc, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

bool mcode_agent_poll(mcode_agent_run *run, mcode_thread *thread,
                      mcode_usage_tracker *usage, bool *out_finished) {
    if (out_finished) *out_finished = false;
    if (!run || !thread || thread->turn_count == 0) return false;

    bool escalate_stop = false;
    bool close_claude_stdin = false;
    pthread_mutex_lock(&run->mu);
    agent_ev *list = run->head;
    run->head = NULL;
    run->tail = NULL;
    bool finished = run->finished;
    if (!finished && run->interrupt_requested_at > 0.0 &&
        (mcode_now_mono() - run->interrupt_requested_at) > 2.5) {
        run->interrupt_requested_at = -1.0;
        escalate_stop = true;
    }
    if (!finished && run->proto == AGENT_PROTO_CLAUDE_CTL &&
        run->claude_waiting_steer_at > 0.0 &&
        (mcode_now_mono() - run->claude_waiting_steer_at) > 3.0) {
        run->claude_waiting_steer_at = -1.0;
        close_claude_stdin = true;
    }
    pthread_mutex_unlock(&run->mu);

    if (escalate_stop && run->proc) {
        mcode_proc_stop(run->proc);
    } else if (close_claude_stdin && run->proc) {
        mcode_proc_close_stdin(run->proc);
    }

    bool changed = false;
    mcode_turn *turn = &thread->turns[thread->turn_count - 1];
    while (list) {
        agent_ev *next = list->next;
        apply_event(thread, turn, usage, list);
        ev_free(list);
        list = next;
        changed = true;
    }
    if (turn->running) {
        turn->duration_sec = (float)(mcode_now_mono() - turn->started_mono);
    }
    if (finished && out_finished) *out_finished = true;
    return changed || finished;
}

void mcode_agent_stop(mcode_agent_run *run) {
    if (!run || !run->proc) return;
    pthread_mutex_lock(&run->mu);
    run->stop_requested = true;
    pthread_mutex_unlock(&run->mu);
    if (run->proto == AGENT_PROTO_CODEX_RPC) {
        codex_rpc_send_turn_interrupt(run);
    } else if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
        claude_ctl_send_interrupt(run);
    } else if (run->proto == AGENT_PROTO_AGY_ACP) {
        agy_acp_send_cancel(run);
    }
    mcode_proc_stop(run->proc);
}

bool mcode_agent_supports_live_steer(const mcode_agent_run *run) {
    return run != NULL &&
           (run->proto == AGENT_PROTO_CODEX_RPC ||
            run->proto == AGENT_PROTO_AGY_ACP ||
            run->proto == AGENT_PROTO_CLAUDE_CTL);
}

mcode_steer_result mcode_agent_steer(mcode_agent_run *run, mcode_thread *thread,
                                     const char *message) {
    if (!run || !run->proc || !message || !message[0]) return MCODE_STEER_FAILED;

    bool has_approval = false;
    bool has_question = false;
    bool already_done = false;
    pthread_mutex_lock(&run->mu);
    has_approval = (run->approval_head != NULL);
    has_question = run->question_pending;
    already_done = run->rpc_turn_done || run->finished;
    pthread_mutex_unlock(&run->mu);

    if (already_done) {
        return MCODE_STEER_INTERRUPTING;
    }

    /* If interactive permission or question prompts are blocking the agent, clear all of them
     * first to unblock the RPC channel and redirect on the next turn. */
    if (has_approval || has_question) {
        for (int n = 0; n < 64 && has_approval; ++n) {
            mcode_agent_respond_approval(run, thread, MCODE_APPROVAL_DENY);
            pthread_mutex_lock(&run->mu);
            has_approval = (run->approval_head != NULL);
            pthread_mutex_unlock(&run->mu);
        }
        emit_simple(run, EV_APPROVAL_CLEAR, NULL);
        if (has_question) mcode_agent_respond_question(run, thread, false);
        pthread_mutex_lock(&run->mu);
        run->steer_interrupting = true;
        run->interrupt_requested_at = mcode_now_mono();
        pthread_mutex_unlock(&run->mu);
        if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
            claude_ctl_send_interrupt(run);
        } else if (run->proto == AGENT_PROTO_AGY_ACP) {
            agy_acp_send_cancel(run);
        } else if (run->proto == AGENT_PROTO_CODEX_RPC) {
            codex_rpc_send_turn_interrupt(run);
        }
        return MCODE_STEER_INTERRUPTING;
    }

    if (run->proto == AGENT_PROTO_CODEX_RPC) {
        char sid[MCODE_SESSION_MAX] = "";
        char tid[128] = "";
        bool buffered = false;
        bool buf_oom = false;

        pthread_mutex_lock(&run->mu);
        if (run->rpc_turn_done || run->finished) {
            pthread_mutex_unlock(&run->mu);
            return MCODE_STEER_INTERRUPTING;
        }
        snprintf(sid, sizeof sid, "%s", run->active_session_id);
        snprintf(tid, sizeof tid, "%s", run->active_turn_id);
        if (!tid[0]) {
            if (run->pending_live_steer_count >= run->pending_live_steer_cap) {
                int nc = run->pending_live_steer_cap ? run->pending_live_steer_cap * 2 : 4;
                char **grown = realloc(run->pending_live_steers, (size_t)nc * sizeof *grown);
                if (grown) {
                    run->pending_live_steers = grown;
                    run->pending_live_steer_cap = nc;
                } else {
                    buf_oom = true;
                }
            }
            if (!buf_oom) {
                char *dup = strdup(message);
                if (dup) {
                    run->pending_live_steers[run->pending_live_steer_count++] = dup;
                    buffered = true;
                } else {
                    buf_oom = true;
                }
            }
        }
        pthread_mutex_unlock(&run->mu);

        if (buffered) return MCODE_STEER_INJECTED;
        if (buf_oom) return MCODE_STEER_FAILED;
        if (codex_rpc_send_turn_steer(run, sid, tid, message)) {
            return MCODE_STEER_INJECTED;
        }
        return MCODE_STEER_INTERRUPTING;
    }

    if (run->proto == AGENT_PROTO_CLAUDE_CTL) {
        char steer_uuid[40] = "";
        bool oom = false;

        pthread_mutex_lock(&run->mu);
        if (run->rpc_turn_done || run->finished) {
            pthread_mutex_unlock(&run->mu);
            return MCODE_STEER_INTERRUPTING;
        }
        if (run->claude_steer_count >= run->claude_steer_cap) {
            int nc = run->claude_steer_cap ? run->claude_steer_cap * 2 : 4;
            claude_live_steer *grown = realloc(run->claude_steers, (size_t)nc * sizeof *grown);
            if (grown) {
                run->claude_steers = grown;
                run->claude_steer_cap = nc;
            } else {
                oom = true;
            }
        }
        if (!oom) {
            char *dup = strdup(message);
            if (dup) {
                claude_make_uuid_locked(run, steer_uuid);
                claude_live_steer *s = &run->claude_steers[run->claude_steer_count++];
                snprintf(s->uuid, sizeof s->uuid, "%s", steer_uuid);
                s->text = dup;
                s->started = false;
                s->completed = false;
            } else {
                oom = true;
            }
        }
        pthread_mutex_unlock(&run->mu);

        if (oom) return MCODE_STEER_FAILED;
        if (claude_ctl_send_user_msg(run, steer_uuid, "next", message)) {
            return MCODE_STEER_INJECTED;
        }
        pthread_mutex_lock(&run->mu);
        for (int i = 0; i < run->claude_steer_count; ++i) {
            if (strcmp(run->claude_steers[i].uuid, steer_uuid) == 0) {
                free(run->claude_steers[i].text);
                for (int j = i + 1; j < run->claude_steer_count; ++j) {
                    run->claude_steers[j - 1] = run->claude_steers[j];
                }
                run->claude_steer_count--;
                break;
            }
        }
        pthread_mutex_unlock(&run->mu);
        return MCODE_STEER_INTERRUPTING;
    }

    if (run->proto == AGENT_PROTO_AGY_ACP) {
        bool prompt_sent = false;
        char sid[MCODE_SESSION_MAX] = "";
        bool buffered = false;
        bool buf_oom = false;

        pthread_mutex_lock(&run->mu);
        if (run->rpc_turn_done || run->finished) {
            pthread_mutex_unlock(&run->mu);
            return MCODE_STEER_INTERRUPTING;
        }
        prompt_sent = run->rpc_prompt_sent;
        snprintf(sid, sizeof sid, "%s", run->active_session_id);
        if (!prompt_sent || !sid[0]) {
            if (run->pending_live_steer_count >= run->pending_live_steer_cap) {
                int nc = run->pending_live_steer_cap ? run->pending_live_steer_cap * 2 : 4;
                char **grown = realloc(run->pending_live_steers, (size_t)nc * sizeof *grown);
                if (grown) {
                    run->pending_live_steers = grown;
                    run->pending_live_steer_cap = nc;
                } else {
                    buf_oom = true;
                }
            }
            if (!buf_oom) {
                char *dup = strdup(message);
                if (dup) {
                    run->pending_live_steers[run->pending_live_steer_count++] = dup;
                    buffered = true;
                } else {
                    buf_oom = true;
                }
            }
        }
        pthread_mutex_unlock(&run->mu);

        if (buffered) return MCODE_STEER_INJECTED;
        if (buf_oom) return MCODE_STEER_FAILED;
        if (agy_acp_send_steer_prompt(run, sid, message)) {
            return MCODE_STEER_INJECTED;
        }
        return MCODE_STEER_INTERRUPTING;
    }

    /* Stream-only fallback */
    pthread_mutex_lock(&run->mu);
    run->steer_interrupting = true;
    run->interrupt_requested_at = -1.0;
    pthread_mutex_unlock(&run->mu);
    mcode_proc_stop(run->proc);
    return MCODE_STEER_INTERRUPTING;
}

bool mcode_agent_respond_approval(mcode_agent_run *run, mcode_thread *thread,
                                  mcode_approval_decision decision) {
    if (thread) {
        memset(&thread->pending_approval, 0, sizeof thread->pending_approval);
    }
    if (!run || !run->proc || decision == MCODE_APPROVAL_NONE) return false;

    pending_approval_item *item = NULL;
    agent_ev *next_ev = NULL;

    pthread_mutex_lock(&run->mu);
    item = run->approval_head;
    if (item) {
        run->approval_head = item->next;
        if (!run->approval_head) run->approval_tail = NULL;
        item->next = NULL;
        if (run->approval_head) {
            next_ev = approval_make_ev(run->approval_head);
        }
    }
    pthread_mutex_unlock(&run->mu);

    if (!item) {
        ev_free(next_ev);
        return false;
    }

    approval_rpc_kind kind = item->kind;
    bool id_is_str = item->rpc_id_is_str;
    char id_str[128] = "";
    snprintf(id_str, sizeof id_str, "%s", item->rpc_id_str);
    int64_t id_num = item->rpc_id_num;
    char allow_opt[64] = "";
    snprintf(allow_opt, sizeof allow_opt, "%s", item->allow_opt);
    char allow_always_opt[64] = "";
    snprintf(allow_always_opt, sizeof allow_always_opt, "%s", item->allow_always_opt);
    char deny_opt[64] = "";
    snprintf(deny_opt, sizeof deny_opt, "%s", item->deny_opt);
    bool codex_amend = item->codex_use_amendment;
    char *payload = item->payload_json;
    item->payload_json = NULL;
    approval_item_free(item);

    bool ok = false;
    bool allow = (decision == MCODE_APPROVAL_ALLOW_ONCE || decision == MCODE_APPROVAL_ALLOW_SESSION);
    bool allow_sess = (decision == MCODE_APPROVAL_ALLOW_SESSION);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc) {
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        if (kind == APPROVAL_KIND_AGY_ACP) {
            const char *opt = deny_opt[0] ? deny_opt : "deny";
            if (allow_sess && allow_always_opt[0]) opt = allow_always_opt;
            else if (allow) opt = allow_opt[0] ? allow_opt : "allow";

            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_val *out = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, out, "outcome", "selected");
            yyjson_mut_obj_add_strcpy(doc, out, "optionId", opt);
            yyjson_mut_obj_add_val(doc, res, "outcome", out);
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CLAUDE_TOOL) {
            yyjson_mut_obj_add_str(doc, root, "type", "control_response");
            yyjson_mut_val *resp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, resp, "subtype", "success");
            yyjson_mut_obj_add_strcpy(doc, resp, "request_id", id_str);
            yyjson_mut_val *inner = yyjson_mut_obj(doc);
            if (allow) {
                yyjson_mut_obj_add_str(doc, inner, "behavior", "allow");
                yyjson_mut_val *raw = yyjson_mut_rawcpy(doc, (payload && payload[0]) ? payload : "{}");
                if (raw) yyjson_mut_obj_add_val(doc, inner, "updatedInput", raw);
            } else {
                yyjson_mut_obj_add_str(doc, inner, "behavior", "deny");
                yyjson_mut_obj_add_str(doc, inner, "message", "User denied permission.");
            }
            yyjson_mut_obj_add_val(doc, resp, "response", inner);
            yyjson_mut_obj_add_val(doc, root, "response", resp);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CODEX_CMD) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            if (allow_sess && codex_amend && payload && payload[0]) {
                yyjson_mut_val *raw = yyjson_mut_rawcpy(doc, payload);
                if (raw) yyjson_mut_obj_add_val(doc, res, "decision", raw);
                else yyjson_mut_obj_add_str(doc, res, "decision", "accept");
            } else if (allow_sess) {
                yyjson_mut_obj_add_str(doc, res, "decision", "acceptForSession");
            } else if (allow) {
                yyjson_mut_obj_add_str(doc, res, "decision", "accept");
            } else {
                yyjson_mut_obj_add_str(doc, res, "decision", "decline");
            }
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CODEX_FILE) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, res, "decision",
                                   allow_sess ? "acceptForSession"
                                              : (allow ? "accept" : "decline"));
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CODEX_PERMS) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            if (allow && payload && payload[0]) {
                yyjson_mut_val *raw = yyjson_mut_rawcpy(doc, payload);
                if (raw) yyjson_mut_obj_add_val(doc, res, "permissions", raw);
                else yyjson_mut_obj_add_val(doc, res, "permissions", yyjson_mut_obj(doc));
            } else {
                yyjson_mut_obj_add_val(doc, res, "permissions", yyjson_mut_obj(doc));
            }
            yyjson_mut_obj_add_str(doc, res, "scope", allow_sess ? "session" : "turn");
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CODEX_LEGACY) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, res, "decision",
                                   allow_sess ? "approved_for_session"
                                              : (allow ? "approved" : "denied"));
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == APPROVAL_KIND_CODEX_ELICIT) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, res, "action", allow ? "accept" : "decline");
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        }

        yyjson_mut_doc_free(doc);
    }

    free(payload);
    if (next_ev) {
        push_ev(run, next_ev);
        if (thread) {
            snprintf(thread->status_text, sizeof thread->status_text, "Waiting for approval...");
        }
    } else if (ok && thread) {
        snprintf(thread->status_text, sizeof thread->status_text, "%s",
                 allow ? "Approved — continuing..." : "Denied — continuing...");
    }
    return ok;
}

bool mcode_agent_respond_question(mcode_agent_run *run, mcode_thread *thread, bool submit) {
    mcode_pending_question snap;
    memset(&snap, 0, sizeof snap);
    if (thread) {
        snap = thread->pending_question;
        memset(&thread->pending_question, 0, sizeof thread->pending_question);
    }
    if (!run || !run->proc) return false;

    bool pending = false;
    question_rpc_kind kind = QUESTION_KIND_NONE;
    bool id_is_str = false;
    char id_str[128] = "";
    int64_t id_num = 0;
    char *payload = NULL;

    pthread_mutex_lock(&run->mu);
    pending = run->question_pending;
    if (pending) {
        run->question_pending = false;
        kind = run->question_kind;
        id_is_str = run->question_rpc_id_is_str;
        snprintf(id_str, sizeof id_str, "%s", run->question_rpc_id_str);
        id_num = run->question_rpc_id_num;
        payload = run->question_payload_json;
        run->question_payload_json = NULL;
    }
    pthread_mutex_unlock(&run->mu);

    if (!pending) {
        free(payload);
        return false;
    }

    bool ok = false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc) {
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        if (kind == QUESTION_KIND_CODEX) {
            yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
            rpc_add_saved_id(doc, root, id_is_str, id_str, id_num);
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_val *ans_map = yyjson_mut_obj(doc);
            if (submit) {
                for (int i = 0; i < snap.count; ++i) {
                    const mcode_question_item *qi = &snap.items[i];
                    yyjson_mut_val *entry = yyjson_mut_obj(doc);
                    yyjson_mut_val *arr = yyjson_mut_arr(doc);
                    for (int oi = 0; oi < qi->option_count; ++oi) {
                        if (qi->selected_mask & (1u << oi)) {
                            yyjson_mut_arr_add_strcpy(doc, arr, qi->options[oi].label);
                        }
                    }
                    const char *ot = qi->other_text;
                    while (*ot == ' ' || *ot == '\t') ++ot;
                    if (ot[0]) {
                        yyjson_mut_arr_add_strcpy(doc, arr, ot);
                    }
                    yyjson_mut_obj_add_val(doc, entry, "answers", arr);
                    yyjson_mut_val *qk = yyjson_mut_strcpy(doc, qi->id[0] ? qi->id : "q");
                    if (qk) yyjson_mut_obj_put(ans_map, qk, entry);
                }
            }
            yyjson_mut_obj_add_val(doc, res, "answers", ans_map);
            yyjson_mut_obj_add_val(doc, root, "result", res);
            ok = send_json_doc(run->proc, doc);
        } else if (kind == QUESTION_KIND_CLAUDE) {
            yyjson_mut_obj_add_str(doc, root, "type", "control_response");
            yyjson_mut_val *resp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, resp, "subtype", "success");
            yyjson_mut_obj_add_strcpy(doc, resp, "request_id", id_str);
            yyjson_mut_val *inner = yyjson_mut_obj(doc);
            if (submit) {
                yyjson_mut_obj_add_str(doc, inner, "behavior", "allow");
                yyjson_mut_val *upd = yyjson_mut_obj(doc);
                /* Preserve original questions array from Claude's tool input */
                if (payload && payload[0]) {
                    yyjson_doc *pdoc = yyjson_read(payload, strlen(payload), 0);
                    if (pdoc) {
                        yyjson_val *proot = yyjson_doc_get_root(pdoc);
                        yyjson_val *qarr = yyjson_is_obj(proot) ? yyjson_obj_get(proot, "questions") : NULL;
                        if (qarr) {
                            yyjson_mut_val *qcopy = yyjson_val_mut_copy(doc, qarr);
                            if (qcopy) yyjson_mut_obj_add_val(doc, upd, "questions", qcopy);
                        }
                        yyjson_doc_free(pdoc);
                    }
                }
                yyjson_mut_val *ans_map = yyjson_mut_obj(doc);
                for (int i = 0; i < snap.count; ++i) {
                    const mcode_question_item *qi = &snap.items[i];
                    mcode_str joined;
                    mcode_str_init(&joined);
                    for (int oi = 0; oi < qi->option_count; ++oi) {
                        if (qi->selected_mask & (1u << oi)) {
                            if (joined.len > 0) mcode_str_append_cstr(&joined, ", ");
                            mcode_str_append_cstr(&joined, qi->options[oi].label);
                        }
                    }
                    const char *ot = qi->other_text;
                    while (*ot == ' ' || *ot == '\t') ++ot;
                    if (ot[0]) {
                        if (joined.len > 0) mcode_str_append_cstr(&joined, ", ");
                        mcode_str_append_cstr(&joined, ot);
                    }
                    yyjson_mut_val *k = yyjson_mut_strcpy(doc, qi->question);
                    yyjson_mut_val *v = yyjson_mut_strcpy(doc, mcode_str_cstr(&joined));
                    if (k && v) yyjson_mut_obj_put(ans_map, k, v);
                    mcode_str_free(&joined);
                }
                yyjson_mut_obj_add_val(doc, upd, "answers", ans_map);
                yyjson_mut_obj_add_val(doc, inner, "updatedInput", upd);
            } else {
                yyjson_mut_obj_add_str(doc, inner, "behavior", "deny");
                yyjson_mut_obj_add_str(doc, inner, "message", "User dismissed question.");
            }
            yyjson_mut_obj_add_val(doc, resp, "response", inner);
            yyjson_mut_obj_add_val(doc, root, "response", resp);
            ok = send_json_doc(run->proc, doc);
        }

        yyjson_mut_doc_free(doc);
    }

    free(payload);
    if (ok && thread) {
        snprintf(thread->status_text, sizeof thread->status_text, "%s",
                 submit ? "Answer sent — continuing..." : "Question dismissed — continuing...");
    }
    return ok;
}

void mcode_agent_destroy_timeout(mcode_agent_run *run, int grace_ms) {
    if (!run) return;
    if (run->proc) {
        mcode_proc_destroy_timeout(run->proc, grace_ms);
        run->proc = NULL;
    }
    pthread_mutex_lock(&run->mu);
    agent_ev *cur = run->head;
    run->head = run->tail = NULL;
    approval_queue_clear_locked(run);
    free(run->question_payload_json);
    run->question_payload_json = NULL;
    for (int i = 0; i < run->pending_live_steer_count; ++i) {
        free(run->pending_live_steers[i]);
    }
    free(run->pending_live_steers);
    run->pending_live_steers = NULL;
    run->pending_live_steer_count = 0;
    run->pending_live_steer_cap = 0;
    for (int i = 0; i < run->inflight_steer_count; ++i) {
        free(run->inflight_steer_texts[i]);
        run->inflight_steer_texts[i] = NULL;
    }
    run->inflight_steer_count = 0;
    for (int i = 0; i < run->claude_steer_count; ++i) {
        free(run->claude_steers[i].text);
    }
    free(run->claude_steers);
    run->claude_steers = NULL;
    run->claude_steer_count = 0;
    run->claude_steer_cap = 0;
    pthread_mutex_unlock(&run->mu);
    while (cur) {
        agent_ev *next = cur->next;
        ev_free(cur);
        cur = next;
    }
    free(run->init_prompt);
    mcode_str_free(&run->out_line);
    mcode_str_free(&run->err_line);
    mcode_str_free(&run->stderr_tail);
    pthread_mutex_destroy(&run->mu);
    free(run);
}

void mcode_agent_destroy(mcode_agent_run *run) {
    mcode_agent_destroy_timeout(run, 150);
}

/* ---- Asynchronous thread title generation ---- */

struct mcode_title_run {
    app_agent_kind agent;
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str stdout_buf;
    mcode_str stderr_buf;
    double started_mono;
    bool timed_out;
    bool finished;
    bool killed;
    int exit_code;
};

static bool starts_with_icase(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        ++s;
        ++prefix;
    }
    return true;
}

bool mcode_agent_sanitize_title(const char *raw, char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!raw || !raw[0]) return false;

    /* Pick the first non-empty line, stripping ANSI codes and spinner \r segments. */
    char clean[512];
    clean[0] = '\0';
    const char *p = raw;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

        const char *seg = p;
        size_t seg_len = line_len;
        for (size_t i = 0; i < line_len; ++i) {
            if (p[i] == '\r') {
                seg = p + i + 1;
                seg_len = line_len - (i + 1);
            }
        }

        char tmp[512];
        strip_ansi_into(seg, seg_len, tmp, sizeof tmp);
        char *s = tmp;
        while (*s && isspace((unsigned char)*s)) ++s;
        if (*s) {
            snprintf(clean, sizeof clean, "%s", s);
            break;
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!clean[0]) return false;

    char *s = clean;
    size_t slen = strlen(s);
    while (slen > 0 && isspace((unsigned char)s[slen - 1])) s[--slen] = '\0';

    /* Strip leading markdown headings/bullets and common "Title:" labels. */
    bool stripped = true;
    while (stripped && slen > 0) {
        stripped = false;
        while (slen > 0 && isspace((unsigned char)*s)) { ++s; --slen; stripped = true; }
        if (slen > 0 && *s == '#') {
            while (slen > 0 && *s == '#') { ++s; --slen; }
            while (slen > 0 && isspace((unsigned char)*s)) { ++s; --slen; }
            stripped = true;
        } else if (slen >= 2 && (*s == '-' || *s == '*') && isspace((unsigned char)s[1])) {
            s += 2;
            slen -= 2;
            stripped = true;
        }
        static const char *const k_labels[] = { "thread title:", "title:", "summary:" };
        for (size_t i = 0; i < sizeof k_labels / sizeof k_labels[0]; ++i) {
            size_t llen = strlen(k_labels[i]);
            if (slen > llen && starts_with_icase(s, k_labels[i])) {
                s += llen;
                slen -= llen;
                stripped = true;
                break;
            }
        }
        while (slen > 0 && isspace((unsigned char)s[slen - 1])) { s[--slen] = '\0'; stripped = true; }

        /* Unwrap symmetric quotes, backticks, or markdown emphasis. */
        if (slen >= 4 && s[0] == '*' && s[1] == '*' && s[slen - 2] == '*' && s[slen - 1] == '*') {
            s += 2;
            slen -= 4;
            s[slen] = '\0';
            stripped = true;
        } else if (slen >= 2 &&
                   ((s[0] == '"' && s[slen - 1] == '"') ||
                    (s[0] == '\'' && s[slen - 1] == '\'') ||
                    (s[0] == '`' && s[slen - 1] == '`') ||
                    (s[0] == '*' && s[slen - 1] == '*') ||
                    (s[0] == '_' && s[slen - 1] == '_'))) {
            ++s;
            slen -= 2;
            s[slen] = '\0';
            stripped = true;
        }
    }

    /* Collapse internal whitespace and clamp to a sidebar-friendly length. */
    size_t max_len = cap - 1;
    if (max_len > 72) max_len = 72;
    size_t w = 0;
    bool prev_space = false;
    for (size_t i = 0; i < slen && w < max_len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (isspace(ch)) {
            if (w == 0 || prev_space) continue;
            out[w++] = ' ';
            prev_space = true;
        } else if (ch < 0x20 || ch == 0x7f) {
            continue;
        } else {
            out[w++] = (char)ch;
            prev_space = false;
        }
    }
    /* Avoid splitting a multi-byte UTF-8 sequence at max_len. */
    if (w == max_len && w > 0) {
        size_t tail = w;
        while (tail > 0 && ((unsigned char)out[tail - 1] & 0xC0) == 0x80) --tail;
        if (tail > 0) {
            unsigned char lead = (unsigned char)out[tail - 1];
            size_t need = 1;
            if ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            if (w - (tail - 1) < need) w = tail - 1;
        }
    }

    /* Strip trailing punctuation and whitespace. */
    while (w > 0 && (isspace((unsigned char)out[w - 1]) ||
                     out[w - 1] == '.' || out[w - 1] == ':' ||
                     out[w - 1] == ';' || out[w - 1] == ',')) {
        --w;
    }
    out[w] = '\0';
    return w >= 2;
}

static void extract_content_text_blocks(yyjson_val *msg, mcode_str *dst) {
    if (!yyjson_is_obj(msg) || !dst) return;
    yyjson_val *content = yyjson_obj_get(msg, "content");
    if (!yyjson_is_arr(content)) return;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(content, idx, max, item) {
        const char *it = jstr(item, "type");
        if (it && strcmp(it, "text") == 0) {
            const char *txt = jstr(item, "text");
            if (txt && txt[0]) mcode_str_append_cstr(dst, txt);
        }
    }
}

static bool extract_one_shot_response_text(app_agent_kind agent, const char *raw, size_t len,
                                           mcode_str *out_text) {
    if (!out_text) return false;
    mcode_str_clear(out_text);
    if (!raw || len == 0) return false;

    mcode_str extracted;
    mcode_str plain;
    mcode_str_init(&extracted);
    mcode_str_init(&plain);
    bool saw_json = false;
    bool saw_error = false;

    const char *p = raw;
    const char *end = raw + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

        const char *trim = p;
        size_t tlen = line_len;
        while (tlen > 0 && isspace((unsigned char)*trim)) { ++trim; --tlen; }
        while (tlen > 0 && isspace((unsigned char)trim[tlen - 1])) --tlen;

        bool handled_json = false;
        if (tlen >= 2 && trim[0] == '{') {
            yyjson_doc *doc = yyjson_read(trim, tlen, 0);
            if (doc) {
                yyjson_val *root = yyjson_doc_get_root(doc);
                if (yyjson_is_obj(root)) {
                    saw_json = true;
                    handled_json = true;

                    if (agent == APP_AGENT_CLAUDE) {
                        const char *type = jstr(root, "type");
                        if (type && strcmp(type, "result") == 0) {
                            yyjson_val *err_v = yyjson_obj_get(root, "is_error");
                            bool is_err = yyjson_is_bool(err_v) && yyjson_get_bool(err_v);
                            if (is_err) {
                                saw_error = true;
                                mcode_str_clear(&extracted);
                            } else {
                                const char *res = jstr(root, "result");
                                if (res && res[0]) mcode_str_set(&extracted, res);
                            }
                        } else if (type && strcmp(type, "stream_event") == 0) {
                            yyjson_val *ev = yyjson_obj_get(root, "event");
                            yyjson_val *delta = yyjson_is_obj(ev) ? yyjson_obj_get(ev, "delta") : NULL;
                            const char *dt = jstr(delta, "type");
                            if (dt && strcmp(dt, "text_delta") == 0) {
                                const char *txt = jstr(delta, "text");
                                if (txt && txt[0]) mcode_str_append_cstr(&extracted, txt);
                            }
                        } else if (type && strcmp(type, "assistant") == 0 && extracted.len == 0) {
                            extract_content_text_blocks(yyjson_obj_get(root, "message"), &extracted);
                        }
                    } else if (agent == APP_AGENT_CODEX) {
                        const char *type = jstr(root, "type");
                        if (type && (strcmp(type, "error") == 0 || strcmp(type, "turn.failed") == 0)) {
                            saw_error = true;
                        } else if (type && strcmp(type, "item.completed") == 0) {
                            yyjson_val *item = yyjson_obj_get(root, "item");
                            const char *itype = jstr(item, "type");
                            if (itype && (strcmp(itype, "agent_message") == 0 ||
                                          strcmp(itype, "message") == 0)) {
                                const char *txt = jstr(item, "text");
                                if (txt && txt[0]) mcode_str_set(&extracted, txt);
                            }
                        }
                    } else { /* APP_AGENT_AGY */
                        const char *event = jstr(root, "event");
                        const char *type = jstr(root, "type");
                        if (event && strcmp(event, "init") == 0) {
                            /* Ignore stream init envelope */
                        } else if (event && strcmp(event, "step_update") == 0) {
                            yyjson_val *su = yyjson_obj_get(root, "step_update");
                            const char *stype = jstr(su, "step_type");
                            if (stype && strcmp(stype, "agent_response") == 0) {
                                const char *td = jstr(su, "text_delta");
                                if (td && td[0]) mcode_str_append_cstr(&extracted, td);
                            }
                        } else if (event && strcmp(event, "result") == 0 &&
                                   yyjson_is_obj(yyjson_obj_get(root, "result"))) {
                            yyjson_val *res_obj = yyjson_obj_get(root, "result");
                            const char *status = jstr(res_obj, "status");
                            const char *err_s = jstr(res_obj, "error");
                            if ((err_s && err_s[0]) ||
                                (status && status[0] && strcmp(status, "SUCCESS") != 0)) {
                                saw_error = true;
                                mcode_str_clear(&extracted);
                            } else {
                                const char *resp = jstr(res_obj, "response");
                                if (resp && resp[0]) mcode_str_set(&extracted, resp);
                            }
                        } else if (!event && !type &&
                                   (yyjson_obj_get(root, "response") || yyjson_obj_get(root, "status"))) {
                            const char *status = jstr(root, "status");
                            const char *err_s = jstr(root, "error");
                            if ((err_s && err_s[0]) ||
                                (status && status[0] && strcmp(status, "SUCCESS") != 0)) {
                                saw_error = true;
                                mcode_str_clear(&extracted);
                            } else {
                                const char *resp = jstr(root, "response");
                                if (resp && resp[0]) mcode_str_set(&extracted, resp);
                            }
                        } else {
                            if (!type) type = event;
                            yyjson_val *err_v = yyjson_obj_get(root, "is_error");
                            bool is_err = yyjson_is_bool(err_v) && yyjson_get_bool(err_v);
                            if (is_err || (type && (strcmp(type, "error") == 0 || strcmp(type, "failed") == 0))) {
                                saw_error = true;
                            } else if (type && (strcmp(type, "thinking") == 0 ||
                                                strcmp(type, "reasoning") == 0 ||
                                                strcmp(type, "thought") == 0 ||
                                                strcmp(type, "tool_use") == 0 ||
                                                strcmp(type, "tool_call") == 0 ||
                                                strcmp(type, "function_call") == 0 ||
                                                strcmp(type, "tool_result") == 0 ||
                                                strcmp(type, "function_response") == 0 ||
                                                strcmp(type, "system") == 0)) {
                                /* Ignore non-text events */
                            } else {
                                const char *delta = jstr(root, "delta");
                                const char *txt = jstr(root, "text");
                                const char *content = jstr(root, "content");
                                const char *res = jstr(root, "result");
                                if (delta && delta[0]) {
                                    mcode_str_append_cstr(&extracted, delta);
                                } else if (txt && txt[0]) {
                                    mcode_str_append_cstr(&extracted, txt);
                                } else if (content && content[0]) {
                                    mcode_str_append_cstr(&extracted, content);
                                } else if (res && res[0] && extracted.len == 0) {
                                    mcode_str_set(&extracted, res);
                                } else if (extracted.len == 0) {
                                    extract_content_text_blocks(yyjson_obj_get(root, "message"), &extracted);
                                }
                            }
                        }
                    }
                }
                yyjson_doc_free(doc);
            }
        }

        if (!handled_json && !saw_json && tlen > 0) {
            if (plain.len > 0) mcode_str_append_cstr(&plain, "\n");
            mcode_str_append(&plain, trim, tlen);
        }

        if (!nl) break;
        p = nl + 1;
    }

    bool ok = false;
    if (saw_json) {
        if (extracted.len > 0 && !saw_error) {
            mcode_str_set(out_text, mcode_str_cstr(&extracted));
            ok = true;
        }
    } else if (plain.len > 0) {
        mcode_str_set(out_text, mcode_str_cstr(&plain));
        ok = true;
    }

    mcode_str_free(&extracted);
    mcode_str_free(&plain);
    return ok;
}

bool mcode_agent_parse_title_response(app_agent_kind agent, const char *raw, size_t len,
                                      char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    mcode_str text;
    mcode_str_init(&text);
    bool ok = extract_one_shot_response_text(agent, raw, len, &text) &&
              mcode_agent_sanitize_title(mcode_str_cstr(&text), out, cap);
    mcode_str_free(&text);
    return ok;
}

bool mcode_agent_sanitize_commit_msg(const char *raw, char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!raw || !raw[0]) return false;

    static const char *const k_labels[] = {
        "suggested commit message:",
        "git commit message:",
        "commit message:",
        "message:",
        "subject:",
    };

    char clean[512];
    clean[0] = '\0';
    const char *p = raw;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

        const char *seg = p;
        size_t seg_len = line_len;
        for (size_t i = 0; i < line_len; ++i) {
            if (p[i] == '\r') {
                seg = p + i + 1;
                seg_len = line_len - (i + 1);
            }
        }

        char tmp[512];
        strip_ansi_into(seg, seg_len, tmp, sizeof tmp);
        char *s = tmp;
        while (*s && isspace((unsigned char)*s)) ++s;
        size_t sl = strlen(s);
        while (sl > 0 && isspace((unsigned char)s[sl - 1])) s[--sl] = '\0';

        /* Skip empty lines and markdown code-fence delimiters (``` or ~~~). */
        if (!*s || strncmp(s, "```", 3) == 0 || strncmp(s, "~~~", 3) == 0) {
            if (!nl) break;
            p = nl + 1;
            continue;
        }

        /* Skip standalone label lines like "Commit message:" with text on next line. */
        bool label_only = false;
        for (size_t i = 0; i < sizeof k_labels / sizeof k_labels[0]; ++i) {
            size_t llen = strlen(k_labels[i]);
            if (sl == llen && starts_with_icase(s, k_labels[i])) {
                label_only = true;
                break;
            }
        }
        if (label_only) {
            if (!nl) break;
            p = nl + 1;
            continue;
        }

        snprintf(clean, sizeof clean, "%s", s);
        break;
    }
    if (!clean[0]) return false;

    char *s = clean;
    size_t slen = strlen(s);
    while (slen > 0 && isspace((unsigned char)s[slen - 1])) s[--slen] = '\0';

    bool stripped = true;
    while (stripped && slen > 0) {
        stripped = false;
        while (slen > 0 && isspace((unsigned char)*s)) { ++s; --slen; stripped = true; }
        if (slen > 0 && *s == '#') {
            while (slen > 0 && *s == '#') { ++s; --slen; }
            while (slen > 0 && isspace((unsigned char)*s)) { ++s; --slen; }
            stripped = true;
        } else if (slen >= 2 && (*s == '-' || *s == '*') && isspace((unsigned char)s[1])) {
            s += 2;
            slen -= 2;
            stripped = true;
        }
        for (size_t i = 0; i < sizeof k_labels / sizeof k_labels[0]; ++i) {
            size_t llen = strlen(k_labels[i]);
            if (slen > llen && starts_with_icase(s, k_labels[i])) {
                s += llen;
                slen -= llen;
                stripped = true;
                break;
            }
        }
        while (slen > 0 && isspace((unsigned char)s[slen - 1])) { s[--slen] = '\0'; stripped = true; }

        if (slen >= 4 && s[0] == '*' && s[1] == '*' && s[slen - 2] == '*' && s[slen - 1] == '*') {
            s += 2;
            slen -= 4;
            s[slen] = '\0';
            stripped = true;
        } else if (slen >= 2 &&
                   ((s[0] == '"' && s[slen - 1] == '"') ||
                    (s[0] == '\'' && s[slen - 1] == '\'') ||
                    (s[0] == '`' && s[slen - 1] == '`') ||
                    (s[0] == '*' && s[slen - 1] == '*') ||
                    (s[0] == '_' && s[slen - 1] == '_'))) {
            ++s;
            slen -= 2;
            s[slen] = '\0';
            stripped = true;
        }
    }

    /* Collapse internal whitespace and clamp to a clean single-line commit subject. */
    size_t max_len = cap - 1;
    if (max_len > 140) max_len = 140;
    size_t w = 0;
    bool prev_space = false;
    for (size_t i = 0; i < slen && w < max_len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (isspace(ch)) {
            if (w == 0 || prev_space) continue;
            out[w++] = ' ';
            prev_space = true;
        } else if (ch < 0x20 || ch == 0x7f) {
            continue;
        } else {
            out[w++] = (char)ch;
            prev_space = false;
        }
    }
    if (w == max_len && w > 0) {
        size_t tail = w;
        while (tail > 0 && ((unsigned char)out[tail - 1] & 0xC0) == 0x80) --tail;
        if (tail > 0) {
            unsigned char lead = (unsigned char)out[tail - 1];
            size_t need = 1;
            if ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            if (w - (tail - 1) < need) w = tail - 1;
        }
    }

    /* Strip trailing period / whitespace (git commit subject convention). */
    while (w > 0 && (isspace((unsigned char)out[w - 1]) ||
                     out[w - 1] == '.' || out[w - 1] == ';' || out[w - 1] == ',')) {
        --w;
    }
    out[w] = '\0';
    return w >= 3;
}

bool mcode_agent_parse_commit_msg_response(app_agent_kind agent, const char *raw, size_t len,
                                           char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    mcode_str text;
    mcode_str_init(&text);
    bool ok = extract_one_shot_response_text(agent, raw, len, &text) &&
              mcode_agent_sanitize_commit_msg(mcode_str_cstr(&text), out, cap);
    mcode_str_free(&text);
    return ok;
}

static mcode_proc *spawn_one_shot_prompt(app_agent_kind agent, const char *bin,
                                         const char *model, const char *workdir,
                                         const char *prompt, const char *agy_timeout_s,
                                         mcode_proc_bytes_fn on_bytes,
                                         mcode_proc_exit_fn on_exit, void *user) {
    const char *argv[24];
    int n = 0;

    if (agent == APP_AGENT_CLAUDE) {
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = prompt;
        argv[n++] = "--output-format";
        argv[n++] = "json";
        argv[n++] = "--no-session-persistence";
        argv[n++] = "--tools";
        argv[n++] = "";
        if (model && model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
    } else if (agent == APP_AGENT_CODEX) {
        argv[n++] = bin;
        argv[n++] = "exec";
        argv[n++] = "--json";
        argv[n++] = "--ephemeral";
        argv[n++] = "--skip-git-repo-check";
        argv[n++] = "-s";
        argv[n++] = "read-only";
        argv[n++] = "-c";
        argv[n++] = "model_reasoning_effort=\"low\"";
        if (workdir && workdir[0]) {
            argv[n++] = "--cd";
            argv[n++] = workdir;
        }
        if (model && model[0]) {
            argv[n++] = "-m";
            argv[n++] = model;
        }
        argv[n++] = prompt;
    } else { /* APP_AGENT_AGY */
        /* Deny all tool actions in headless one-shot mode so the agent cannot
         * trigger an interactive permission prompt (which headless mode soft-denies
         * by canceling the turn with an empty response and exit code 0) or execute
         * side effects when summarizing prompts or diffs. */
        static const char k_agy_one_shot_settings[] =
            "{\"permissions\":{\"deny\":["
            "\"command(*)\",\"read_file(*)\",\"write_file(*)\","
            "\"read_url(*)\",\"execute_url(*)\",\"mcp(*)\""
            "]}}";
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = prompt;
        argv[n++] = "--output-format";
        argv[n++] = "json";
        argv[n++] = "--disable-slash-commands";
        argv[n++] = "--settings";
        argv[n++] = k_agy_one_shot_settings;
        argv[n++] = "--print-timeout";
        argv[n++] = (agy_timeout_s && agy_timeout_s[0]) ? agy_timeout_s : "30s";
        if (model && model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
    }
    argv[n] = NULL;

    return mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = (workdir && workdir[0]) ? workdir : NULL,
        .merge_stderr = false,
        .on_bytes = on_bytes,
        .on_exit = on_exit,
        .user = user,
    });
}

static void copy_first_clean_line(const char *raw, size_t len, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!raw || len == 0) return;

    const char *p = raw;
    const char *end = raw + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

        const char *seg = p;
        size_t seg_len = line_len;
        for (size_t i = 0; i < line_len; ++i) {
            if (p[i] == '\r') {
                seg = p + i + 1;
                seg_len = line_len - (i + 1);
            }
        }

        char tmp[256];
        strip_ansi_into(seg, seg_len, tmp, sizeof tmp);
        char *s = tmp;
        while (*s && isspace((unsigned char)*s)) ++s;
        size_t sl = strlen(s);
        while (sl > 0 && isspace((unsigned char)s[sl - 1])) s[--sl] = '\0';
        if (sl > 0) {
            snprintf(out, cap, "%s", s);
            return;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void summarize_one_shot_failure(const mcode_str *stdout_buf,
                                       const mcode_str *stderr_buf,
                                       char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    if (stdout_buf && stdout_buf->len > 0) {
        const char *p = mcode_str_cstr(stdout_buf);
        const char *end = p + stdout_buf->len;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

            const char *trim = p;
            size_t tlen = line_len;
            while (tlen > 0 && isspace((unsigned char)*trim)) { ++trim; --tlen; }
            while (tlen > 0 && isspace((unsigned char)trim[tlen - 1])) --tlen;
            if (tlen >= 2 && trim[0] == '{') {
                yyjson_doc *doc = yyjson_read(trim, tlen, 0);
                if (doc) {
                    yyjson_val *root = yyjson_doc_get_root(doc);
                    if (yyjson_is_obj(root)) {
                        yyjson_val *res = yyjson_obj_get(root, "result");
                        yyjson_val *src = yyjson_is_obj(res) ? res : root;
                        yyjson_val *err_v = yyjson_obj_get(root, "is_error");
                        bool is_err = yyjson_is_bool(err_v) && yyjson_get_bool(err_v);
                        const char *err_s = jstr(src, "error");
                        if ((!err_s || !err_s[0]) && is_err) err_s = jstr(root, "result");
                        if (!err_s || !err_s[0]) err_s = jstr(root, "message");
                        if (err_s && err_s[0]) {
                            copy_first_clean_line(err_s, strlen(err_s), out, cap);
                        } else {
                            yyjson_val *denied = yyjson_obj_get(src, "denied_actions");
                            if (yyjson_is_arr(denied) && yyjson_arr_size(denied) > 0) {
                                yyjson_val *first = yyjson_arr_get_first(denied);
                                const char *disp = jstr(first, "display_name");
                                const char *act = jstr(first, "action");
                                snprintf(out, cap, "denied tool action: %s",
                                         (disp && disp[0]) ? disp
                                                           : ((act && act[0]) ? act : "unknown"));
                            } else {
                                const char *status = jstr(src, "status");
                                if (status && status[0] && strcmp(status, "SUCCESS") != 0) {
                                    snprintf(out, cap, "status=%s", status);
                                }
                            }
                        }
                    }
                    yyjson_doc_free(doc);
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
    }

    if (!out[0] && stderr_buf && stderr_buf->len > 0) {
        copy_first_clean_line(mcode_str_cstr(stderr_buf), stderr_buf->len, out, cap);
    }
    if (!out[0]) {
        if (!stdout_buf || stdout_buf->len == 0) {
            snprintf(out, cap, "empty stdout");
        } else {
            snprintf(out, cap, "unparseable output (%zu bytes)", stdout_buf->len);
        }
    }
}

static void on_title_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    mcode_title_run *run = (mcode_title_run *)user;
    pthread_mutex_lock(&run->mu);
    if (is_stderr) {
        if (run->stderr_buf.len < 4 * 1024) {
            mcode_str_append(&run->stderr_buf, data, len);
        }
    } else if (run->stdout_buf.len < 64 * 1024) {
        mcode_str_append(&run->stdout_buf, data, len);
    }
    pthread_mutex_unlock(&run->mu);
}

static void on_title_exit(int exit_code, bool killed, void *user) {
    mcode_title_run *run = (mcode_title_run *)user;
    pthread_mutex_lock(&run->mu);
    run->exit_code = exit_code;
    run->killed = killed;
    run->finished = true;
    pthread_mutex_unlock(&run->mu);
}

mcode_title_run *mcode_agent_start_title(const mcode_thread *thread,
                                         const app_settings *settings,
                                         const char *prompt) {
    if (!thread || !prompt || !prompt[0]) return NULL;

    mcode_cli_status status;
    mcode_agent_detect_clis(settings, &status);
    if (!status.available[thread->agent]) return NULL;

    /* Cap the user prompt snippet sent for title generation (UTF-8 safe). */
    size_t plen = strlen(prompt);
    if (plen > 1000) {
        plen = 1000;
        while (plen > 0 && ((unsigned char)prompt[plen] & 0xC0) == 0x80) --plen;
    }

    mcode_str title_prompt;
    mcode_str_init(&title_prompt);
    mcode_str_append_cstr(&title_prompt,
        "Generate a concise 3 to 6 word title summarizing this coding request. "
        "Use ONLY the request text below; do not run commands, read files, or call any tools. "
        "Reply with ONLY the title text: no quotes, no markdown, no prefixes, "
        "no explanation, and no trailing punctuation.\n\nRequest:\n");
    mcode_str_append(&title_prompt, prompt, plen);

    const char *bin = status.resolved_path[thread->agent];
    const char *model = mcode_agent_effective_model(thread, settings);

    mcode_title_run *run = calloc(1, sizeof *run);
    if (!run) {
        mcode_str_free(&title_prompt);
        return NULL;
    }
    run->agent = thread->agent;
    run->started_mono = mcode_now_mono();
    mcode_str_init(&run->stdout_buf);
    mcode_str_init(&run->stderr_buf);
    pthread_mutex_init(&run->mu, NULL);

    run->proc = spawn_one_shot_prompt(thread->agent, bin, model, thread->workdir,
                                      mcode_str_cstr(&title_prompt), "30s",
                                      on_title_bytes, on_title_exit, run);
    mcode_str_free(&title_prompt);

    if (!run->proc) {
        mcode_str_free(&run->stdout_buf);
        mcode_str_free(&run->stderr_buf);
        pthread_mutex_destroy(&run->mu);
        free(run);
        return NULL;
    }

    app_log_info("Requested thread title from %s", mcode_agent_label(thread->agent));
    return run;
}

bool mcode_agent_poll_title(mcode_title_run *run, char *out_title, size_t cap, bool *out_ok) {
    if (out_title && cap > 0) out_title[0] = '\0';
    if (out_ok) *out_ok = false;
    if (!run) return true;

    pthread_mutex_lock(&run->mu);
    bool finished = run->finished;
    pthread_mutex_unlock(&run->mu);

    if (!finished) {
        if (!run->timed_out && (mcode_now_mono() - run->started_mono) > 45.0) {
            run->timed_out = true;
            if (run->proc) mcode_proc_stop(run->proc);
        }
        return false;
    }

    bool ok = false;
    if (!run->killed && run->exit_code == 0 && run->stdout_buf.len > 0) {
        ok = mcode_agent_parse_title_response(run->agent,
                                              mcode_str_cstr(&run->stdout_buf),
                                              run->stdout_buf.len,
                                              out_title, cap);
    }
    if (!ok && !run->killed) {
        char reason[256] = "";
        summarize_one_shot_failure(&run->stdout_buf, &run->stderr_buf, reason, sizeof reason);
        app_log_warn("Title generation via %s did not produce a title (exit=%d%s%s)",
                     mcode_agent_label(run->agent), run->exit_code,
                     reason[0] ? ", " : "", reason);
    }
    if (out_ok) *out_ok = ok;
    return true;
}

void mcode_agent_title_stop(mcode_title_run *run) {
    if (!run || !run->proc) return;
    mcode_proc_stop(run->proc);
}

void mcode_agent_title_kill(mcode_title_run *run) {
    if (!run || !run->proc) return;
    mcode_proc_kill(run->proc);
}

void mcode_agent_title_destroy(mcode_title_run *run) {
    if (!run) return;
    if (run->proc) {
        mcode_proc_destroy(run->proc);
        run->proc = NULL;
    }
    mcode_str_free(&run->stdout_buf);
    mcode_str_free(&run->stderr_buf);
    pthread_mutex_destroy(&run->mu);
    free(run);
}

/* ---- Asynchronous commit message generation ---- */

struct mcode_commit_msg_run {
    app_agent_kind agent;
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str stdout_buf;
    mcode_str stderr_buf;
    double started_mono;
    bool timed_out;
    bool finished;
    bool killed;
    int exit_code;
};

static void on_commit_msg_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    mcode_commit_msg_run *run = (mcode_commit_msg_run *)user;
    pthread_mutex_lock(&run->mu);
    if (is_stderr) {
        if (run->stderr_buf.len < 4 * 1024) {
            mcode_str_append(&run->stderr_buf, data, len);
        }
    } else if (run->stdout_buf.len < 64 * 1024) {
        mcode_str_append(&run->stdout_buf, data, len);
    }
    pthread_mutex_unlock(&run->mu);
}

static void on_commit_msg_exit(int exit_code, bool killed, void *user) {
    mcode_commit_msg_run *run = (mcode_commit_msg_run *)user;
    pthread_mutex_lock(&run->mu);
    run->exit_code = exit_code;
    run->killed = killed;
    run->finished = true;
    pthread_mutex_unlock(&run->mu);
}

mcode_commit_msg_run *mcode_agent_start_commit_msg(app_agent_kind agent,
                                                   const char *model,
                                                   const char *workdir,
                                                   const app_settings *settings,
                                                   const char *diff_context) {
    if (!diff_context || !diff_context[0]) return NULL;
    if (agent < 0 || agent >= APP_AGENT_COUNT) agent = APP_AGENT_CLAUDE;

    mcode_cli_status status;
    mcode_agent_detect_clis(settings, &status);
    if (!status.available[agent]) return NULL;

    size_t dlen = strlen(diff_context);
    if (dlen > 8192) {
        dlen = 8192;
        while (dlen > 0 && ((unsigned char)diff_context[dlen] & 0xC0) == 0x80) --dlen;
    }

    mcode_str prompt;
    mcode_str_init(&prompt);
    mcode_str_append_cstr(&prompt,
        "Write a concise single-line git commit message summarizing the changes below. "
        "Use ONLY the context provided below; do not run commands, read files, or call any tools. "
        "Match the repository's recent commit style if shown (such as conventional commits); "
        "otherwise use imperative mood (about 50 to 72 characters). "
        "Reply with ONLY the commit message line: no quotes, no code fences, no markdown, "
        "no prefixes like \"Commit message:\", no trailing period, and no explanation.\n\n");
    mcode_str_append(&prompt, diff_context, dlen);

    const char *bin = status.resolved_path[agent];
    const char *eff_model = (model && model[0]) ? model : mcode_agent_default_model(settings, agent);

    mcode_commit_msg_run *run = calloc(1, sizeof *run);
    if (!run) {
        mcode_str_free(&prompt);
        return NULL;
    }
    run->agent = agent;
    run->started_mono = mcode_now_mono();
    mcode_str_init(&run->stdout_buf);
    mcode_str_init(&run->stderr_buf);
    pthread_mutex_init(&run->mu, NULL);

    run->proc = spawn_one_shot_prompt(agent, bin, eff_model, workdir,
                                      mcode_str_cstr(&prompt), "45s",
                                      on_commit_msg_bytes, on_commit_msg_exit, run);
    mcode_str_free(&prompt);

    if (!run->proc) {
        mcode_str_free(&run->stdout_buf);
        mcode_str_free(&run->stderr_buf);
        pthread_mutex_destroy(&run->mu);
        free(run);
        return NULL;
    }

    app_log_info("Requested commit message from %s", mcode_agent_label(agent));
    return run;
}

bool mcode_agent_poll_commit_msg(mcode_commit_msg_run *run, char *out_msg, size_t cap,
                                 bool *out_ok) {
    if (out_msg && cap > 0) out_msg[0] = '\0';
    if (out_ok) *out_ok = false;
    if (!run) return true;

    pthread_mutex_lock(&run->mu);
    bool finished = run->finished;
    pthread_mutex_unlock(&run->mu);

    if (!finished) {
        if (!run->timed_out && (mcode_now_mono() - run->started_mono) > 60.0) {
            run->timed_out = true;
            if (run->proc) mcode_proc_stop(run->proc);
        }
        return false;
    }

    bool ok = false;
    if (!run->killed && run->exit_code == 0 && run->stdout_buf.len > 0) {
        ok = mcode_agent_parse_commit_msg_response(run->agent,
                                                   mcode_str_cstr(&run->stdout_buf),
                                                   run->stdout_buf.len,
                                                   out_msg, cap);
    }
    if (!ok && !run->killed) {
        char reason[256] = "";
        summarize_one_shot_failure(&run->stdout_buf, &run->stderr_buf, reason, sizeof reason);
        app_log_warn("Commit message generation via %s did not produce a message (exit=%d%s%s)",
                     mcode_agent_label(run->agent), run->exit_code,
                     reason[0] ? ", " : "", reason);
    }
    if (out_ok) *out_ok = ok;
    return true;
}

void mcode_agent_commit_msg_stop(mcode_commit_msg_run *run) {
    if (!run || !run->proc) return;
    mcode_proc_stop(run->proc);
}

void mcode_agent_commit_msg_kill(mcode_commit_msg_run *run) {
    if (!run || !run->proc) return;
    mcode_proc_kill(run->proc);
}

void mcode_agent_commit_msg_destroy(mcode_commit_msg_run *run) {
    if (!run) return;
    if (run->proc) {
        mcode_proc_destroy(run->proc);
        run->proc = NULL;
    }
    mcode_str_free(&run->stdout_buf);
    mcode_str_free(&run->stderr_buf);
    pthread_mutex_destroy(&run->mu);
    free(run);
}

app_agent_kind mcode_agent_commit_msg_agent(const mcode_commit_msg_run *run) {
    return run ? run->agent : APP_AGENT_CLAUDE;
}

void mcode_agent_parse_line_for_test(app_agent_kind agent, mcode_thread *thread,
                                     mcode_turn *turn, const char *line, bool is_stderr) {
    mcode_agent_run run;
    memset(&run, 0, sizeof run);
    run.agent = agent;
    mcode_str_init(&run.out_line);
    mcode_str_init(&run.err_line);
    pthread_mutex_init(&run.mu, NULL);

    handle_line(&run, line, is_stderr);

    agent_ev *cur = run.head;
    while (cur) {
        agent_ev *next = cur->next;
        apply_event(thread, turn, NULL, cur);
        ev_free(cur);
        cur = next;
    }
    mcode_str_free(&run.out_line);
    mcode_str_free(&run.err_line);
    pthread_mutex_destroy(&run.mu);
}
