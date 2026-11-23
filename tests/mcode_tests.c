#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_agent.h"
#include "mcode_git.h"
#include "mcode_proc.h"
#include "mcode_store.h"
#include "mcode_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static void test_helpers(void) {
    char slug[64];
    mcode_slugify("Fix Auth Token Refresh!!", slug, sizeof slug);
    CHECK(strcmp(slug, "fix-auth-token-refresh") == 0);
    mcode_slugify("   ---   ", slug, sizeof slug);
    CHECK(strcmp(slug, "task") == 0);

    CHECK(strcmp(mcode_lang_from_path("src/main.c"), "c") == 0);
    CHECK(strcmp(mcode_lang_from_path("app/index.tsx"), "tsx") == 0);
    CHECK(strcmp(mcode_lang_from_path("CMakeLists.txt"), "cmake") == 0);
    CHECK(strcmp(mcode_lang_from_path("README.md"), "markdown") == 0);
    CHECK(strcmp(mcode_lang_from_path("unknown.xyz"), "text") == 0);

    char pr_url[512];
    CHECK(mcode_git_build_pr_url("git@github.com:acme/mcode.git", "main", "mcode/fix-bug",
                                 "Fix bug", pr_url, sizeof pr_url));
    CHECK(strstr(pr_url, "https://github.com/acme/mcode/compare/main...mcode%2Ffix-bug?expand=1&title=Fix%20bug") != NULL);
}

static void test_claude_parser(void) {
    mcode_thread t;
    memset(&t, 0, sizeof t);
    mcode_turn *turn = mcode_thread_add_turn(&t, "Fix the bug");
    CHECK(turn != NULL);

    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"sess_claude_123\"}", false);
    CHECK(strcmp(t.session_id, "sess_claude_123") == 0);
    CHECK(t.status_text[0] == '\0');

    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Checking files...\"}}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\"}}", false);

    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\",\"input\":{\"file_path\":\"main.c\"}}]}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\",\"content\":\"int main(void) {}\",\"is_error\":false}]}}", false);

    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"All \"}}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"fixed!\"}}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"result\",\"is_error\":false,\"total_cost_usd\":0.0125,\"usage\":{\"input_tokens\":1000,\"output_tokens\":250}}", false);

    CHECK(turn->block_count == 3);
    CHECK(turn->blocks[0].kind == MCODE_BLOCK_REASONING);
    CHECK(!turn->blocks[0].thinking);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[0].text), "Checking files...") == 0);
    CHECK(turn->blocks[1].kind == MCODE_BLOCK_TOOL);
    CHECK(strcmp(turn->blocks[1].tool_name, "Read") == 0);
    CHECK(strcmp(turn->blocks[1].tool_detail, "main.c") == 0);
    CHECK(turn->blocks[1].status == CUI_TASK_DONE);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[1].output), "int main(void) {}") == 0);
    CHECK(turn->blocks[2].kind == MCODE_BLOCK_TEXT);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[2].text), "All fixed!") == 0);
    CHECK(turn->input_tokens == 1000);
    CHECK(turn->output_tokens == 250);

    mcode_thread_free(&t);
}

static void test_codex_parser(void) {
    mcode_thread t;
    memset(&t, 0, sizeof t);
    mcode_turn *turn = mcode_thread_add_turn(&t, "Run tests");

    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"thread.started\",\"thread_id\":\"codex_thr_99\"}", false);
    CHECK(strcmp(t.session_id, "codex_thr_99") == 0);
    CHECK(t.status_text[0] == '\0');
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"turn.started\"}", false);
    CHECK(t.status_text[0] == '\0');

    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.started\",\"item\":{\"id\":\"item_1\",\"type\":\"command_execution\",\"command\":\"ctest\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_1\",\"type\":\"command_execution\",\"command\":\"ctest\",\"aggregated_output\":\"100% tests passed\",\"exit_code\":0}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_rg\",\"type\":\"command_execution\",\"command\":\"/bin/zsh -lc 'ls -la && rg --files -g *.html'\",\"aggregated_output\":\"total 0\\n\",\"exit_code\":1,\"status\":\"completed\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_fail\",\"type\":\"command_execution\",\"command\":\"ctest\",\"aggregated_output\":\"1 test failed\",\"exit_code\":1,\"status\":\"failed\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_2\",\"type\":\"agent_message\",\"text\":\"Tests pass.\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":400,\"output_tokens\":80}}", false);

    CHECK(turn->block_count == 4);
    CHECK(turn->blocks[0].kind == MCODE_BLOCK_TOOL);
    CHECK(strcmp(turn->blocks[0].tool_name, "bash") == 0);
    CHECK(turn->blocks[0].status == CUI_TASK_DONE);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[0].output), "100% tests passed") != NULL);
    CHECK(turn->blocks[1].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[1].status == CUI_TASK_DONE);
    CHECK(turn->blocks[2].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[2].status == CUI_TASK_FAILED);
    CHECK(turn->blocks[3].kind == MCODE_BLOCK_TEXT);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[3].text), "Tests pass.") != NULL);
    CHECK(turn->input_tokens == 400);
    CHECK(turn->output_tokens == 80);

    mcode_thread_free(&t);
}

static void test_agy_parser(void) {
    mcode_thread t;
    memset(&t, 0, sizeof t);
    mcode_turn *turn = mcode_thread_add_turn(&t, "Inspect repo");

    mcode_agent_parse_line_for_test(APP_AGENT_AGY, &t, turn,
        "{\"type\":\"thinking\",\"conversation_id\":\"agy_conv_7\",\"delta\":\"Planning...\"}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_AGY, &t, turn,
        "{\"type\":\"tool_call\",\"id\":\"call_1\",\"name\":\"grep_search\",\"args\":{\"query\":\"main\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_AGY, &t, turn,
        "{\"type\":\"tool_result\",\"call_id\":\"call_1\",\"output\":\"main.c:1\",\"is_error\":false}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_AGY, &t, turn,
        "{\"type\":\"text\",\"delta\":\"Found main.c.\"}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_AGY, &t, turn,
        "Plain fallback line", false);

    CHECK(strcmp(t.session_id, "agy_conv_7") == 0);
    CHECK(turn->block_count == 3);
    CHECK(turn->blocks[0].kind == MCODE_BLOCK_REASONING);
    CHECK(turn->blocks[1].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[1].status == CUI_TASK_DONE);
    CHECK(turn->blocks[2].kind == MCODE_BLOCK_TEXT);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[2].text), "Found main.c.") != NULL);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[2].text), "Plain fallback line") != NULL);

    mcode_thread_free(&t);
}

static void test_store_roundtrip(void) {
    char path[512];
#ifndef _WIN32
    snprintf(path, sizeof path, "/tmp/mcode_store_test_%d.json", (int)getpid());
#else
    snprintf(path, sizeof path, "mcode_store_test.json");
#endif
    remove(path);

    mcode_store s;
    mcode_store_init(&s);
    mcode_project *p = mcode_store_add_project(&s, "/tmp/demo-repo", "demo-repo");
    CHECK(p != NULL);
    mcode_thread *t = mcode_store_add_thread(&s, p, "Implement login", APP_AGENT_CODEX,
                                             "o3", true, false, "/tmp/wt-demo", "mcode/login");
    CHECK(t != NULL);
    mcode_turn *tr = mcode_thread_add_turn(t, "Build login flow");
    mcode_block *b = mcode_turn_add_block(tr, MCODE_BLOCK_TEXT);
    mcode_str_set(&b->text, "Done implementing login.");
    s.show_terminal_panel = true;
    s.diff_inline_mode = true;

    CHECK(mcode_store_save(&s, path));
    mcode_store_free(&s);

    mcode_store loaded;
    mcode_store_init(&loaded);
    CHECK(mcode_store_load(&loaded, path));
    CHECK(loaded.project_count == 1);
    CHECK(strcmp(loaded.projects[0].name, "demo-repo") == 0);
    CHECK(loaded.thread_count == 1);
    CHECK(strcmp(loaded.threads[0].title, "Implement login") == 0);
    CHECK(loaded.threads[0].agent == APP_AGENT_CODEX);
    CHECK(loaded.threads[0].use_worktree);
    CHECK(loaded.threads[0].turn_count == 1);
    CHECK(strcmp(mcode_str_cstr(&loaded.threads[0].turns[0].blocks[0].text), "Done implementing login.") == 0);
    CHECK(loaded.show_terminal_panel);
    CHECK(loaded.diff_inline_mode);

    mcode_store_free(&loaded);
    remove(path);
}

#ifndef _WIN32
static void test_git_worktree_and_diff(void) {
    char repo[512];
    snprintf(repo, sizeof repo, "/tmp/mcode_git_test_%d", (int)getpid());
    mkdir(repo, 0755);

    int rc = -1;
    const char *init_cmd[] = { "git", "init", NULL };
    CHECK(mcode_proc_run_capture(init_cmd, repo, true, &rc, NULL, NULL) && rc == 0);
    const char *cfg1[] = { "git", "config", "user.email", "test@mcode.local", NULL };
    const char *cfg2[] = { "git", "config", "user.name", "Mcode Test", NULL };
    CHECK(mcode_proc_run_capture(cfg1, repo, true, &rc, NULL, NULL) && rc == 0);
    CHECK(mcode_proc_run_capture(cfg2, repo, true, &rc, NULL, NULL) && rc == 0);

    char fpath[600];
    snprintf(fpath, sizeof fpath, "%s/hello.c", repo);
    FILE *f = fopen(fpath, "wb");
    CHECK(f != NULL);
    fputs("int main(void) {\n    return 0;\n}\n", f);
    fclose(f);

    char err[256] = "";
    CHECK(mcode_git_commit_all(repo, "Initial commit", err, sizeof err));

    bool is_repo = false;
    char branch[128] = "";
    CHECK(mcode_git_inspect_repo(repo, &is_repo, branch, sizeof branch, NULL, 0));
    CHECK(is_repo && branch[0] != '\0');

    char wt_path[MCODE_PATH_MAX] = "";
    char wt_branch[MCODE_BRANCH_MAX] = "";
    CHECK(mcode_git_create_worktree(repo, "demo", "add greeting",
                                    wt_path, sizeof wt_path,
                                    wt_branch, sizeof wt_branch,
                                    err, sizeof err));
    CHECK(wt_path[0] != '\0');
    CHECK(strncmp(wt_branch, "mcode/add-greeting-", 19) == 0);

    /* Modify file inside isolated worktree */
    char wt_file[MCODE_PATH_MAX + 32];
    snprintf(wt_file, sizeof wt_file, "%s/hello.c", wt_path);
    f = fopen(wt_file, "wb");
    CHECK(f != NULL);
    fputs("#include <stdio.h>\nint main(void) {\n    puts(\"hi\");\n    return 0;\n}\n", f);
    fclose(f);

    mcode_diff_file *files = NULL;
    int count = 0, add = 0, rem = 0;
    CHECK(mcode_git_list_changes(wt_path, &files, &count, &add, &rem));
    CHECK(count == 1);
    CHECK(strcmp(files[0].path, "hello.c") == 0);
    CHECK(add >= 2);

    char *old_t = NULL, *new_t = NULL;
    CHECK(mcode_git_load_file_diff(wt_path, &files[0], &old_t, &new_t));
    CHECK(strstr(old_t, "return 0;") != NULL && strstr(old_t, "puts") == NULL);
    CHECK(strstr(new_t, "puts(\"hi\")") != NULL);
    free(old_t);
    free(new_t);
    free(files);

    CHECK(mcode_git_remove_worktree(repo, wt_path, err, sizeof err));

    char cleanup[600];
    snprintf(cleanup, sizeof cleanup, "rm -rf '%s'", repo);
    (void)system(cleanup);
}
#endif

static void test_model_catalog(void) {
    const char *codex_json =
        "{\"models\":["
        "{\"slug\":\"gpt-6.2-sol\",\"display_name\":\"GPT-6.2-Sol\",\"visibility\":\"list\",\"description\":\"Fast go-to model.\"},"
        "{\"slug\":\"gpt-hidden\",\"display_name\":\"Hidden\",\"visibility\":\"hide\",\"description\":\"Internal.\"},"
        "{\"slug\":\"gpt-6.2-astra\",\"display_name\":\"GPT-6.2-Astra\",\"visibility\":\"list\",\"description\":\"Complex coding.\"}"
        "]}";
    mcode_model_option opts[8];
    int n = mcode_agent_parse_codex_models_json(codex_json, strlen(codex_json), opts, 8);
    CHECK(n == 2);
    CHECK(strcmp(opts[0].id, "gpt-6.2-sol") == 0);
    CHECK(strcmp(opts[0].label, "GPT-6.2-Sol") == 0);
    CHECK(strcmp(opts[0].detail, "Fast go-to model.") == 0);
    CHECK(strcmp(opts[1].id, "gpt-6.2-astra") == 0);

    const char *agy_text =
        "\r\xe2\xa0\x8b Fetching available models...\rgemini-4-neon-high             Gemini 4 Neon (High)\n"
        "claude-opus-5-5-high           Claude Opus 5.5 (High)\n"
        "Not a model line\n";
    n = mcode_agent_parse_agy_models_text(agy_text, opts, 8);
    CHECK(n == 2);
    CHECK(strcmp(opts[0].id, "gemini-4-neon-high") == 0);
    CHECK(strcmp(opts[0].label, "Gemini 4 Neon (High)") == 0);
    CHECK(strcmp(opts[1].id, "claude-opus-5-5-high") == 0);
    CHECK(strcmp(opts[1].label, "Claude Opus 5.5 (High)") == 0);

    mcode_model_catalog cat;
    mcode_model_catalog_init(&cat);
    CHECK(cat.count[APP_AGENT_CLAUDE] >= 3);
    CHECK(cat.count[APP_AGENT_CODEX] >= 1);
    CHECK(cat.count[APP_AGENT_AGY] >= 1);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, "opus"), "Claude Opus") == 0);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, "custom-id"), "custom-id") == 0);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, ""), "Default") == 0);

    app_settings s;
    memset(&s, 0, sizeof s);
    snprintf(s.mcode.default_codex_model, sizeof s.mcode.default_codex_model, "gpt-6.2-sol");
    mcode_thread t;
    memset(&t, 0, sizeof t);
    t.agent = APP_AGENT_CODEX;
    CHECK(strcmp(mcode_agent_effective_model(&t, &s), "gpt-6.2-sol") == 0);
    snprintf(t.model, sizeof t.model, "gpt-6.2-astra");
    CHECK(strcmp(mcode_agent_effective_model(&t, &s), "gpt-6.2-astra") == 0);

    mcode_model_catalog_free(&cat);
}

int main(void) {
    test_helpers();
    test_claude_parser();
    test_codex_parser();
    test_agy_parser();
    test_model_catalog();
    test_store_roundtrip();
#ifndef _WIN32
    test_git_worktree_and_diff();
#endif
    if (failures > 0) {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    puts("All Mcode tests passed.");
    return 0;
}
