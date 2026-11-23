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

typedef enum {
    EV_SESSION = 0,
    EV_TEXT_DELTA,
    EV_TEXT_FALLBACK,
    EV_REASONING_DELTA,
    EV_REASONING_DONE,
    EV_TOOL_START,
    EV_TOOL_INPUT_DELTA,
    EV_TOOL_RESULT,
    EV_STATUS,
    EV_USAGE,
    EV_ERROR,
    EV_DONE,
} agent_ev_kind;

typedef struct agent_ev {
    agent_ev_kind kind;
    char *text;
    char *tool_id;
    char *tool_name;
    char *tool_detail;
    bool flag; /* failed / killed */
    int int_a; /* exit_code or input_tokens */
    int int_b; /* output_tokens */
    double dbl_a; /* cost_usd */
    struct agent_ev *next;
} agent_ev;

struct mcode_agent_run {
    app_agent_kind agent;
    mcode_proc *proc;

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

static void emit_tool_result(mcode_agent_run *run, const char *id, const char *output, bool failed) {
    agent_ev *ev = calloc(1, sizeof *ev);
    if (!ev) return;
    ev->kind = EV_TOOL_RESULT;
    ev->tool_id = dup_or_null(id);
    ev->text = dup_or_null(output);
    ev->flag = failed;
    push_ev(run, ev);
}

static const char *jstr(yyjson_val *obj, const char *key) {
    if (!yyjson_is_obj(obj)) return NULL;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
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
        "file_path", "path", "command", "cmd", "pattern", "query", "url", "description", "prompt"
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
static void parse_claude_json(mcode_agent_run *run, yyjson_val *root) {
    const char *type = jstr(root, "type");
    const char *sid = jstr(root, "session_id");
    if (sid && sid[0]) emit_simple(run, EV_SESSION, sid);
    if (!type) return;

    if (strcmp(type, "system") == 0) {
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
                    emit_simple(run, EV_TEXT_FALLBACK, txt);
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
                char detail[256] = "";
                summarize_tool_input(inp, detail, sizeof detail);
                char *inp_json = stringify_json_val(inp);
                emit_tool_start(run, tid, tname ? tname : "tool", detail, inp_json ? inp_json : "");
                free(inp_json);
                char st[160];
                if (detail[0]) snprintf(st, sizeof st, "%s: %s", tname ? tname : "tool", detail);
                else snprintf(st, sizeof st, "Running %s...", tname ? tname : "tool");
                emit_simple(run, EV_STATUS, st);
            }
        }
        /* Reset per-message streaming flags for next assistant turn within tool loop */
        run->streamed_text_deltas = false;
        run->streamed_thinking_deltas = false;
        return;
    }

    if (strcmp(type, "user") == 0) {
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
        }
        return;
    }

    if (strcmp(type, "result") == 0) {
        yyjson_val *is_err_v = yyjson_obj_get(root, "is_error");
        bool is_err = yyjson_is_bool(is_err_v) && yyjson_get_bool(is_err_v);
        const char *res_text = jstr(root, "result");
        if (is_err && res_text && res_text[0]) {
            emit_simple(run, EV_ERROR, res_text);
        } else if (res_text && res_text[0]) {
            emit_simple(run, EV_TEXT_FALLBACK, res_text);
        }
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        yyjson_val *cost = yyjson_obj_get(root, "total_cost_usd");
        if (yyjson_is_obj(usage) || yyjson_is_num(cost)) {
            agent_ev *uev = calloc(1, sizeof *uev);
            if (uev) {
                uev->kind = EV_USAGE;
                if (yyjson_is_obj(usage)) {
                    yyjson_val *in_v = yyjson_obj_get(usage, "input_tokens");
                    yyjson_val *out_v = yyjson_obj_get(usage, "output_tokens");
                    if (yyjson_is_num(in_v)) uev->int_a = (int)yyjson_get_num(in_v);
                    if (yyjson_is_num(out_v)) uev->int_b = (int)yyjson_get_num(out_v);
                }
                if (yyjson_is_num(cost)) uev->dbl_a = yyjson_get_num(cost);
                push_ev(run, uev);
            }
        }
    }
}

/* Treat exit code 1 from common search/probe utilities (rg, grep, diff, test, etc.)
 * as a normal "no matches" completion unless stderr/output indicates an actual error. */
static bool is_benign_exit_one(const char *cmd, const char *output) {
    if (!cmd || !cmd[0]) return false;
    static const char *const search_tools[] = {
        "rg ", "grep ", "egrep ", "fgrep ", "fd ", "diff ", "cmp ", "which ", "command -v ", "test "
    };
    bool has_probe = false;
    for (size_t i = 0; i < sizeof search_tools / sizeof search_tools[0]; ++i) {
        if (strstr(cmd, search_tools[i])) {
            has_probe = true;
            break;
        }
    }
    if (!has_probe) return false;
    if (output && output[0]) {
        if (strstr(output, "command not found") ||
            strstr(output, "Permission denied") ||
            strstr(output, "No such file or directory") ||
            strstr(output, "error:") ||
            strstr(output, "Error:") ||
            strstr(output, "fatal:")) {
            return false;
        }
    }
    return true;
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
            int exit_code = yyjson_is_num(ec) ? (int)yyjson_get_num(ec) : 0;
            bool failed = yyjson_is_num(ec) && exit_code != 0;
            const char *status = jstr(item, "status");
            if (status && strcmp(status, "failed") == 0) failed = true;
            if (failed && exit_code == 1 && is_benign_exit_one(cmd, out)) {
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
                emit_tool_result(run, iid ? iid : "cmd", out ? out : "", failed);
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
        }
        return;
    }

    if (strcmp(type, "turn.completed") == 0) {
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        if (yyjson_is_obj(usage)) {
            agent_ev *uev = calloc(1, sizeof *uev);
            if (uev) {
                uev->kind = EV_USAGE;
                yyjson_val *in_v = yyjson_obj_get(usage, "input_tokens");
                yyjson_val *out_v = yyjson_obj_get(usage, "output_tokens");
                if (yyjson_is_num(in_v)) uev->int_a = (int)yyjson_get_num(in_v);
                if (yyjson_is_num(out_v)) uev->int_b = (int)yyjson_get_num(out_v);
                push_ev(run, uev);
            }
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
static void parse_agy_json(mcode_agent_run *run, yyjson_val *root) {
    const char *cid = jstr(root, "conversation_id");
    if (!cid) cid = jstr(root, "session_id");
    if (!cid) cid = jstr(root, "conversationId");
    if (cid && cid[0]) emit_simple(run, EV_SESSION, cid);

    const char *type = jstr(root, "type");
    if (!type) type = jstr(root, "event");

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

    /* Text delta or final text */
    const char *txt = jstr(root, "delta");
    if (!txt) txt = jstr(root, "text");
    if (!txt) txt = jstr(root, "content");
    if (!txt) txt = jstr(root, "result");
    if (txt && txt[0]) {
        emit_simple(run, EV_REASONING_DONE, NULL);
        emit_simple(run, EV_TEXT_DELTA, txt);
        emit_simple(run, EV_STATUS, "Responding...");
    }

    yyjson_val *usage = yyjson_obj_get(root, "usage");
    if (yyjson_is_obj(usage)) {
        agent_ev *uev = calloc(1, sizeof *uev);
        if (uev) {
            uev->kind = EV_USAGE;
            yyjson_val *in_v = yyjson_obj_get(usage, "input_tokens");
            yyjson_val *out_v = yyjson_obj_get(usage, "output_tokens");
            if (yyjson_is_num(in_v)) uev->int_a = (int)yyjson_get_num(in_v);
            if (yyjson_is_num(out_v)) uev->int_b = (int)yyjson_get_num(out_v);
            push_ev(run, uev);
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
            if (strstr(p, "Error") || strstr(p, "error") || strstr(p, "FATAL") || strstr(p, "not found")) {
                emit_simple(run, EV_ERROR, p);
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
                switch (run->agent) {
                case APP_AGENT_CLAUDE: parse_claude_json(run, root); break;
                case APP_AGENT_CODEX:  parse_codex_json(run, root); break;
                case APP_AGENT_AGY:    parse_agy_json(run, root); break;
                default: break;
                }
            }
            yyjson_doc_free(doc);
            free(copy);
            return;
        }
    }

    /* Plain text fallback */
    emit_simple(run, EV_TEXT_DELTA, copy);
    emit_simple(run, EV_TEXT_DELTA, "\n");
    free(copy);
}

static void on_proc_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    mcode_agent_run *run = (mcode_agent_run *)user;
    mcode_str *buf = is_stderr ? &run->err_line : &run->out_line;

    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            handle_line(run, mcode_str_cstr(buf), is_stderr);
            mcode_str_clear(buf);
        } else {
            mcode_str_append(buf, &data[i], 1);
        }
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
    agent_ev *ev = calloc(1, sizeof *ev);
    if (ev) {
        ev->kind = EV_DONE;
        ev->int_a = exit_code;
        ev->flag = killed;
        if (exit_code != 0 && !killed && run->stderr_tail.len > 0) {
            ev->text = dup_or_null(mcode_str_cstr(&run->stderr_tail));
        }
        push_ev(run, ev);
    }
    pthread_mutex_lock(&run->mu);
    run->finished = true;
    pthread_mutex_unlock(&run->mu);
}

static void apply_event(mcode_thread *thread, mcode_turn *turn, const agent_ev *ev) {
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
            b->status = ev->flag ? CUI_TASK_FAILED : CUI_TASK_DONE;
            b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            if (ev->text) mcode_str_set(&b->output, ev->text);
        }
        break;
    }
    case EV_STATUS:
        if (ev->text && ev->text[0]) {
            snprintf(thread->status_text, sizeof thread->status_text, "%s", ev->text);
        }
        break;
    case EV_USAGE:
        turn->input_tokens += ev->int_a;
        turn->output_tokens += ev->int_b;
        if (ev->dbl_a > 0.0) turn->cost_usd = ev->dbl_a;
        break;
    case EV_ERROR: {
        if (ev->text && ev->text[0]) {
            mcode_block *b = mcode_turn_add_block(turn, MCODE_BLOCK_ERROR);
            if (b) mcode_str_set(&b->text, ev->text);
            turn->failed = true;
            thread->scroll_to_bottom_frames = 2;
        }
        break;
    }
    case EV_DONE: {
        turn->running = false;
        turn->duration_sec = (float)(mcode_now_mono() - turn->started_mono);
        /* Finalize any still-running blocks */
        for (int i = 0; i < turn->block_count; ++i) {
            mcode_block *b = &turn->blocks[i];
            if (b->kind == MCODE_BLOCK_REASONING && b->thinking) {
                b->thinking = false;
                b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            } else if (b->kind == MCODE_BLOCK_TOOL && b->status == CUI_TASK_RUNNING) {
                b->status = (ev->flag || ev->int_a != 0) ? CUI_TASK_FAILED : CUI_TASK_DONE;
                b->duration_sec = (float)(mcode_now_mono() - b->started_mono);
            }
        }
        if (ev->flag) {
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
    char codex_effort_cfg[64] = "";
    if (thread->reasoning_effort[0]) {
        snprintf(codex_effort_cfg, sizeof codex_effort_cfg,
                 "model_reasoning_effort=\"%s\"", thread->reasoning_effort);
    }
    const char *codex_speed_cfg = NULL;
    if (thread->speed_mode == MCODE_SPEED_FAST) {
        codex_speed_cfg = "service_tier=\"fast\"";
    } else if (thread->speed_mode == MCODE_SPEED_STANDARD) {
        codex_speed_cfg = "service_tier=\"default\"";
    }
    const char *argv[32];
    int n = 0;

    if (thread->agent == APP_AGENT_CLAUDE) {
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = prompt;
        argv[n++] = "--output-format";
        argv[n++] = "stream-json";
        argv[n++] = "--verbose";
        argv[n++] = "--include-partial-messages";
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
        if (thread->auto_approve) {
            argv[n++] = "--dangerously-skip-permissions";
        }
    } else if (thread->agent == APP_AGENT_CODEX) {
        argv[n++] = bin;
        argv[n++] = "exec";
        if (thread->session_id[0]) {
            argv[n++] = "resume";
            argv[n++] = "--json";
            argv[n++] = "--skip-git-repo-check";
            if (model[0]) {
                argv[n++] = "-m";
                argv[n++] = model;
            }
            if (codex_effort_cfg[0]) {
                argv[n++] = "-c";
                argv[n++] = codex_effort_cfg;
            }
            if (codex_speed_cfg) {
                argv[n++] = "-c";
                argv[n++] = codex_speed_cfg;
            }
            if (thread->auto_approve) {
                argv[n++] = "--dangerously-bypass-approvals-and-sandbox";
            } else {
                argv[n++] = "-c";
                argv[n++] = "sandbox_mode=\"workspace-write\"";
            }
            argv[n++] = thread->session_id;
            argv[n++] = prompt;
        } else {
            argv[n++] = "--json";
            argv[n++] = "--skip-git-repo-check";
            if (thread->workdir[0]) {
                argv[n++] = "--cd";
                argv[n++] = thread->workdir;
            }
            if (model[0]) {
                argv[n++] = "-m";
                argv[n++] = model;
            }
            if (codex_effort_cfg[0]) {
                argv[n++] = "-c";
                argv[n++] = codex_effort_cfg;
            }
            if (codex_speed_cfg) {
                argv[n++] = "-c";
                argv[n++] = codex_speed_cfg;
            }
            if (thread->auto_approve) {
                argv[n++] = "--dangerously-bypass-approvals-and-sandbox";
            } else {
                argv[n++] = "-s";
                argv[n++] = "workspace-write";
            }
            argv[n++] = prompt;
        }
    } else { /* APP_AGENT_AGY */
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = prompt;
        argv[n++] = "--output-format";
        argv[n++] = "stream-json";
        if (thread->session_id[0]) {
            argv[n++] = "--conversation";
            argv[n++] = thread->session_id;
        }
        if (model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
        if (thread->auto_approve) {
            argv[n++] = "--dangerously-skip-permissions";
        }
    }
    argv[n] = NULL;

    mcode_agent_run *run = calloc(1, sizeof *run);
    if (!run) return NULL;
    run->agent = thread->agent;
    mcode_str_init(&run->out_line);
    mcode_str_init(&run->err_line);
    mcode_str_init(&run->stderr_tail);
    pthread_mutex_init(&run->mu, NULL);

    mcode_turn *turn = mcode_thread_add_turn(thread, prompt);
    if (!turn) {
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

    run->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = thread->workdir[0] ? thread->workdir : NULL,
        .merge_stderr = false,
        .on_bytes = on_proc_bytes,
        .on_exit = on_proc_exit,
        .user = run,
    });
    if (!run->proc) {
        turn->running = false;
        turn->failed = true;
        snprintf(thread->status_text, sizeof thread->status_text, "Launch failed");
        if (err && err_cap) snprintf(err, err_cap, "Failed to spawn %s.", bin);
        pthread_mutex_destroy(&run->mu);
        free(run);
        return NULL;
    }

    thread->active_run = run;
    app_log_info("Started %s turn in %s", mcode_agent_label(thread->agent), thread->workdir);
    return run;
}

bool mcode_agent_poll(mcode_agent_run *run, mcode_thread *thread, bool *out_finished) {
    if (out_finished) *out_finished = false;
    if (!run || !thread || thread->turn_count == 0) return false;

    pthread_mutex_lock(&run->mu);
    agent_ev *list = run->head;
    run->head = NULL;
    run->tail = NULL;
    bool finished = run->finished;
    pthread_mutex_unlock(&run->mu);

    bool changed = false;
    mcode_turn *turn = &thread->turns[thread->turn_count - 1];
    while (list) {
        agent_ev *next = list->next;
        apply_event(thread, turn, list);
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
    mcode_proc_stop(run->proc);
}

void mcode_agent_destroy(mcode_agent_run *run) {
    if (!run) return;
    if (run->proc) {
        mcode_proc_destroy(run->proc);
        run->proc = NULL;
    }
    pthread_mutex_lock(&run->mu);
    agent_ev *cur = run->head;
    run->head = run->tail = NULL;
    pthread_mutex_unlock(&run->mu);
    while (cur) {
        agent_ev *next = cur->next;
        ev_free(cur);
        cur = next;
    }
    mcode_str_free(&run->out_line);
    mcode_str_free(&run->err_line);
    mcode_str_free(&run->stderr_tail);
    pthread_mutex_destroy(&run->mu);
    free(run);
}

/* ---- Asynchronous thread title generation ---- */

struct mcode_title_run {
    app_agent_kind agent;
    mcode_proc *proc;
    pthread_mutex_t mu;
    mcode_str stdout_buf;
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

bool mcode_agent_parse_title_response(app_agent_kind agent, const char *raw, size_t len,
                                      char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
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
                        const char *type = jstr(root, "type");
                        if (!type) type = jstr(root, "event");
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
            ok = mcode_agent_sanitize_title(mcode_str_cstr(&extracted), out, cap);
        }
    } else if (plain.len > 0) {
        ok = mcode_agent_sanitize_title(mcode_str_cstr(&plain), out, cap);
    }

    mcode_str_free(&extracted);
    mcode_str_free(&plain);
    return ok;
}

static void on_title_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    if (is_stderr) return;
    mcode_title_run *run = (mcode_title_run *)user;
    pthread_mutex_lock(&run->mu);
    if (run->stdout_buf.len < 64 * 1024) {
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
        "Reply with ONLY the title text: no quotes, no markdown, no prefixes, "
        "no explanation, and no trailing punctuation.\n\nRequest:\n");
    mcode_str_append(&title_prompt, prompt, plen);

    const char *bin = status.resolved_path[thread->agent];
    const char *model = mcode_agent_effective_model(thread, settings);
    const char *argv[24];
    int n = 0;

    if (thread->agent == APP_AGENT_CLAUDE) {
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = mcode_str_cstr(&title_prompt);
        argv[n++] = "--output-format";
        argv[n++] = "json";
        argv[n++] = "--no-session-persistence";
        argv[n++] = "--tools";
        argv[n++] = "";
        if (model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
    } else if (thread->agent == APP_AGENT_CODEX) {
        argv[n++] = bin;
        argv[n++] = "exec";
        argv[n++] = "--json";
        argv[n++] = "--ephemeral";
        argv[n++] = "--skip-git-repo-check";
        argv[n++] = "-s";
        argv[n++] = "read-only";
        argv[n++] = "-c";
        argv[n++] = "model_reasoning_effort=\"low\"";
        if (thread->workdir[0]) {
            argv[n++] = "--cd";
            argv[n++] = thread->workdir;
        }
        if (model[0]) {
            argv[n++] = "-m";
            argv[n++] = model;
        }
        argv[n++] = mcode_str_cstr(&title_prompt);
    } else { /* APP_AGENT_AGY */
        argv[n++] = bin;
        argv[n++] = "-p";
        argv[n++] = mcode_str_cstr(&title_prompt);
        argv[n++] = "--output-format";
        argv[n++] = "stream-json";
        argv[n++] = "--disable-slash-commands";
        argv[n++] = "--print-timeout";
        argv[n++] = "30s";
        if (model[0]) {
            argv[n++] = "--model";
            argv[n++] = model;
        }
    }
    argv[n] = NULL;

    mcode_title_run *run = calloc(1, sizeof *run);
    if (!run) {
        mcode_str_free(&title_prompt);
        return NULL;
    }
    run->agent = thread->agent;
    run->started_mono = mcode_now_mono();
    mcode_str_init(&run->stdout_buf);
    pthread_mutex_init(&run->mu, NULL);

    run->proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = thread->workdir[0] ? thread->workdir : NULL,
        .merge_stderr = false,
        .on_bytes = on_title_bytes,
        .on_exit = on_title_exit,
        .user = run,
    });
    mcode_str_free(&title_prompt);

    if (!run->proc) {
        mcode_str_free(&run->stdout_buf);
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
        app_log_warn("Title generation via %s did not produce a title (exit=%d)",
                     mcode_agent_label(run->agent), run->exit_code);
    }
    if (out_ok) *out_ok = ok;
    return true;
}

void mcode_agent_title_stop(mcode_title_run *run) {
    if (!run || !run->proc) return;
    mcode_proc_stop(run->proc);
}

void mcode_agent_title_destroy(mcode_title_run *run) {
    if (!run) return;
    if (run->proc) {
        mcode_proc_destroy(run->proc);
        run->proc = NULL;
    }
    mcode_str_free(&run->stdout_buf);
    pthread_mutex_destroy(&run->mu);
    free(run);
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
        apply_event(thread, turn, cur);
        ev_free(cur);
        cur = next;
    }
    mcode_str_free(&run.out_line);
    mcode_str_free(&run.err_line);
    pthread_mutex_destroy(&run.mu);
}
