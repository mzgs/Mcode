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
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"Bash\",\"input\":{\"command\":\"ls -a /tmp/nonexistent-agents-md\"}}]}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_2\",\"content\":\"Exit code 1\\nls: /tmp/nonexistent-agents-md: No such file or directory\",\"is_error\":true}]}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"All \"}}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"fixed!\"}}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CLAUDE, &t, turn,
        "{\"type\":\"result\",\"is_error\":false,\"total_cost_usd\":0.0125,\"usage\":{\"input_tokens\":1000,\"output_tokens\":250}}", false);

    CHECK(turn->block_count == 4);
    CHECK(turn->blocks[0].kind == MCODE_BLOCK_REASONING);
    CHECK(!turn->blocks[0].thinking);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[0].text), "Checking files...") == 0);
    CHECK(turn->blocks[1].kind == MCODE_BLOCK_TOOL);
    CHECK(strcmp(turn->blocks[1].tool_name, "Read") == 0);
    CHECK(strcmp(turn->blocks[1].tool_detail, "main.c") == 0);
    CHECK(turn->blocks[1].status == CUI_TASK_DONE);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[1].output), "int main(void) {}") == 0);
    CHECK(turn->blocks[2].kind == MCODE_BLOCK_TOOL);
    CHECK(strcmp(turn->blocks[2].tool_name, "Bash") == 0);
    CHECK(turn->blocks[2].status == CUI_TASK_DONE);
    CHECK(turn->blocks[3].kind == MCODE_BLOCK_TEXT);
    CHECK(strcmp(mcode_str_cstr(&turn->blocks[3].text), "All fixed!") == 0);
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
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_ls_probe\",\"type\":\"command_execution\",\"command\":\"/bin/zsh -lc \\\"git status --short\\nrg --files --hidden -g AGENTS.md -g '\\\"'!.git'\\\"'\\nnl -ba index.html\\nls -a /Users/demo/Downloads/AGENTS.md /Users/demo/AGENTS.md\\\"\",\"aggregated_output\":\"?? index.html\\nls: /Users/demo/AGENTS.md: No such file or directory\\n\",\"exit_code\":1,\"status\":\"failed\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_git_nongit\",\"type\":\"command_execution\",\"command\":\"/bin/zsh -lc 'git status --short'\",\"aggregated_output\":\"fatal: not a git repository (or any of the parent directories): .git\\n\",\"exit_code\":128,\"status\":\"failed\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_fail\",\"type\":\"command_execution\",\"command\":\"ctest\",\"aggregated_output\":\"1 test failed\",\"exit_code\":1,\"status\":\"failed\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"item_2\",\"type\":\"agent_message\",\"text\":\"Tests pass.\"}}", false);
    mcode_agent_parse_line_for_test(APP_AGENT_CODEX, &t, turn,
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":400,\"output_tokens\":80}}", false);

    CHECK(turn->block_count == 6);
    CHECK(turn->blocks[0].kind == MCODE_BLOCK_TOOL);
    CHECK(strcmp(turn->blocks[0].tool_name, "bash") == 0);
    CHECK(turn->blocks[0].status == CUI_TASK_DONE);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[0].output), "100% tests passed") != NULL);
    CHECK(turn->blocks[1].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[1].status == CUI_TASK_DONE);
    CHECK(turn->blocks[2].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[2].status == CUI_TASK_DONE);
    CHECK(turn->blocks[3].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[3].status == CUI_TASK_DONE);
    CHECK(turn->blocks[4].kind == MCODE_BLOCK_TOOL);
    CHECK(turn->blocks[4].status == CUI_TASK_FAILED);
    CHECK(turn->blocks[5].kind == MCODE_BLOCK_TEXT);
    CHECK(strstr(mcode_str_cstr(&turn->blocks[5].text), "Tests pass.") != NULL);
    CHECK(turn->input_tokens == 400);
    CHECK(turn->output_tokens == 80);

    mcode_thread_free(&t);
}

static void test_benign_probe_detection(void) {
    /* Benign probes */
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc \"ls -a; rg --files -g 'AGENTS.md' -g '*.html' -g '\"'!*node_modules*'\"'\"",
        ".\n..\n", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc \"git status --short\nrg --files --hidden -g AGENTS.md -g '\"'!.git'\"'\nnl -ba index.html\nls -a /tmp/missing1 /tmp/missing2\"",
        "?? index.html\nls: /tmp/missing1: No such file or directory\n", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'git status --short; rg --files -g AGENTS.md'",
        "fatal: not a git repository (or any of the parent directories): .git\n", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'git status --short'",
        "fatal: not a git repository (or any of the parent directories): .git\n", 128));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'git -C /tmp rev-parse --verify main'",
        "fatal: Needed a single revision\n", 128));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'test -f AGENTS.md'", "", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc '[ -d build ]'", "", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'command -v pnpm'", "", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'cat index.html | rg missing'", "", 1));
    CHECK(mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'ls /tmp/no-such-file | head -n 5'",
        "ls: /tmp/no-such-file: No such file or directory\n", 1));

    /* Real failures that MUST remain failed */
    CHECK(!mcode_command_is_benign_probe_exit("ctest", "1 test failed\n", 1));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'ctest | tail -n 20'", "1 test failed\n", 1));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'set -e\nffmpeg -i a b\nls -a /tmp/x'",
        "ffmpeg: failed\n", 1));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'rg \"[unclosed\" src/'",
        "regex parse error:\n    [unclosed\n", 2));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'ls --bogus'",
        "ls: illegal option -- -\n", 1));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'ls /root/secret'",
        "ls: /root/secret: Permission denied\n", 1));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'git commit -m \"test\"'",
        "fatal: not a git repository\n", 128));
    CHECK(!mcode_command_is_benign_probe_exit(
        "/bin/zsh -lc 'ls /tmp/foo'",
        "zsh: command not found: ls\n", 127));

    /* Claude Bash tool JSON input + "Exit code N" prefix */
    CHECK(mcode_tool_is_benign_probe_failure(
        "Bash", "{\"command\": \"ls -a AGENTS.md\"}",
        "Exit code 1\nls: AGENTS.md: No such file or directory\n", -1));
    CHECK(!mcode_tool_is_benign_probe_failure(
        "Bash", "{\"command\": \"ctest\"}",
        "Exit code 1\n1 test failed\n", -1));
    CHECK(!mcode_tool_is_benign_probe_failure(
        "Read", "{\"file_path\": \"missing.txt\"}",
        "No such file or directory", -1));
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
    snprintf(t->reasoning_effort, sizeof t->reasoning_effort, "high");
    t->speed_mode = MCODE_SPEED_FAST;
    mcode_turn *tr = mcode_thread_add_turn(t, "Build login flow");
    mcode_block *b = mcode_turn_add_block(tr, MCODE_BLOCK_TEXT);
    mcode_str_set(&b->text, "Done implementing login.");
    mcode_block *tb_probe = mcode_turn_add_block(tr, MCODE_BLOCK_TOOL);
    snprintf(tb_probe->tool_name, sizeof tb_probe->tool_name, "bash");
    mcode_str_set(&tb_probe->input, "/bin/zsh -lc 'ls -a /tmp/missing-file'");
    mcode_str_set(&tb_probe->output, "ls: /tmp/missing-file: No such file or directory\n");
    tb_probe->status = CUI_TASK_FAILED;
    mcode_block *tb_err = mcode_turn_add_block(tr, MCODE_BLOCK_TOOL);
    snprintf(tb_err->tool_name, sizeof tb_err->tool_name, "bash");
    mcode_str_set(&tb_err->input, "ctest");
    mcode_str_set(&tb_err->output, "1 test failed\n");
    tb_err->status = CUI_TASK_FAILED;
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
    CHECK(strcmp(loaded.threads[0].reasoning_effort, "high") == 0);
    CHECK(loaded.threads[0].speed_mode == MCODE_SPEED_FAST);
    CHECK(loaded.threads[0].use_worktree);
    CHECK(loaded.threads[0].turn_count == 1);
    CHECK(loaded.threads[0].turns[0].block_count == 3);
    CHECK(strcmp(mcode_str_cstr(&loaded.threads[0].turns[0].blocks[0].text), "Done implementing login.") == 0);
    CHECK(loaded.threads[0].turns[0].blocks[1].status == CUI_TASK_DONE);
    CHECK(loaded.threads[0].turns[0].blocks[2].status == CUI_TASK_FAILED);
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
        "{\"slug\":\"gpt-6.2-sol\",\"display_name\":\"GPT-6.2-Sol\",\"visibility\":\"list\",\"description\":\"Fast go-to model.\",\"default_reasoning_level\":\"low\",\"supported_reasoning_levels\":[{\"effort\":\"low\"},{\"effort\":\"medium\"},{\"effort\":\"high\"},{\"effort\":\"xhigh\"}],\"additional_speed_tiers\":[\"fast\"],\"service_tiers\":[{\"id\":\"priority\",\"name\":\"Fast\",\"description\":\"2x speed, increased usage\"}]},"
        "{\"slug\":\"gpt-hidden\",\"display_name\":\"Hidden\",\"visibility\":\"hide\",\"description\":\"Internal.\"},"
        "{\"slug\":\"gpt-6.2-astra\",\"display_name\":\"GPT-6.2-Astra\",\"visibility\":\"list\",\"description\":\"Complex coding.\"}"
        "]}";
    mcode_model_option opts[8];
    int n = mcode_agent_parse_codex_models_json(codex_json, strlen(codex_json), opts, 8);
    CHECK(n == 2);
    CHECK(strcmp(opts[0].id, "gpt-6.2-sol") == 0);
    CHECK(strcmp(opts[0].label, "GPT-6.2-Sol") == 0);
    CHECK(strcmp(opts[0].detail, "Fast go-to model.") == 0);
    CHECK(strcmp(opts[0].default_effort, "low") == 0);
    CHECK(opts[0].effort_count == 4);
    CHECK(strcmp(opts[0].efforts[2], "high") == 0);
    CHECK(opts[0].supports_fast);
    CHECK(strcmp(opts[0].fast_desc, "2x speed, increased usage") == 0);
    CHECK(strcmp(opts[1].id, "gpt-6.2-astra") == 0);
    CHECK(!opts[1].supports_fast);

    char cfg_model[APP_MODEL_NAME_MAX] = "";
    char cfg_effort[24] = "";
    bool cfg_fast = false;
    const char *codex_toml =
        "# comment\n"
        "model = \"gpt-6.2-sol\"\n"
        "model_reasoning_effort = \"high\"\n"
        "service_tier = \"fast\"\n"
        "[features]\n"
        "model = \"ignored-section-model\"\n";
    mcode_agent_parse_codex_config_toml(codex_toml, cfg_model, sizeof cfg_model,
                                        cfg_effort, sizeof cfg_effort, &cfg_fast);
    CHECK(strcmp(cfg_model, "gpt-6.2-sol") == 0);
    CHECK(strcmp(cfg_effort, "high") == 0);
    CHECK(cfg_fast);

    const char *claude_json = "{\"model\":\"opus\",\"effortLevel\":\"xhigh\"}";
    mcode_agent_parse_claude_settings_json(claude_json, strlen(claude_json),
                                           cfg_model, sizeof cfg_model, cfg_effort, sizeof cfg_effort);
    CHECK(strcmp(cfg_model, "opus") == 0);
    CHECK(strcmp(cfg_effort, "xhigh") == 0);

    const char *agy_text =
        "\r\xe2\xa0\x8b Fetching available models...\rgemini-4-neon-high             Gemini 4 Neon (High)\n"
        "gemini-4-neon-xhigh\tGemini 4 Neon (XHigh)\n"
        "gemini-4-neon-medium\tGemini 4 Neon (Medium)\n"
        "gemini-4-neon-low\tGemini 4 Neon (Low)\n"
        "claude-opus-5-5-high           Claude Opus 5.5 (High)\n"
        "Not a model line\n";
    n = mcode_agent_parse_agy_models_text(agy_text, opts, 8);
    CHECK(n == 5);
    CHECK(strcmp(opts[0].id, "gemini-4-neon-high") == 0);
    CHECK(strcmp(opts[0].label, "Gemini 4 Neon (High)") == 0);
    CHECK(strcmp(opts[1].id, "gemini-4-neon-xhigh") == 0);
    CHECK(strcmp(opts[1].label, "Gemini 4 Neon (XHigh)") == 0);
    CHECK(strcmp(opts[3].id, "gemini-4-neon-low") == 0);
    CHECK(strcmp(opts[4].id, "claude-opus-5-5-high") == 0);
    CHECK(strcmp(opts[4].label, "Claude Opus 5.5 (High)") == 0);

    mcode_model_catalog cat;
    mcode_model_catalog_init(&cat);
    CHECK(cat.count[APP_AGENT_CLAUDE] >= 3);
    CHECK(cat.count[APP_AGENT_CODEX] >= 1);
    CHECK(cat.count[APP_AGENT_AGY] >= 1);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, "opus"), "Claude Opus") == 0);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, "custom-id"), "custom-id") == 0);
    CHECK(strcmp(mcode_model_display_label(&cat, APP_AGENT_CLAUDE, ""), "Default") == 0);

    /* Display formatting: Codex GPT-6.2-Sol + high -> "GPT-6.2 Sol" / "High" */
    mcode_model_catalog mock;
    memset(&mock, 0, sizeof mock);
    mock.count[APP_AGENT_CODEX] = mcode_agent_parse_codex_models_json(
        codex_json, strlen(codex_json), mock.items[APP_AGENT_CODEX], MCODE_MAX_MODELS_PER_AGENT);
    mock.count[APP_AGENT_AGY] = mcode_agent_parse_agy_models_text(
        agy_text, mock.items[APP_AGENT_AGY], MCODE_MAX_MODELS_PER_AGENT);
    snprintf(mock.cli_default_model[APP_AGENT_CODEX], sizeof mock.cli_default_model[APP_AGENT_CODEX], "gpt-6.2-sol");
    snprintf(mock.cli_default_effort[APP_AGENT_CODEX], sizeof mock.cli_default_effort[APP_AGENT_CODEX], "high");

    app_settings s;
    memset(&s, 0, sizeof s);
    mcode_thread t;
    memset(&t, 0, sizeof t);
    t.agent = APP_AGENT_CODEX;
    char dname[128] = "", deff[32] = "";
    mcode_model_format_display(&mock, &t, &s, dname, sizeof dname, deff, sizeof deff);
    CHECK(strcmp(dname, "GPT-6.2 Sol") == 0);
    CHECK(strcmp(deff, "High") == 0);

    snprintf(t.reasoning_effort, sizeof t.reasoning_effort, "xhigh");
    mcode_model_format_display(&mock, &t, &s, dname, sizeof dname, deff, sizeof deff);
    CHECK(strcmp(dname, "GPT-6.2 Sol") == 0);
    CHECK(strcmp(deff, "XHigh") == 0);

    /* Display formatting: Agy "Gemini 4 Neon (High)" -> "Gemini 4 Neon" / "High" */
    memset(&t, 0, sizeof t);
    t.agent = APP_AGENT_AGY;
    snprintf(t.model, sizeof t.model, "gemini-4-neon-high");
    mcode_model_format_display(&mock, &t, &s, dname, sizeof dname, deff, sizeof deff);
    CHECK(strcmp(dname, "Gemini 4 Neon") == 0);
    CHECK(strcmp(deff, "High") == 0);

    memset(&t, 0, sizeof t);
    t.agent = APP_AGENT_CODEX;
    snprintf(s.mcode.default_codex_model, sizeof s.mcode.default_codex_model, "gpt-6.2-sol");
    CHECK(strcmp(mcode_agent_effective_model(&t, &s), "gpt-6.2-sol") == 0);
    snprintf(t.model, sizeof t.model, "gpt-6.2-astra");
    CHECK(strcmp(mcode_agent_effective_model(&t, &s), "gpt-6.2-astra") == 0);

    mcode_model_catalog_free(&cat);
}

static void test_title_generation(void) {
    char title[MCODE_NAME_MAX] = "";

    /* Sanitization edge cases */
    CHECK(mcode_agent_sanitize_title("Fix auth token refresh", title, sizeof title));
    CHECK(strcmp(title, "Fix auth token refresh") == 0);

    CHECK(mcode_agent_sanitize_title("\n  Title: \"**Add sidebar auto-titling.**\"\nMore details here",
                                     title, sizeof title));
    CHECK(strcmp(title, "Add sidebar auto-titling") == 0);

    CHECK(mcode_agent_sanitize_title("###   Refactor   Git\tWorktrees:  \n", title, sizeof title));
    CHECK(strcmp(title, "Refactor Git Worktrees") == 0);

    CHECK(mcode_agent_sanitize_title("\r\x1b[2K\x1b[36mThinking...\rFix memory leak in parser",
                                     title, sizeof title));
    CHECK(strcmp(title, "Fix memory leak in parser") == 0);

    CHECK(!mcode_agent_sanitize_title("   \n\t  ", title, sizeof title));
    CHECK(!mcode_agent_sanitize_title("?", title, sizeof title));

    /* Claude JSON response */
    const char *claude_ok =
        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"\\\"Fix sidebar thread titling.\\\"\"}";
    CHECK(mcode_agent_parse_title_response(APP_AGENT_CLAUDE, claude_ok, strlen(claude_ok),
                                           title, sizeof title));
    CHECK(strcmp(title, "Fix sidebar thread titling") == 0);

    const char *claude_err =
        "{\"type\":\"result\",\"subtype\":\"error\",\"is_error\":true,\"result\":\"Invalid API key\"}";
    CHECK(!mcode_agent_parse_title_response(APP_AGENT_CLAUDE, claude_err, strlen(claude_err),
                                            title, sizeof title));

    /* Codex JSONL response (reasoning ignored, agent_message extracted) */
    const char *codex_ok =
        "{\"type\":\"thread.started\",\"thread_id\":\"thr_1\"}\n"
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"r1\",\"type\":\"reasoning\",\"text\":\"Should be short\"}}\n"
        "{\"type\":\"item.completed\",\"item\":{\"id\":\"m1\",\"type\":\"agent_message\",\"text\":\"Add dark mode toggle\"}}\n"
        "{\"type\":\"turn.completed\"}\n";
    CHECK(mcode_agent_parse_title_response(APP_AGENT_CODEX, codex_ok, strlen(codex_ok),
                                           title, sizeof title));
    CHECK(strcmp(title, "Add dark mode toggle") == 0);

    const char *codex_err =
        "{\"type\":\"turn.failed\",\"error\":{\"message\":\"Quota exceeded\"}}\n";
    CHECK(!mcode_agent_parse_title_response(APP_AGENT_CODEX, codex_err, strlen(codex_err),
                                            title, sizeof title));

    /* Antigravity CLI stream-json response (thinking ignored, deltas joined, result not duplicated) */
    const char *agy_ok =
        "{\"type\":\"thinking\",\"delta\":\"Summarizing request...\"}\n"
        "{\"type\":\"text\",\"delta\":\"Optimize \"}\n"
        "{\"type\":\"text\",\"delta\":\"diff renderer\"}\n"
        "{\"type\":\"result\",\"result\":\"Optimize diff renderer\"}\n";
    CHECK(mcode_agent_parse_title_response(APP_AGENT_AGY, agy_ok, strlen(agy_ok),
                                           title, sizeof title));
    CHECK(strcmp(title, "Optimize diff renderer") == 0);

    /* Plain text fallback */
    const char *plain = "Update CMake build flags\n";
    CHECK(mcode_agent_parse_title_response(APP_AGENT_AGY, plain, strlen(plain),
                                           title, sizeof title));
    CHECK(strcmp(title, "Update CMake build flags") == 0);

#ifndef _WIN32
    /* End-to-end async title request against a local mock CLI script */
    char mock_cli[512];
    snprintf(mock_cli, sizeof mock_cli, "/tmp/mcode_mock_title_cli_%d.sh", (int)getpid());
    FILE *f = fopen(mock_cli, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("#!/bin/sh\n"
              "printf '{\"type\":\"result\",\"is_error\":false,\"result\":\"Implement Auto Thread Titles\"}\\n'\n",
              f);
        fclose(f);
        chmod(mock_cli, 0755);

        app_settings s;
        memset(&s, 0, sizeof s);
        snprintf(s.mcode.claude_bin, sizeof s.mcode.claude_bin, "%s", mock_cli);

        mcode_thread t;
        memset(&t, 0, sizeof t);
        t.agent = APP_AGENT_CLAUDE;

        mcode_title_run *run = mcode_agent_start_title(&t, &s, "When we start a thread ask provider for title");
        CHECK(run != NULL);
        bool done = false;
        bool ok = false;
        char generated[MCODE_NAME_MAX] = "";
        for (int i = 0; i < 100 && !done; ++i) {
            done = mcode_agent_poll_title(run, generated, sizeof generated, &ok);
            if (!done) usleep(10000);
        }
        CHECK(done);
        CHECK(ok);
        CHECK(strcmp(generated, "Implement Auto Thread Titles") == 0);
        mcode_agent_title_destroy(run);
        remove(mock_cli);
    }
#endif
}

int main(void) {
    test_helpers();
    test_claude_parser();
    test_codex_parser();
    test_benign_probe_detection();
    test_agy_parser();
    test_model_catalog();
    test_title_generation();
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
