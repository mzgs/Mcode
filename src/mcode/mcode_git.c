#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_git.h"
#include "mcode_proc.h"
#include "app_log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static void trim_trailing(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool mkdir_p(const char *path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    if (!path || !path[0]) return false;
    char buf[MCODE_PATH_MAX];
    int n = snprintf(buf, sizeof buf, "%s", path);
    if (n <= 0 || (size_t)n >= sizeof buf) return false;
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0755);
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) == 0 || errno == EEXIST) return true;
    return false;
#endif
}

bool mcode_git_inspect_repo(const char *dir, bool *is_repo,
                            char *branch_out, size_t branch_cap,
                            char *remote_out, size_t remote_cap) {
    if (is_repo) *is_repo = false;
    if (branch_out && branch_cap > 0) branch_out[0] = '\0';
    if (remote_out && remote_cap > 0) remote_out[0] = '\0';
    if (!dir || !dir[0]) return false;

    const char *argv_check[] = { "git", "rev-parse", "--is-inside-work-tree", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv_check, dir, true, &rc, &out, NULL) || rc != 0 || !out) {
        free(out);
        return false;
    }
    trim_trailing(out);
    bool ok = strcmp(out, "true") == 0;
    free(out);
    if (!ok) return false;
    if (is_repo) *is_repo = true;

    if (branch_out && branch_cap > 0) {
        const char *argv_br[] = { "git", "rev-parse", "--abbrev-ref", "HEAD", NULL };
        if (mcode_proc_run_capture(argv_br, dir, false, &rc, &out, NULL) && rc == 0 && out) {
            trim_trailing(out);
            snprintf(branch_out, branch_cap, "%s", out);
        }
        free(out);
        out = NULL;
    }

    if (remote_out && remote_cap > 0) {
        const char *argv_rem[] = { "git", "remote", "get-url", "origin", NULL };
        if (mcode_proc_run_capture(argv_rem, dir, false, &rc, &out, NULL) && rc == 0 && out) {
            trim_trailing(out);
            snprintf(remote_out, remote_cap, "%s", out);
        }
        free(out);
    }
    return true;
}

bool mcode_git_init_repo(const char *dir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!dir || !dir[0]) return false;
    const char *argv[] = { "git", "init", NULL };
    int rc = -1;
    char *out = NULL;
    bool ran = mcode_proc_run_capture(argv, dir, true, &rc, &out, NULL);
    if (!ran || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git init failed (exit %d).", rc);
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

bool mcode_git_create_worktree(const char *repo_path, const char *project_name,
                               const char *title_or_slug,
                               char *worktree_out, size_t worktree_cap,
                               char *branch_out, size_t branch_cap,
                               char *err, size_t err_cap) {
    if (worktree_out && worktree_cap) worktree_out[0] = '\0';
    if (branch_out && branch_cap) branch_out[0] = '\0';
    if (err && err_cap) err[0] = '\0';

    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";

    char base_dir[MCODE_PATH_MAX];
    snprintf(base_dir, sizeof base_dir, "%s/.mcode/worktrees", home);
    if (!mkdir_p(base_dir)) {
        if (err && err_cap) snprintf(err, err_cap, "Could not create ~/.mcode/worktrees directory.");
        return false;
    }

    char proj_slug[64];
    char task_slug[64];
    char suffix[24];
    mcode_slugify(project_name && project_name[0] ? project_name : "repo", proj_slug, sizeof proj_slug);
    mcode_slugify(title_or_slug && title_or_slug[0] ? title_or_slug : "thread", task_slug, sizeof task_slug);
    mcode_gen_id(suffix, sizeof suffix, "");
    if (strlen(suffix) > 6) suffix[6] = '\0';

    char wt_path[MCODE_PATH_MAX];
    char br_name[MCODE_BRANCH_MAX];
    snprintf(wt_path, sizeof wt_path, "%s/%s-%s-%s", base_dir, proj_slug, task_slug, suffix);
    snprintf(br_name, sizeof br_name, "mcode/%s-%s", task_slug, suffix);

    const char *argv[] = { "git", "worktree", "add", "-b", br_name, wt_path, NULL };
    int rc = -1;
    char *out = NULL;
    bool ran = mcode_proc_run_capture(argv, repo_path, true, &rc, &out, NULL);
    if (!ran || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) {
                trim_trailing(out);
                snprintf(err, err_cap, "%s", out);
            } else {
                snprintf(err, err_cap, "git worktree add failed (exit %d).", rc);
            }
        }
        free(out);
        return false;
    }
    free(out);

    if (worktree_out && worktree_cap) snprintf(worktree_out, worktree_cap, "%s", wt_path);
    if (branch_out && branch_cap) snprintf(branch_out, branch_cap, "%s", br_name);
    app_log_info("Created git worktree %s (branch %s)", wt_path, br_name);
    return true;
}

bool mcode_git_remove_worktree(const char *repo_path, const char *worktree_path,
                               char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!repo_path || !worktree_path || !worktree_path[0]) return false;

    const char *argv[] = { "git", "worktree", "remove", "--force", worktree_path, NULL };
    int rc = -1;
    char *out = NULL;
    bool ran = mcode_proc_run_capture(argv, repo_path, true, &rc, &out, NULL);
    if (!ran || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) {
                trim_trailing(out);
                snprintf(err, err_cap, "%s", out);
            } else {
                snprintf(err, err_cap, "git worktree remove failed (exit %d).", rc);
            }
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

static int find_diff_file(mcode_diff_file *files, int count, const char *path) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(files[i].path, path) == 0) return i;
    }
    return -1;
}

static int count_file_lines(const char *workdir, const char *rel_path) {
    char full[MCODE_PATH_MAX + 512];
    snprintf(full, sizeof full, "%s/%s", workdir, rel_path);
    FILE *f = fopen(full, "rb");
    if (!f) return 0;
    int lines = 0;
    int last = 0;
    char buf[4096];
    size_t n = 0;
    size_t total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') ++lines;
            last = (unsigned char)buf[i];
        }
        total += n;
        if (total > 512 * 1024) break;
    }
    fclose(f);
    if (total > 0 && last != '\n') ++lines;
    return lines;
}

bool mcode_git_list_changes(const char *workdir,
                            mcode_diff_file **out_files, int *out_count,
                            int *out_total_added, int *out_total_removed) {
    if (out_files) *out_files = NULL;
    if (out_count) *out_count = 0;
    if (out_total_added) *out_total_added = 0;
    if (out_total_removed) *out_total_removed = 0;
    if (!workdir || !workdir[0]) return false;

    const char *argv_status[] = { "git", "status", "--porcelain=v1", "-uall", NULL };
    int rc = -1;
    char *status_out = NULL;
    if (!mcode_proc_run_capture(argv_status, workdir, false, &rc, &status_out, NULL) || rc != 0 || !status_out) {
        free(status_out);
        return false;
    }

    int cap = 32;
    int count = 0;
    mcode_diff_file *files = calloc((size_t)cap, sizeof *files);
    if (!files) {
        free(status_out);
        return false;
    }

    char *p = status_out;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len >= 4) {
            char x = p[0];
            char y = p[1];
            const char *path_start = p + 3;
            size_t path_len = line_len - 3;

            /* Handle rename "old -> new" */
            const char *arrow = NULL;
            for (size_t i = 0; i + 4 <= path_len; ++i) {
                if (memcmp(path_start + i, " -> ", 4) == 0) {
                    arrow = path_start + i + 4;
                    break;
                }
            }
            if (arrow) {
                path_len = (size_t)((path_start + path_len) - arrow);
                path_start = arrow;
            }
            /* Strip optional quotes */
            if (path_len >= 2 && path_start[0] == '"' && path_start[path_len - 1] == '"') {
                path_start++;
                path_len -= 2;
            }
            if (path_len > 0 && path_len < sizeof(files[0].path)) {
                if (count >= cap) {
                    cap *= 2;
                    mcode_diff_file *grown = realloc(files, (size_t)cap * sizeof *files);
                    if (!grown) break;
                    files = grown;
                }
                mcode_diff_file *df = &files[count];
                memset(df, 0, sizeof *df);
                memcpy(df->path, path_start, path_len);
                df->path[path_len] = '\0';
                if (x == '?' && y == '?') {
                    strcpy(df->status, "?");
                    df->untracked = true;
                } else if (x == 'D' || y == 'D') {
                    strcpy(df->status, "D");
                } else if (x == 'A' || y == 'A') {
                    strcpy(df->status, "A");
                } else if (x == 'R' || y == 'R') {
                    strcpy(df->status, "R");
                } else {
                    strcpy(df->status, "M");
                }
                ++count;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(status_out);

    /* Parse numstat against HEAD (fallback to plain git diff if unborn HEAD) */
    const char *argv_numstat[] = { "git", "diff", "HEAD", "--numstat", NULL };
    char *numstat_out = NULL;
    if (!mcode_proc_run_capture(argv_numstat, workdir, false, &rc, &numstat_out, NULL) || rc != 0) {
        free(numstat_out);
        numstat_out = NULL;
        const char *argv_numstat_fb[] = { "git", "diff", "--numstat", NULL };
        (void)mcode_proc_run_capture(argv_numstat_fb, workdir, false, &rc, &numstat_out, NULL);
    }

    if (numstat_out) {
        char *q = numstat_out;
        while (*q) {
            char *eol = strchr(q, '\n');
            if (eol) *eol = '\0';
            char *t1 = strchr(q, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t1 && t2) {
                *t1 = '\0';
                *t2 = '\0';
                const char *fpath = t2 + 1;
                int add = (q[0] == '-') ? 0 : atoi(q);
                int rem = (t1[1] == '-') ? 0 : atoi(t1 + 1);
                int idx = find_diff_file(files, count, fpath);
                if (idx >= 0) {
                    files[idx].added = add;
                    files[idx].removed = rem;
                }
            }
            if (!eol) break;
            q = eol + 1;
        }
        free(numstat_out);
    }

    int total_add = 0, total_rem = 0;
    for (int i = 0; i < count; ++i) {
        if (files[i].untracked && files[i].added == 0) {
            files[i].added = count_file_lines(workdir, files[i].path);
        }
        total_add += files[i].added;
        total_rem += files[i].removed;
    }

    if (out_files) *out_files = files;
    else free(files);
    if (out_count) *out_count = count;
    if (out_total_added) *out_total_added = total_add;
    if (out_total_removed) *out_total_removed = total_rem;
    return true;
}

static char *read_file_capped(const char *path, size_t max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return strdup("");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    fseek(f, 0, SEEK_SET);
    size_t to_read = (size_t)sz;
    if (to_read > max_bytes) to_read = max_bytes;
    char *buf = malloc(to_read + 1);
    if (!buf) {
        fclose(f);
        return strdup("");
    }
    size_t n = fread(buf, 1, to_read, f);
    buf[n] = '\0';
    fclose(f);
    /* Guard binary files: replace embedded NULs */
    if (memchr(buf, '\0', n)) {
        free(buf);
        return strdup("[binary file]\n");
    }
    return buf;
}

bool mcode_git_load_file_diff(const char *workdir, const mcode_diff_file *file,
                              char **out_old, char **out_new) {
    if (out_old) *out_old = NULL;
    if (out_new) *out_new = NULL;
    if (!workdir || !file || !file->path[0]) return false;

    enum { MAX_DIFF_FILE_BYTES = 512 * 1024 };

    char *old_text = NULL;
    if (file->untracked || strcmp(file->status, "A") == 0) {
        old_text = strdup("");
    } else {
        char spec[600];
        snprintf(spec, sizeof spec, "HEAD:%s", file->path);
        const char *argv[] = { "git", "show", spec, NULL };
        int rc = -1;
        size_t len = 0;
        if (!mcode_proc_run_capture(argv, workdir, false, &rc, &old_text, &len) || rc != 0 || !old_text) {
            free(old_text);
            old_text = strdup("");
        } else if (len > MAX_DIFF_FILE_BYTES) {
            old_text[MAX_DIFF_FILE_BYTES] = '\0';
        } else if (memchr(old_text, '\0', len)) {
            free(old_text);
            old_text = strdup("[binary file]\n");
        }
    }

    char *new_text = NULL;
    if (strcmp(file->status, "D") == 0) {
        new_text = strdup("");
    } else {
        char full[MCODE_PATH_MAX + 512];
        snprintf(full, sizeof full, "%s/%s", workdir, file->path);
        new_text = read_file_capped(full, MAX_DIFF_FILE_BYTES);
    }

    if (out_old) *out_old = old_text;
    else free(old_text);
    if (out_new) *out_new = new_text;
    else free(new_text);
    return true;
}

const char *mcode_lang_from_path(const char *path) {
    if (!path) return "text";
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "CMakeLists.txt") == 0) return "cmake";
    if (strcmp(base, "Makefile") == 0) return "make";

    const char *ext = strrchr(base, '.');
    if (!ext || !ext[1]) return "text";
    ext++;

    static const struct { const char *ext; const char *lang; } table[] = {
        { "c", "c" }, { "h", "c" },
        { "cc", "cpp" }, { "cpp", "cpp" }, { "cxx", "cpp" }, { "hpp", "cpp" }, { "m", "c" }, { "mm", "cpp" },
        { "js", "javascript" }, { "mjs", "javascript" }, { "cjs", "javascript" }, { "jsx", "javascript" },
        { "ts", "typescript" }, { "mts", "typescript" }, { "tsx", "tsx" },
        { "py", "python" }, { "rs", "rust" }, { "go", "go" }, { "zig", "zig" },
        { "swift", "swift" }, { "kt", "kotlin" }, { "java", "java" }, { "cs", "csharp" },
        { "rb", "ruby" }, { "php", "php" }, { "lua", "lua" }, { "ex", "elixir" }, { "exs", "elixir" },
        { "json", "json" }, { "yaml", "yaml" }, { "yml", "yaml" }, { "toml", "toml" },
        { "md", "markdown" }, { "html", "html" }, { "htm", "html" }, { "css", "css" },
        { "sql", "sql" }, { "sh", "bash" }, { "zsh", "bash" }, { "bash", "bash" },
        { "cmake", "cmake" }, { "proto", "proto" }, { "svelte", "svelte" }, { "astro", "astro" },
        { "graphql", "graphql" }, { "gql", "graphql" }, { "diff", "diff" }, { "patch", "diff" },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (strcmp(ext, table[i].ext) == 0) return table[i].lang;
    }
    return "text";
}

bool mcode_git_commit_all(const char *workdir, const char *message,
                          char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0] || !message || !message[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Commit message cannot be empty.");
        return false;
    }

    const char *argv_add[] = { "git", "add", "-A", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv_add, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git add -A failed.");
        }
        free(out);
        return false;
    }
    free(out);

    const char *argv_commit[] = { "git", "commit", "-m", message, NULL };
    out = NULL;
    if (!mcode_proc_run_capture(argv_commit, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git commit failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

bool mcode_git_push(const char *workdir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0]) return false;

    const char *argv[] = { "git", "push", "-u", "origin", "HEAD", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git push failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

static void url_encode_append(mcode_str *s, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; p && *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            char c = (char)*p;
            mcode_str_append(s, &c, 1);
        } else {
            char enc[3] = { '%', hex[*p >> 4], hex[*p & 0xF] };
            mcode_str_append(s, enc, 3);
        }
    }
}

bool mcode_git_build_pr_url(const char *remote_url, const char *base_branch,
                            const char *head_branch, const char *title,
                            char *url_out, size_t url_cap) {
    if (url_out && url_cap) url_out[0] = '\0';
    if (!remote_url || !remote_url[0] || !head_branch || !head_branch[0]) return false;

    char https_base[512] = "";
    if (strncmp(remote_url, "git@", 4) == 0) {
        const char *colon = strchr(remote_url + 4, ':');
        if (!colon) return false;
        snprintf(https_base, sizeof https_base, "https://%.*s/%s",
                 (int)(colon - (remote_url + 4)), remote_url + 4, colon + 1);
    } else if (strncmp(remote_url, "https://", 8) == 0 || strncmp(remote_url, "http://", 7) == 0) {
        snprintf(https_base, sizeof https_base, "%s", remote_url);
    } else {
        return false;
    }

    size_t len = strlen(https_base);
    if (len >= 4 && strcmp(https_base + len - 4, ".git") == 0) https_base[len - 4] = '\0';
    while (strlen(https_base) > 0 && https_base[strlen(https_base) - 1] == '/') {
        https_base[strlen(https_base) - 1] = '\0';
    }

    mcode_str u;
    mcode_str_init(&u);
    mcode_str_append_cstr(&u, https_base);
    mcode_str_append_cstr(&u, "/compare/");
    if (base_branch && base_branch[0] && strcmp(base_branch, head_branch) != 0) {
        url_encode_append(&u, base_branch);
        mcode_str_append_cstr(&u, "...");
    }
    url_encode_append(&u, head_branch);
    mcode_str_append_cstr(&u, "?expand=1");
    if (title && title[0]) {
        mcode_str_append_cstr(&u, "&title=");
        url_encode_append(&u, title);
    }

    bool ok = u.len > 0 && u.len < url_cap;
    if (ok && url_out) snprintf(url_out, url_cap, "%s", mcode_str_cstr(&u));
    mcode_str_free(&u);
    return ok;
}

bool mcode_git_create_pr(const char *workdir, const char *title, const char *body,
                         char *url_out, size_t url_cap, bool *used_gh,
                         char *err, size_t err_cap) {
    if (url_out && url_cap) url_out[0] = '\0';
    if (used_gh) *used_gh = false;
    if (err && err_cap) err[0] = '\0';

    /* Push branch first so PR creation succeeds */
    if (!mcode_git_push(workdir, err, err_cap)) return false;

    char gh_bin[512];
    if (mcode_find_executable("gh", gh_bin, sizeof gh_bin)) {
        const char *argv[] = {
            gh_bin, "pr", "create",
            "--title", (title && title[0]) ? title : "Mcode changes",
            "--body", (body && body[0]) ? body : "Created with Mcode.",
            NULL
        };
        int rc = -1;
        char *out = NULL;
        if (mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) && rc == 0 && out) {
            trim_trailing(out);
            const char *http = strstr(out, "https://");
            if (!http) http = strstr(out, "http://");
            if (http && url_out && url_cap) {
                snprintf(url_out, url_cap, "%s", http);
            }
            if (used_gh) *used_gh = true;
            free(out);
            return true;
        }
        free(out);
    }

    /* Fallback: build compare URL */
    bool is_repo = false;
    char branch[MCODE_BRANCH_MAX] = "";
    char remote[512] = "";
    if (!mcode_git_inspect_repo(workdir, &is_repo, branch, sizeof branch, remote, sizeof remote) || !remote[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Could not determine Git origin URL for PR.");
        return false;
    }
    if (!mcode_git_build_pr_url(remote, "", branch, title, url_out, url_cap)) {
        if (err && err_cap) snprintf(err, err_cap, "Unsupported Git remote URL format.");
        return false;
    }
    return true;
}
