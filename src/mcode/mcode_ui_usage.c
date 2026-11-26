#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_ui_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cached_tokens;
    double cost_usd;
    float duration_sec;
    int turns;
    int threads;
} usage_bucket;

typedef struct {
    usage_bucket total;
    usage_bucket by_agent[APP_AGENT_COUNT];
} usage_scope_stats;

typedef struct {
    usage_bucket thread;
    int64_t thread_ctx_tokens;
    int64_t thread_ctx_window;
    usage_scope_stats scopes[3]; /* 0: Today, 1: 7 days, 2: All time */
} usage_snapshot;

static int64_t local_day_start_epoch(int64_t now_epoch) {
    time_t t = (time_t)now_epoch;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &t) != 0) return now_epoch - (now_epoch % 86400);
#else
    if (!localtime_r(&t, &lt)) return now_epoch - (now_epoch % 86400);
#endif
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    time_t start = mktime(&lt);
    if (start == (time_t)-1) return now_epoch - (now_epoch % 86400);
    return (int64_t)start;
}

static void add_turn_to_bucket(usage_bucket *b, const mcode_turn *tr) {
    if (!b || !tr) return;
    b->input_tokens += tr->input_tokens;
    b->output_tokens += tr->output_tokens;
    b->cached_tokens += tr->cached_tokens;
    b->cost_usd += tr->cost_usd;
    if (tr->duration_sec > 0.0f) b->duration_sec += tr->duration_sec;
    b->turns += 1;
}

static void compute_usage_snapshot(const mcode_app *app, const mcode_thread *active,
                                   int64_t now_epoch, usage_snapshot *out) {
    memset(out, 0, sizeof *out);
    if (!app) return;

    int64_t today_start = local_day_start_epoch(now_epoch);
    int64_t week_start = today_start - (int64_t)6 * 86400;

    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        int ag = (int)t->agent;
        if (ag < 0 || ag >= APP_AGENT_COUNT) ag = 0;

        bool thread_in_scope[3] = { false, false, false };

        for (int ti = 0; ti < t->turn_count; ++ti) {
            const mcode_turn *tr = &t->turns[ti];
            int64_t ts = tr->created_at > 0 ? tr->created_at : t->updated_at;

            add_turn_to_bucket(&out->scopes[2].total, tr);
            add_turn_to_bucket(&out->scopes[2].by_agent[ag], tr);
            thread_in_scope[2] = true;

            if (ts >= week_start) {
                add_turn_to_bucket(&out->scopes[1].total, tr);
                add_turn_to_bucket(&out->scopes[1].by_agent[ag], tr);
                thread_in_scope[1] = true;
            }
            if (ts >= today_start) {
                add_turn_to_bucket(&out->scopes[0].total, tr);
                add_turn_to_bucket(&out->scopes[0].by_agent[ag], tr);
                thread_in_scope[0] = true;
            }
        }

        for (int s = 0; s < 3; ++s) {
            if (thread_in_scope[s]) {
                out->scopes[s].total.threads += 1;
                out->scopes[s].by_agent[ag].threads += 1;
            }
        }
    }

    if (active) {
        for (int ti = 0; ti < active->turn_count; ++ti) {
            const mcode_turn *tr = &active->turns[ti];
            add_turn_to_bucket(&out->thread, tr);
            if (tr->context_tokens > 0) out->thread_ctx_tokens = tr->context_tokens;
            if (tr->context_window > 0) out->thread_ctx_window = tr->context_window;
        }
        if (out->thread_ctx_window <= 0) {
            const app_settings *settings = app->chrome ? app_chrome_settings(app->chrome) : NULL;
            out->thread_ctx_window = mcode_model_context_window(&app->models, active, settings);
        }
    }
}

static bool is_agy_busy(const mcode_app *app) {
    if (!app) return false;
    for (int i = 0; i < app->store.thread_count; ++i) {
        const mcode_thread *t = &app->store.threads[i];
        if (t->agent == APP_AGENT_AGY && (t->active_run || t->title_run)) {
            return true;
        }
    }
    return false;
}

static void format_tokens_compact(int64_t tokens, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (tokens < 0) tokens = 0;
    if (tokens < 1000) {
        snprintf(out, cap, "%lld", (long long)tokens);
    } else if (tokens < 100000) {
        snprintf(out, cap, "%.1fk", (double)tokens / 1000.0);
    } else if (tokens < 1000000) {
        snprintf(out, cap, "%.0fk", (double)tokens / 1000.0);
    } else if (tokens < 100000000) {
        snprintf(out, cap, "%.2fM", (double)tokens / 1000000.0);
    } else if (tokens < 1000000000) {
        snprintf(out, cap, "%.1fM", (double)tokens / 1000000.0);
    } else {
        snprintf(out, cap, "%.2fB", (double)tokens / 1000000000.0);
    }
}

static void format_cost_usd(double usd, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (usd < 0.0) usd = 0.0;
    if (usd < 0.01) {
        snprintf(out, cap, "$%.4f", usd);
    } else {
        snprintf(out, cap, "$%.2f", usd);
    }
}

static void format_window_short(int64_t mins, const char *fallback, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (mins <= 0) {
        snprintf(out, cap, "%s", fallback ? fallback : "");
        return;
    }
    if (mins % 1440 == 0) {
        snprintf(out, cap, "%lldd", (long long)(mins / 1440));
    } else if (mins % 60 == 0) {
        snprintf(out, cap, "%lldh", (long long)(mins / 60));
    } else {
        snprintf(out, cap, "%lldm", (long long)mins);
    }
}

static void format_reset_short(int64_t resets_at, int64_t now_epoch, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (resets_at <= 0) return;
    int64_t diff = resets_at - now_epoch;
    if (diff <= 0) {
        snprintf(out, cap, "soon");
    } else if (diff < 60) {
        snprintf(out, cap, "<1m");
    } else if (diff < 3600) {
        snprintf(out, cap, "%lldm", (long long)(diff / 60));
    } else if (diff < 86400) {
        int64_t h = diff / 3600;
        int64_t m = (diff % 3600) / 60;
        if (m > 0) snprintf(out, cap, "%lldh %lldm", (long long)h, (long long)m);
        else snprintf(out, cap, "%lldh", (long long)h);
    } else {
        int64_t d = diff / 86400;
        int64_t h = (diff % 86400) / 3600;
        if (h > 0) snprintf(out, cap, "%lldd %lldh", (long long)d, (long long)h);
        else snprintf(out, cap, "%lldd", (long long)d);
    }
}

static void format_age_short(int64_t updated_epoch, int64_t now_epoch, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (updated_epoch <= 0) return;
    int64_t diff = now_epoch - updated_epoch;
    if (diff < 15) {
        snprintf(out, cap, "just now");
    } else if (diff < 60) {
        snprintf(out, cap, "%llds ago", (long long)diff);
    } else if (diff < 3600) {
        snprintf(out, cap, "%lldm ago", (long long)(diff / 60));
    } else {
        snprintf(out, cap, "%lldh ago", (long long)(diff / 3600));
    }
}

static void format_plan_label(const char *plan, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!plan || !plan[0]) return;
    snprintf(out, cap, "%s", plan);
    if (islower((unsigned char)out[0])) {
        out[0] = (char)toupper((unsigned char)out[0]);
    }
}

static int remaining_percent(int used_percent) {
    int left = 100 - used_percent;
    if (left < 0) return 0;
    if (left > 100) return 100;
    return left;
}

static cui_tone tone_for_percent(int pct) {
    if (pct >= 90) return CUI_TONE_DANGER;
    if (pct >= 70) return CUI_TONE_WARNING;
    return CUI_TONE_SUCCESS;
}

static cui_color color_for_percent(cui_ctx *ui, int pct) {
    if (pct >= 90) return cui_color_of(ui, CUI_COLOR_DANGER);
    if (pct >= 70) return cui_color_of(ui, CUI_COLOR_WARNING);
    return cui_color_of(ui, CUI_COLOR_FOREGROUND);
}

/* Select which Antigravity quota group to spotlight in the compact bottom bar
 * (matches the active thread's model when on Antigravity, otherwise group 0). */
static const mcode_quota_group *select_agy_bar_group(const mcode_app *app,
                                                     const mcode_thread *thread) {
    const mcode_provider_quota *aq = &app->usage.quotas[APP_AGENT_AGY];
    if (aq->group_count <= 0) return NULL;
    if (thread && thread->agent == APP_AGENT_AGY) {
        const app_settings *settings = app->chrome ? app_chrome_settings(app->chrome) : NULL;
        const char *m = mcode_agent_effective_model(thread, settings);
        if (m && (strstr(m, "claude") != NULL || strstr(m, "gpt") != NULL)) {
            for (int i = 0; i < aq->group_count; ++i) {
                if (strstr(aq->groups[i].name, "Claude") != NULL ||
                    strstr(aq->groups[i].name, "GPT") != NULL) {
                    return &aq->groups[i];
                }
            }
        }
    }
    return &aq->groups[0];
}

static void draw_window_pair(cui_ctx *ui, const char *id,
                             const mcode_rate_window *w1, const char *lbl1,
                             const mcode_rate_window *w2, const char *lbl2,
                             int64_t now_epoch) {
    bool has1 = (w1 && w1->available);
    bool has2 = (w2 && w2->available);
    if (!has1 && !has2) return;

    cui_push_id(ui, id);
    cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_STRETCH) {
        const mcode_rate_window *ws[2] = { w1, w2 };
        const char *lbls[2] = { lbl1, lbl2 };
        bool has[2] = { has1, has2 };
        for (int i = 0; i < 2; ++i) {
            if (!has[i]) continue;
            const mcode_rate_window *w = ws[i];
            int left_pct = remaining_percent(w->used_percent);
            char wtag[16];
            format_window_short(w->window_mins, lbls[i], wtag, sizeof wtag);
            char pct_s[24];
            snprintf(pct_s, sizeof pct_s, "%d%% left", left_pct);
            char rst_s[24];
            format_reset_short(w->resets_at, now_epoch, rst_s, sizeof rst_s);

            cui_push_id_int(ui, i);
            cui_column(ui, .width = CUI_FILL, .gap = 3) {
                cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, wtag, .size = 11.0f, .color = CUI_COLOR_TEXT_SECONDARY,
                             .nowrap = true);
                    cui_text(ui, pct_s, .font = CUI_FONT_MEDIUM, .size = 11.5f,
                             .color = color_for_percent(ui, w->used_percent),
                             .tabular = true, .nowrap = true);
                    cui_spacer(ui);
                    if (rst_s[0]) {
                        cui_text(ui, rst_s, .size = 10.5f, .color = CUI_COLOR_TEXT_MUTED,
                                 .tabular = true, .nowrap = true);
                    }
                }
                cui_progress(ui, (float)left_pct / 100.0f,
                             .tone = tone_for_percent(w->used_percent),
                             .height = 4.5f, .width = CUI_FILL);
            }
            cui_pop_id(ui);
        }
    }
    cui_pop_id(ui);
}

static void draw_provider_limit_card(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                     const usage_snapshot *snap, int64_t now_epoch) {
    const mcode_provider_quota *q = &app->usage.quotas[agent];
    bool installed = app->cli_status.available[agent];
    const usage_bucket *mcode_all = &snap->scopes[2].by_agent[agent];
    int64_t mcode_tot = mcode_all->input_tokens + mcode_all->output_tokens;

    cui_push_id_int(ui, (int)agent);
    cui_card(ui, .width = CUI_FILL, .muted = true, .pad_x = 10, .pad_y = 8,
             .radius = 9, .gap = 6) {
        /* Card header: Provider icon + Name + Plan/Auth badge + Age/Spinner */
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            mcode_ui_draw_provider_icon(app, ui, agent, 14.0f,
                                        installed ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_TEXT_MUTED);
            cui_text(ui, mcode_agent_label(agent), .font = CUI_FONT_MEDIUM, .size = 12.5f,
                     .color = installed ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED,
                     .nowrap = true);

            if (!installed) {
                cui_badge(ui, "Not installed", .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
            } else if (q->plan[0]) {
                char plan_lbl[48];
                format_plan_label(q->plan, plan_lbl, sizeof plan_lbl);
                cui_badge(ui, plan_lbl, .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
            } else if (agent == APP_AGENT_CLAUDE && q->auth_checked) {
                if (!q->logged_in) {
                    cui_badge(ui, "Not signed in", .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM);
                } else if (q->auth_method[0]) {
                    cui_badge(ui, q->auth_method, .tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM);
                } else {
                    cui_badge(ui, "Signed in", .tone = CUI_TONE_SUCCESS, .size = CUI_SIZE_SM);
                }
            }

            cui_spacer(ui);
            if (q->loading) {
                cui_spinner(ui, 11.0f);
            } else if (q->updated_epoch > 0) {
                char age[32];
                format_age_short(q->updated_epoch, now_epoch, age, sizeof age);
                if (age[0]) {
                    cui_text(ui, age, .size = 10.5f, .color = CUI_COLOR_TEXT_MUTED,
                             .tabular = true, .nowrap = true);
                }
            }
        }

        if (!installed) {
            cui_text(ui, "Configure CLI path in Settings to enable usage tracking.",
                     .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED);
        } else if (q->group_count > 0) {
            /* Multi-group quotas (Antigravity: Gemini Models + Claude & GPT models) */
            for (int gi = 0; gi < q->group_count; ++gi) {
                const mcode_quota_group *g = &q->groups[gi];
                cui_push_id_int(ui, gi);
                cui_column(ui, .width = CUI_FILL, .gap = 3) {
                    cui_text(ui, g->name, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                             .nowrap = true);
                    draw_window_pair(ui, "gwin",
                                     &g->primary, "5h",
                                     &g->secondary, "7d",
                                     now_epoch);
                }
                cui_pop_id(ui);
            }
        } else if (q->available) {
            draw_window_pair(ui, "win",
                             &q->primary, "5h",
                             &q->secondary, "7d",
                             now_epoch);
        } else if (q->loading) {
            cui_text(ui, "Checking live limits...", .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED);
        } else if (agent == APP_AGENT_CLAUDE && q->auth_checked && !q->logged_in) {
            cui_text(ui, "Run `claude auth login` to enable live plan rate limits.",
                     .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED);
        } else if (agent == APP_AGENT_CLAUDE && q->auth_checked && q->logged_in) {
            cui_text(ui, "Live 5h / 7d plan limits update automatically during Claude turns.",
                     .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED);
        } else if (q->error[0]) {
            cui_text(ui, q->error, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED);
        }

        /* Compact account / Mcode token summary footer */
        char meta[192] = "";
        int pos = 0;
        if (q->account_today_tokens > 0) {
            char tb[24];
            format_tokens_compact(q->account_today_tokens, tb, sizeof tb);
            pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s today", tb);
        }
        if (q->account_lifetime_tokens > 0 && (size_t)pos < sizeof meta) {
            char lb[24];
            format_tokens_compact(q->account_lifetime_tokens, lb, sizeof lb);
            const char *lbl = (agent == APP_AGENT_CLAUDE) ? "local CLI" : "lifetime";
            pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%s %s",
                            pos > 0 ? " · " : "", lb, lbl);
        }
        if (mcode_tot > 0 && (size_t)pos < sizeof meta) {
            char mb[24];
            format_tokens_compact(mcode_tot, mb, sizeof mb);
            pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%s in Mcode",
                            pos > 0 ? " · " : "", mb);
        }
        double total_cost = mcode_all->cost_usd > q->account_cost_usd
            ? mcode_all->cost_usd : q->account_cost_usd;
        if (total_cost > 0.0 && (size_t)pos < sizeof meta) {
            char cb[24];
            format_cost_usd(total_cost, cb, sizeof cb);
            pos += snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%s",
                            pos > 0 ? " · " : "", cb);
        }
        if (q->current_streak_days > 0 && (size_t)pos < sizeof meta) {
            snprintf(meta + pos, sizeof meta - (size_t)pos, "%s%dd streak",
                     pos > 0 ? " · " : "", q->current_streak_days);
        }
        if (meta[0]) {
            cui_text(ui, meta, .size = 10.5f, .color = CUI_COLOR_TEXT_MUTED,
                     .tabular = true, .nowrap = true);
        }
    }
    cui_pop_id(ui);
}

static void draw_usage_activity_tab(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                    const usage_snapshot *snap) {
    /* Active thread summary & context window bar */
    if (thread) {
        int64_t thr_total = snap->thread.input_tokens + snap->thread.output_tokens;
        const app_settings *settings = app->chrome ? app_chrome_settings(app->chrome) : NULL;
        char model_name[128] = "";
        mcode_model_format_display(&app->models, thread, settings,
                                   model_name, sizeof model_name, NULL, 0);

        cui_card(ui, .width = CUI_FILL, .muted = true, .pad_x = 10, .pad_y = 8,
                 .radius = 9, .gap = 5) {
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Active thread", .font = CUI_FONT_MEDIUM, .size = 11.5f,
                         .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                cui_spacer(ui);
                mcode_ui_draw_provider_icon(app, ui, thread->agent, 12.5f,
                                            CUI_COLOR_TEXT_MUTED);
                cui_text(ui, model_name[0] ? model_name : mcode_agent_short_label(thread->agent),
                         .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
            }

            char tot_s[24], in_s[24], out_s[24];
            format_tokens_compact(thr_total, tot_s, sizeof tot_s);
            format_tokens_compact(snap->thread.input_tokens, in_s, sizeof in_s);
            format_tokens_compact(snap->thread.output_tokens, out_s, sizeof out_s);

            cui_row(ui, .width = CUI_FILL, .gap = 7, .align = CUI_ALIGN_CENTER) {
                char main_tok[48];
                snprintf(main_tok, sizeof main_tok, "%s tok", tot_s);
                cui_text(ui, main_tok, .font = CUI_FONT_MEDIUM, .size = 12.5f,
                         .color = CUI_COLOR_FOREGROUND, .tabular = true, .nowrap = true);
                if (snap->thread.cost_usd > 0.0) {
                    char cost_s[24];
                    format_cost_usd(snap->thread.cost_usd, cost_s, sizeof cost_s);
                    cui_badge(ui, cost_s, .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
                }
                if (thr_total > 0) {
                    char detail[96];
                    if (snap->thread.cached_tokens > 0) {
                        char cache_s[24];
                        format_tokens_compact(snap->thread.cached_tokens, cache_s, sizeof cache_s);
                        snprintf(detail, sizeof detail, "%s in · %s out · %s cached",
                                 in_s, out_s, cache_s);
                    } else {
                        snprintf(detail, sizeof detail, "%s in · %s out", in_s, out_s);
                    }
                    cui_text(ui, detail, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                             .tabular = true, .nowrap = true);
                }
                cui_spacer(ui);
                char turns_s[32];
                snprintf(turns_s, sizeof turns_s, "%d %s",
                         snap->thread.turns, snap->thread.turns == 1 ? "turn" : "turns");
                cui_text(ui, turns_s, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                         .tabular = true, .nowrap = true);
            }

            if (snap->thread_ctx_tokens > 0 && snap->thread_ctx_window > 0) {
                float ratio = (float)snap->thread_ctx_tokens / (float)snap->thread_ctx_window;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                int pct = (int)(ratio * 100.0f + 0.5f);
                char ctx_s[24], win_s[24], right_s[64];
                format_tokens_compact(snap->thread_ctx_tokens, ctx_s, sizeof ctx_s);
                format_tokens_compact(snap->thread_ctx_window, win_s, sizeof win_s);
                snprintf(right_s, sizeof right_s, "%s / %s (%d%%)", ctx_s, win_s, pct);

                cui_column(ui, .width = CUI_FILL, .gap = 3, .pad_top = 2) {
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, "Context window", .size = 11.5f,
                                 .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        cui_spacer(ui);
                        cui_text(ui, right_s, .size = 11.5f,
                                 .color = color_for_percent(ui, pct),
                                 .tabular = true, .nowrap = true);
                    }
                    cui_progress(ui, ratio, .tone = tone_for_percent(pct),
                                 .height = 4.5f, .width = CUI_FILL);
                }
            }
        }
    }

    /* Mcode tracked activity breakdown across all 3 providers */
    static const char *const k_scope_labels[3] = { "Today", "7d", "All" };
    if (app->usage_scope < 0 || app->usage_scope > 2) app->usage_scope = 2;

    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, "Mcode activity", .font = CUI_FONT_MEDIUM, .size = 12.0f,
                 .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
        cui_spacer(ui);
        cui_segmented(ui, "usage-scope", k_scope_labels, 3, &app->usage_scope,
                      .size = CUI_SIZE_SM);
    }

    const usage_scope_stats *sc = &snap->scopes[app->usage_scope];
    int64_t sc_total = sc->total.input_tokens + sc->total.output_tokens;

    if (sc->total.turns == 0) {
        cui_text(ui, "No agent turns recorded in this period.",
                 .size = 12.0f, .color = CUI_COLOR_TEXT_MUTED);
    } else {
        char tot_s[24];
        format_tokens_compact(sc_total, tot_s, sizeof tot_s);

        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            char sum_lbl[48];
            snprintf(sum_lbl, sizeof sum_lbl, "%s tokens", tot_s);
            cui_text(ui, sum_lbl, .font = CUI_FONT_MEDIUM, .size = 12.5f,
                     .color = CUI_COLOR_FOREGROUND, .tabular = true, .nowrap = true);
            if (sc->total.cost_usd > 0.0) {
                char cost_s[24];
                format_cost_usd(sc->total.cost_usd, cost_s, sizeof cost_s);
                cui_badge(ui, cost_s, .tone = CUI_TONE_NEUTRAL, .size = CUI_SIZE_SM);
            }
            cui_spacer(ui);
            char cnt_lbl[48];
            snprintf(cnt_lbl, sizeof cnt_lbl, "%d %s · %d %s",
                     sc->total.turns, sc->total.turns == 1 ? "turn" : "turns",
                     sc->total.threads, sc->total.threads == 1 ? "thread" : "threads");
            cui_text(ui, cnt_lbl, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                     .tabular = true, .nowrap = true);
        }

        /* Show all 3 providers in the activity breakdown */
        cui_column(ui, .width = CUI_FILL, .gap = 7, .pad_top = 2) {
            for (int ag = 0; ag < APP_AGENT_COUNT; ++ag) {
                const usage_bucket *ab = &sc->by_agent[ag];
                int64_t ag_tot = ab->input_tokens + ab->output_tokens;
                float share = sc_total > 0 ? (float)ag_tot / (float)sc_total : 0.0f;
                char atok[24];
                format_tokens_compact(ag_tot, atok, sizeof atok);

                char rmeta[64];
                if (ab->turns == 0) {
                    snprintf(rmeta, sizeof rmeta, "0 turns");
                } else if (ab->cost_usd > 0.0) {
                    char acost[24];
                    format_cost_usd(ab->cost_usd, acost, sizeof acost);
                    snprintf(rmeta, sizeof rmeta, "%s · %s", atok, acost);
                } else {
                    snprintf(rmeta, sizeof rmeta, "%s · %d %s",
                             atok, ab->turns, ab->turns == 1 ? "turn" : "turns");
                }

                cui_push_id_int(ui, ag);
                cui_column(ui, .width = CUI_FILL, .gap = 3) {
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        mcode_ui_draw_provider_icon(app, ui, (app_agent_kind)ag, 12.5f,
                                                    ab->turns > 0 ? CUI_COLOR_TEXT_SECONDARY
                                                                  : CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, mcode_agent_label((app_agent_kind)ag),
                                 .size = 11.5f,
                                 .color = ab->turns > 0 ? CUI_COLOR_FOREGROUND
                                                        : CUI_COLOR_TEXT_MUTED,
                                 .nowrap = true);
                        cui_spacer(ui);
                        cui_text(ui, rmeta, .size = 11.5f,
                                 .color = ab->turns > 0 ? CUI_COLOR_TEXT_SECONDARY
                                                        : CUI_COLOR_TEXT_MUTED,
                                 .tabular = true, .nowrap = true);
                    }
                    cui_progress(ui, share, .height = 4.0f, .width = CUI_FILL);
                }
                cui_pop_id(ui);
            }
        }
    }
}

static void draw_usage_popover_body(mcode_app *app, cui_ctx *ui, mcode_thread *thread,
                                    const usage_snapshot *snap, int64_t now_epoch) {
    static const char *const k_tab_labels[2] = { "Limits", "Activity" };
    if (app->usage_tab < 0 || app->usage_tab > 1) app->usage_tab = 0;

    cui_column(ui, .width = CUI_FILL, .gap = 8) {
        /* Header with tab switcher + refresh + settings */
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_segmented(ui, "usage-tab", k_tab_labels, 2, &app->usage_tab,
                          .size = CUI_SIZE_SM);
            cui_spacer(ui);
            bool any_loading = mcode_usage_tracker_is_loading(&app->usage);
            if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .disabled = any_loading,
                                .tooltip = "Refresh Claude, Codex, and Antigravity usage")) {
                mcode_usage_tracker_refresh(&app->usage, &app->cli_status, is_agy_busy(app));
                app->usage_checked_at = mcode_now_mono();
            }
            if (cui_icon_button(ui, CUI_ICON_SETTINGS, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .tooltip = "Configure agent CLIs in Settings")) {
                cui_popover_close(ui, "mcode-usage-pop");
                app->chrome->settings_open = true;
                app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
            }
        }

        /* Scrollable interior capped inside CUI's 420pt popover height limit */
        cui_column(ui, .id = "usage-pop-scroll", .width = CUI_FILL,
                   .max_height = 358.0f, .scroll_y = true, .gap = 7) {
            if (app->usage_tab == 0) {
                /* Show all 3 providers side-by-side: Codex, Antigravity, Claude */
                draw_provider_limit_card(app, ui, APP_AGENT_CODEX, snap, now_epoch);
                draw_provider_limit_card(app, ui, APP_AGENT_AGY, snap, now_epoch);
                draw_provider_limit_card(app, ui, APP_AGENT_CLAUDE, snap, now_epoch);
            } else {
                draw_usage_activity_tab(app, ui, thread, snap);
            }
        }
    }
}

static void draw_bar_provider_segment(mcode_app *app, cui_ctx *ui, app_agent_kind agent,
                                      const mcode_thread *thread,
                                      float vw, float text_sz, cui_color fg) {
    const mcode_provider_quota *q = &app->usage.quotas[agent];
    const mcode_rate_window *prim = &q->primary;
    const mcode_rate_window *sec = &q->secondary;
    if (agent == APP_AGENT_AGY) {
        const mcode_quota_group *ag_grp = select_agy_bar_group(app, thread);
        if (ag_grp) {
            prim = &ag_grp->primary;
            sec = &ag_grp->secondary;
        }
    }

    bool has_prim = (q->available && prim->available);
    bool has_sec = (q->available && sec->available);

    int peak = 0;
    if (has_prim && prim->used_percent > peak) peak = prim->used_percent;
    if (has_sec && sec->used_percent > peak) peak = sec->used_percent;

    cui_color icon_col = (peak >= 90) ? cui_color_of(ui, CUI_COLOR_DANGER)
                       : (peak >= 70) ? cui_color_of(ui, CUI_COLOR_WARNING)
                       : (q->auth_checked && !q->logged_in) ? cui_color_of(ui, CUI_COLOR_TEXT_MUTED)
                                                            : fg;

    cui_push_id_int(ui, (int)agent);
    cui_row(ui, .gap = 5, .align = CUI_ALIGN_CENTER) {
        mcode_ui_draw_provider_icon(app, ui, agent, 12.0f, icon_col);

        if (has_prim || has_sec) {
            const mcode_rate_window *first = has_prim ? prim : sec;
            int first_left = remaining_percent(first->used_percent);
            char txt[32];
            if (vw >= 1120.0f) {
                char wlbl[12];
                format_window_short(first->window_mins, has_prim ? "5h" : "7d", wlbl, sizeof wlbl);
                snprintf(txt, sizeof txt, "%s %d%%", wlbl, first_left);
            } else {
                snprintf(txt, sizeof txt, "%d%%", first_left);
            }
            cui_text(ui, txt, .size = text_sz, .color = fg, .tabular = true, .nowrap = true);
            if (vw >= 840.0f) {
                cui_progress(ui, (float)first_left / 100.0f,
                             .tone = tone_for_percent(first->used_percent),
                             .height = 4.0f, .width = CUI_PX(28));
            }
            if (has_prim && has_sec && vw >= 1220.0f) {
                int sec_left = remaining_percent(sec->used_percent);
                char w2[12], txt2[24];
                format_window_short(sec->window_mins, "7d", w2, sizeof w2);
                snprintf(txt2, sizeof txt2, "%s %d%%", w2, sec_left);
                cui_text(ui, txt2, .size = text_sz, .color = CUI_COLOR_TEXT_MUTED,
                         .tabular = true, .nowrap = true);
            }
        } else if (q->loading) {
            cui_spinner(ui, 10.0f);
        } else {
            /* No active rate-limit window yet (e.g. not signed in); never fall back to token counts in the bar. */
            cui_text(ui, "—", .size = text_sz, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
        }
    }
    cui_pop_id(ui);
}

void mcode_ui_draw_status_usage(mcode_app *app, cui_ctx *ui, mcode_thread *thread) {
    if (!app || !ui) return;

    int64_t now_epoch = mcode_now_epoch();
    usage_snapshot snap;
    compute_usage_snapshot(app, thread, now_epoch, &snap);

    cui_vec2 vp = cui_viewport_size(ui);
    float vw = vp.x > 0.0f ? vp.x : 1280.0f;

    cui_id btn_id = cui_make_id(ui, "sb-usage-track");
    cui_rect prev_r = cui_get_rect(ui, btn_id);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = prev_r.w > 0.0f &&
               mp.x >= prev_r.x && mp.x <= prev_r.x + prev_r.w &&
               mp.y >= prev_r.y && mp.y <= prev_r.y + prev_r.h;
    bool prs = hov && cui_mouse_down(ui, CUI_MOUSE_LEFT);
    bool open = cui_popover_is_open(ui, "mcode-usage-pop");

    cui_color bg = prs ? cui_alpha(cui_color_of(ui, CUI_COLOR_FOREGROUND), 0.12f)
                 : (open || hov) ? cui_color_of(ui, CUI_COLOR_HOVER)
                                 : CUI_RGBA(0x00000001u);
    cui_color fg = (open || hov) ? cui_color_of(ui, CUI_COLOR_FOREGROUND)
                                 : cui_color_of(ui, CUI_COLOR_TEXT_SECONDARY);
    float text_sz = cui_theme_get(ui)->text_sm;

    bool clicked = cui_card_begin(ui, &(cui_card_opts){
        .id = "sb-usage-track",
        .dir = CUI_ROW,
        .align = CUI_ALIGN_CENTER,
        .height = CUI_PX(22),
        .pad_x = 7,
        .pad_y = 0,
        .gap = 8,
        .hoverable = true,
        .border = -1.0f,
        .radius = 4.0f,
        .bg = bg,
    });

    int segments = 0;

    /* All installed providers' live usage right in the bottom bar: Claude, Codex, Antigravity */
    static const app_agent_kind k_order[APP_AGENT_COUNT] = {
        APP_AGENT_CLAUDE, APP_AGENT_CODEX, APP_AGENT_AGY
    };
    for (int oi = 0; oi < APP_AGENT_COUNT; ++oi) {
        app_agent_kind ag = k_order[oi];
        if (!app->cli_status.available[ag]) continue;
        if (segments > 0) {
            cui_box(ui, .width = CUI_PX(1), .height = CUI_PX(11),
                    .bg = CUI_COLOR_BORDER) {}
        }
        draw_bar_provider_segment(app, ui, ag, thread, vw, text_sz, fg);
        ++segments;
    }

    if (segments == 0) {
        cui_icon(ui, CUI_ICON_CHART_BAR, 12.0f, fg);
        cui_text(ui, "Usage", .size = text_sz, .color = fg, .nowrap = true);
    }

    cui_card_end(ui);

    if (clicked) {
        cui_popover_toggle(ui, "mcode-usage-pop");
    }

    cui_popover(ui, "mcode-usage-pop", .placement = CUI_PLACE_TOP_START,
                .width = 412, .pad = 12, .gap = 8) {
        draw_usage_popover_body(app, ui, thread, &snap, now_epoch);
    }

    /* Show any missing CLIs cleanly next to the usage strip so they still open Settings */
    for (int i = 0; i < APP_AGENT_COUNT; ++i) {
        if (app->cli_status.available[i]) continue;
        char lbl[48];
        snprintf(lbl, sizeof lbl, "%s###cli-%d", mcode_agent_short_label((app_agent_kind)i), i);
        if (cui_status_bar_item(ui, lbl, .icon = CUI_ICON_CIRCLE_X, .tone = CUI_TONE_NEUTRAL,
                                .tooltip = "Not detected (configure in Settings)")) {
            app->chrome->settings_open = true;
            app->chrome->settings_section = SETTINGS_SECTION_GENERAL;
        }
    }
}

/* ---- Chatbox Circular Context / Token Usage Ring + Hover Card ---- */

typedef struct {
    float ratio;
    int pct;
    bool hovered;
} composer_ring_paint;

static void paint_composer_usage_ring(cui_ctx *ui, cui_rect r, void *user) {
    const composer_ring_paint *p = (const composer_ring_paint *)user;
    bool dark = (cui_get_theme_mode(ui) == CUI_THEME_DARK);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);

    if (p->hovered) {
        float rad = cui_theme_get(ui)->radius_surface - 10.0f;
        if (rad < 6.0f) rad = 6.0f;
        cui_draw_rect(ui, r, CUI_CORNERS(rad), cui_alpha(fg, 0.08f));
    }

    cui_vec2 center = { r.x + r.w * 0.5f, r.y + r.h * 0.5f };
    float radius = 7.2f;
    float thick = 2.0f;
    static const float k_two_pi = 6.28318530718f;
    static const float k_start = -1.57079632679f; /* 12 o'clock */

    cui_color track_col = cui_alpha(fg, dark ? (p->hovered ? 0.24f : 0.16f)
                                             : (p->hovered ? 0.20f : 0.14f));
    cui_draw_arc(ui, center, radius, 0.0f, k_two_pi, thick, track_col);

    if (p->ratio > 0.0005f) {
        float sweep = p->ratio;
        if (sweep < 0.035f) sweep = 0.035f;
        if (sweep > 1.0f) sweep = 1.0f;

        cui_color fill_col;
        if (p->pct >= 90) {
            fill_col = cui_color_of(ui, CUI_COLOR_DANGER);
        } else if (p->pct >= 70) {
            fill_col = cui_color_of(ui, CUI_COLOR_WARNING);
        } else if (cui_get_accent(ui) != CUI_ACCENT_NEUTRAL) {
            fill_col = cui_color_of(ui, CUI_COLOR_ACCENT);
        } else {
            fill_col = cui_alpha(fg, p->hovered ? 0.92f : 0.72f);
        }
        cui_draw_arc(ui, center, radius, k_start, k_start + k_two_pi * sweep, thick, fill_col);
    }
}

void mcode_ui_draw_composer_usage_ring(mcode_app *app, cui_ctx *ui, const mcode_thread *thread,
                                       cui_placement hover_placement) {
    if (!app || !ui || !thread) return;

    int64_t in_tok = 0, out_tok = 0, cached_tok = 0;
    double cost_usd = 0.0;
    int64_t ctx_tok = 0, ctx_win = 0;
    int64_t last_in_tok = 0;
    for (int i = 0; i < thread->turn_count; ++i) {
        const mcode_turn *tr = &thread->turns[i];
        in_tok += tr->input_tokens;
        out_tok += tr->output_tokens;
        cached_tok += tr->cached_tokens;
        cost_usd += tr->cost_usd;
        if (tr->context_tokens > 0) ctx_tok = tr->context_tokens;
        if (tr->context_window > 0) ctx_win = tr->context_window;
        if (tr->input_tokens > 0) last_in_tok = tr->input_tokens;
    }

    const app_settings *settings = app->chrome ? app_chrome_settings(app->chrome) : NULL;
    if (ctx_win <= 0) {
        ctx_win = mcode_model_context_window(&app->models, thread, settings);
    }

    bool estimated_ctx = false;
    int64_t eff_ctx = ctx_tok;
    if (eff_ctx <= 0 && last_in_tok > 0) {
        eff_ctx = last_in_tok;
        estimated_ctx = true;
    }

    float target_ratio = 0.0f;
    int pct = 0;
    if (eff_ctx > 0 && ctx_win > 0) {
        target_ratio = (float)eff_ctx / (float)ctx_win;
        if (target_ratio < 0.0f) target_ratio = 0.0f;
        if (target_ratio > 1.0f) target_ratio = 1.0f;
        pct = (int)(target_ratio * 100.0f + 0.5f);
    }

    cui_id rid = cui_make_id(ui, "composer-ctx-ring");
    cui_rect prev = cui_get_rect(ui, rid);
    cui_vec2 mp = cui_mouse_pos(ui);
    bool hov = prev.w > 0.0f &&
               mp.x >= prev.x && mp.x <= prev.x + prev.w &&
               mp.y >= prev.y && mp.y <= prev.y + prev.h;

    float shown_ratio = cui_animate(ui, rid ^ 0xA2C1u, target_ratio);
    composer_ring_paint spec = { shown_ratio, pct, hov };
    (void)cui_canvas(ui, "composer-ctx-ring", CUI_PX(32), CUI_PX(32),
                     paint_composer_usage_ring, &spec, sizeof spec);

    cui_hover_card(ui, "composer-ctx-hover", .placement = hover_placement,
                   .width = 272, .delay = 0.12f) {
        char model_name[128] = "";
        mcode_model_format_display(&app->models, thread, settings,
                                   model_name, sizeof model_name, NULL, 0);

        char pct_s[16], ctx_s[24], win_s[24], tot_s[24], in_s[24], out_s[24];
        snprintf(pct_s, sizeof pct_s, "%d%%", pct);
        format_tokens_compact(eff_ctx, ctx_s, sizeof ctx_s);
        format_tokens_compact(ctx_win, win_s, sizeof win_s);
        int64_t total_tok = in_tok + out_tok;
        format_tokens_compact(total_tok, tot_s, sizeof tot_s);
        format_tokens_compact(in_tok, in_s, sizeof in_s);
        format_tokens_compact(out_tok, out_s, sizeof out_s);

        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            /* Context window header + ring/bar summary */
            cui_column(ui, .width = CUI_FILL, .gap = 5) {
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "Context window", .font = CUI_FONT_MEDIUM, .size = 12.5f,
                             .color = CUI_COLOR_FOREGROUND, .nowrap = true);
                    cui_spacer(ui);
                    cui_text(ui, pct_s, .font = CUI_FONT_MEDIUM, .size = 12.5f,
                             .color = color_for_percent(ui, pct),
                             .tabular = true, .nowrap = true);
                }
                cui_tone ctx_tone = pct >= 90 ? CUI_TONE_DANGER
                                  : pct >= 70 ? CUI_TONE_WARNING
                                              : CUI_TONE_NEUTRAL;
                cui_progress(ui, target_ratio, .tone = ctx_tone,
                             .height = 5.0f, .width = CUI_FILL);
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    char ratio_lbl[64];
                    if (ctx_win > 0) {
                        snprintf(ratio_lbl, sizeof ratio_lbl, "%s / %s", ctx_s, win_s);
                    } else {
                        snprintf(ratio_lbl, sizeof ratio_lbl, "%s tokens", ctx_s);
                    }
                    cui_text(ui, ratio_lbl, .size = 11.5f, .color = CUI_COLOR_TEXT_SECONDARY,
                             .tabular = true, .nowrap = true);
                    cui_spacer(ui);
                    if (model_name[0]) {
                        cui_text(ui, model_name, .size = 11.0f, .color = CUI_COLOR_TEXT_MUTED,
                                 .nowrap = true);
                    }
                }
            }

            cui_separator(ui);

            if (thread->turn_count == 0 && total_tok == 0) {
                cui_text(ui, "Send a message to start tracking thread tokens.",
                         .size = 11.5f, .color = CUI_COLOR_TEXT_MUTED);
            } else {
                cui_column(ui, .width = CUI_FILL, .gap = 4) {
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, "Thread tokens", .size = 11.5f,
                                 .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        cui_spacer(ui);
                        cui_text(ui, tot_s, .font = CUI_FONT_MEDIUM, .size = 12.0f,
                                 .color = CUI_COLOR_FOREGROUND, .tabular = true, .nowrap = true);
                    }
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, "Input / Output", .size = 11.0f,
                                 .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                        cui_spacer(ui);
                        char io_s[64];
                        snprintf(io_s, sizeof io_s, "%s / %s", in_s, out_s);
                        cui_text(ui, io_s, .size = 11.0f, .color = CUI_COLOR_TEXT_SECONDARY,
                                 .tabular = true, .nowrap = true);
                    }
                    if (cached_tok > 0) {
                        char cache_s[24];
                        format_tokens_compact(cached_tok, cache_s, sizeof cache_s);
                        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                            cui_text(ui, "Cached", .size = 11.0f,
                                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                            cui_spacer(ui);
                            cui_text(ui, cache_s, .size = 11.0f, .color = CUI_COLOR_TEXT_SECONDARY,
                                     .tabular = true, .nowrap = true);
                        }
                    }
                    if (cost_usd > 0.0) {
                        char cost_s[24];
                        format_cost_usd(cost_usd, cost_s, sizeof cost_s);
                        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                            cui_text(ui, "Cost", .size = 11.0f,
                                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                            cui_spacer(ui);
                            cui_text(ui, cost_s, .size = 11.0f, .color = CUI_COLOR_TEXT_SECONDARY,
                                     .tabular = true, .nowrap = true);
                        }
                    }
                    cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, "Turns", .size = 11.0f,
                                 .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                        cui_spacer(ui);
                        char turns_s[24];
                        snprintf(turns_s, sizeof turns_s, "%d", thread->turn_count);
                        cui_text(ui, turns_s, .size = 11.0f, .color = CUI_COLOR_TEXT_SECONDARY,
                                 .tabular = true, .nowrap = true);
                    }
                    if (estimated_ctx) {
                        cui_text(ui, "Context estimated from last turn input.",
                                 .size = 10.5f, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
        }
    }
}
