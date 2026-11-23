#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_types.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

void mcode_str_init(mcode_str *s) {
    if (!s) return;
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void mcode_str_free(mcode_str *s) {
    if (!s) return;
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void mcode_str_clear(mcode_str *s) {
    if (!s) return;
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

bool mcode_str_append(mcode_str *s, const char *data, size_t n) {
    if (!s) return false;
    if (!data || n == 0) return true;
    size_t needed = s->len + n + 1;
    if (needed > s->cap) {
        size_t new_cap = s->cap ? s->cap : 128;
        while (new_cap < needed) new_cap *= 2;
        char *buf = realloc(s->data, new_cap);
        if (!buf) return false;
        s->data = buf;
        s->cap = new_cap;
    }
    memcpy(s->data + s->len, data, n);
    s->len += n;
    s->data[s->len] = '\0';
    return true;
}

bool mcode_str_append_cstr(mcode_str *s, const char *cstr) {
    if (!cstr) return true;
    return mcode_str_append(s, cstr, strlen(cstr));
}

bool mcode_str_set(mcode_str *s, const char *cstr) {
    mcode_str_clear(s);
    return mcode_str_append_cstr(s, cstr);
}

const char *mcode_str_cstr(const mcode_str *s) {
    return (s && s->data) ? s->data : "";
}

double mcode_now_mono(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t tb = { 0, 0 };
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return (double)t * ((double)tb.numer / (double)tb.denom) * 1e-9;
#elif defined(_WIN32)
    return (double)clock() / (double)CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

int64_t mcode_now_epoch(void) {
    return (int64_t)time(NULL);
}

void mcode_gen_id(char *out, size_t cap, const char *prefix) {
    if (!out || cap == 0) return;
    static uint32_t counter = 0;
    ++counter;
    uint64_t mix = ((uint64_t)time(NULL) << 20) ^ ((uint64_t)clock() << 8) ^ (uint64_t)counter;
    mix ^= mix >> 33;
    mix *= 0xff51afd7ed558ccdULL;
    mix ^= mix >> 33;
    if (prefix && prefix[0]) {
        snprintf(out, cap, "%s_%08llx", prefix, (unsigned long long)(mix & 0xffffffffULL));
    } else {
        snprintf(out, cap, "%08llx", (unsigned long long)(mix & 0xffffffffULL));
    }
}

void mcode_slugify(const char *input, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!input) {
        snprintf(out, cap, "task");
        return;
    }
    size_t w = 0;
    bool prev_dash = true;
    for (const unsigned char *p = (const unsigned char *)input; *p && w + 1 < cap && w < 32; ++p) {
        if (isalnum(*p)) {
            out[w++] = (char)tolower(*p);
            prev_dash = false;
        } else if (!prev_dash) {
            out[w++] = '-';
            prev_dash = true;
        }
    }
    while (w > 0 && out[w - 1] == '-') --w;
    out[w] = '\0';
    if (w == 0) snprintf(out, cap, "task");
}

mcode_turn *mcode_thread_add_turn(mcode_thread *thread, const char *prompt) {
    if (!thread) return NULL;
    if (thread->turn_count >= thread->turn_cap) {
        int new_cap = thread->turn_cap ? thread->turn_cap * 2 : 8;
        mcode_turn *grown = realloc(thread->turns, (size_t)new_cap * sizeof *grown);
        if (!grown) return NULL;
        thread->turns = grown;
        thread->turn_cap = new_cap;
    }
    mcode_turn *t = &thread->turns[thread->turn_count++];
    memset(t, 0, sizeof *t);
    mcode_str_init(&t->prompt);
    mcode_str_set(&t->prompt, prompt ? prompt : "");
    t->created_at = mcode_now_epoch();
    return t;
}

mcode_block *mcode_turn_add_block(mcode_turn *turn, mcode_block_kind kind) {
    if (!turn) return NULL;
    if (turn->block_count >= turn->block_cap) {
        int new_cap = turn->block_cap ? turn->block_cap * 2 : 8;
        mcode_block *grown = realloc(turn->blocks, (size_t)new_cap * sizeof *grown);
        if (!grown) return NULL;
        turn->blocks = grown;
        turn->block_cap = new_cap;
    }
    mcode_block *b = &turn->blocks[turn->block_count++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    mcode_str_init(&b->text);
    mcode_str_init(&b->input);
    mcode_str_init(&b->output);
    b->started_mono = mcode_now_mono();
    return b;
}

mcode_block *mcode_turn_find_tool(mcode_turn *turn, const char *tool_id) {
    if (!turn || !tool_id || !tool_id[0]) return NULL;
    for (int i = turn->block_count - 1; i >= 0; --i) {
        mcode_block *b = &turn->blocks[i];
        if (b->kind == MCODE_BLOCK_TOOL && strcmp(b->tool_id, tool_id) == 0) {
            return b;
        }
    }
    return NULL;
}

void mcode_turn_free(mcode_turn *turn) {
    if (!turn) return;
    mcode_str_free(&turn->prompt);
    for (int i = 0; i < turn->block_count; ++i) {
        mcode_str_free(&turn->blocks[i].text);
        mcode_str_free(&turn->blocks[i].input);
        mcode_str_free(&turn->blocks[i].output);
    }
    free(turn->blocks);
    turn->blocks = NULL;
    turn->block_count = 0;
    turn->block_cap = 0;
}

void mcode_thread_free_runtime(mcode_thread *thread) {
    if (!thread) return;
    free(thread->diff_files);
    thread->diff_files = NULL;
    thread->diff_file_count = 0;
    free(thread->diff_old_text);
    thread->diff_old_text = NULL;
    free(thread->diff_new_text);
    thread->diff_new_text = NULL;
    thread->diff_loaded_path[0] = '\0';
}

void mcode_thread_free(mcode_thread *thread) {
    if (!thread) return;
    for (int i = 0; i < thread->turn_count; ++i) {
        mcode_turn_free(&thread->turns[i]);
    }
    free(thread->turns);
    thread->turns = NULL;
    thread->turn_count = 0;
    thread->turn_cap = 0;
    mcode_thread_free_runtime(thread);
}
