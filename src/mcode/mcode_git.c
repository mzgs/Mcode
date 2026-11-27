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
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#else
#include <direct.h>
#include <sys/stat.h>
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

static bool path_is_dir(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool path_exists(const char *path) {
    if (!path || !path[0]) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

void mcode_git_default_projects_dir(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
    if (!home || !home[0]) return;

    static const char *const k_candidates[] = {
        "Developer", "Projects", "projects", "Code", "code", "src", "workspace", "repos"
    };
    char probe[MCODE_PATH_MAX];
    for (size_t i = 0; i < sizeof k_candidates / sizeof k_candidates[0]; ++i) {
        snprintf(probe, sizeof probe, "%s/%s", home, k_candidates[i]);
        if (path_is_dir(probe)) {
            snprintf(out, cap, "%s", probe);
            return;
        }
    }
    snprintf(out, cap, "%s", home);
}

void mcode_git_shorten_home_path(const char *path, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    const char *home = getenv("HOME");
    if (home && home[0]) {
        size_t hlen = strlen(home);
        while (hlen > 1 && home[hlen - 1] == '/') --hlen;
        if (strncmp(path, home, hlen) == 0 && (path[hlen] == '/' || path[hlen] == '\0')) {
            snprintf(out, out_cap, "~%s", path + hlen);
            return;
        }
    }
    snprintf(out, out_cap, "%s", path);
}

void mcode_git_repo_name_from_url(const char *url, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!url || !url[0]) return;

    const char *start = url;
    while (*start && isspace((unsigned char)*start)) ++start;
    size_t len = strlen(start);
    while (len > 0 && (isspace((unsigned char)start[len - 1]) || start[len - 1] == '/')) --len;
    if (len == 0) return;

    const char *last_sep = NULL;
    for (size_t i = 0; i < len; ++i) {
        if (start[i] == '/' || start[i] == ':') last_sep = start + i + 1;
    }
    const char *name = last_sep ? last_sep : start;
    size_t nlen = (size_t)((start + len) - name);
    if (nlen >= 4 && memcmp(name + nlen - 4, ".git", 4) == 0) {
        nlen -= 4;
    }
    if (nlen == 0) return;
    if (nlen >= out_cap) nlen = out_cap - 1;
    memcpy(out, name, nlen);
    out[nlen] = '\0';
}

bool mcode_git_create_project_dir(const char *parent_dir, const char *folder_name,
                                  bool init_git, char *out_path, size_t out_cap,
                                  char *err, size_t err_cap) {
    if (out_path && out_cap) out_path[0] = '\0';
    if (err && err_cap) err[0] = '\0';

    if (!parent_dir || !parent_dir[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Choose a location folder.");
        return false;
    }
    if (!folder_name || !folder_name[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Enter a project name.");
        return false;
    }

    while (*folder_name && isspace((unsigned char)*folder_name)) ++folder_name;
    size_t flen = strlen(folder_name);
    while (flen > 0 && isspace((unsigned char)folder_name[flen - 1])) --flen;
    if (flen == 0 || flen >= MCODE_NAME_MAX) {
        if (err && err_cap) snprintf(err, err_cap, "Enter a valid project name.");
        return false;
    }

    char clean_name[MCODE_NAME_MAX];
    memcpy(clean_name, folder_name, flen);
    clean_name[flen] = '\0';

    if (strcmp(clean_name, ".") == 0 || strcmp(clean_name, "..") == 0 ||
        strchr(clean_name, '/') != NULL || strchr(clean_name, '\\') != NULL) {
        if (err && err_cap) snprintf(err, err_cap, "Project name cannot contain path separators.");
        return false;
    }

    /* Expand leading ~/ in parent_dir */
    char base[MCODE_PATH_MAX];
    if (parent_dir[0] == '~' && (parent_dir[1] == '/' || parent_dir[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            if (err && err_cap) snprintf(err, err_cap, "Could not resolve home directory.");
            return false;
        }
        snprintf(base, sizeof base, "%s%s", home, parent_dir + 1);
    } else {
        snprintf(base, sizeof base, "%s", parent_dir);
    }
    size_t blen = strlen(base);
    while (blen > 1 && (base[blen - 1] == '/' || base[blen - 1] == '\\')) {
        base[--blen] = '\0';
    }

    char full[MCODE_PATH_MAX];
    int n = snprintf(full, sizeof full, "%s/%s", base, clean_name);
    if (n <= 0 || (size_t)n >= sizeof full) {
        if (err && err_cap) snprintf(err, err_cap, "Project path is too long.");
        return false;
    }

    if (path_exists(full)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "Folder already exists: %s (use Open Folder instead).", clean_name);
        }
        return false;
    }

    if (!path_is_dir(base)) {
        if (!mkdir_p(base)) {
            if (err && err_cap) snprintf(err, err_cap, "Could not create parent folder.");
            return false;
        }
    }

#ifdef _WIN32
    if (_mkdir(full) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "Could not create folder: %s", strerror(errno));
        return false;
    }
#else
    if (mkdir(full, 0755) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "Could not create folder: %s", strerror(errno));
        return false;
    }
#endif

    if (init_git) {
        char git_err[256] = "";
        if (!mcode_git_init_repo(full, git_err, sizeof git_err)) {
            app_log_warn("Created %s but git init failed: %s", full, git_err);
        }
    }

    if (out_path && out_cap) snprintf(out_path, out_cap, "%s", full);
    app_log_info("Created project folder %s (git=%s)", full, init_git ? "yes" : "no");
    return true;
}

static int compare_discovered_repos(const void *a, const void *b) {
    const mcode_discovered_repo *ra = (const mcode_discovered_repo *)a;
    const mcode_discovered_repo *rb = (const mcode_discovered_repo *)b;
    if (rb->mtime != ra->mtime) {
        return (rb->mtime > ra->mtime) ? 1 : -1;
    }
    const char *sa = ra->name;
    const char *sb = rb->name;
    while (*sa && *sb) {
        int ca = tolower((unsigned char)*sa);
        int cb = tolower((unsigned char)*sb);
        if (ca != cb) return ca - cb;
        ++sa;
        ++sb;
    }
    return tolower((unsigned char)*sa) - tolower((unsigned char)*sb);
}

int mcode_git_discover_repos(const char *extra_dir, mcode_discovered_repo *out, int max_count) {
    if (!out || max_count <= 0) return 0;
#ifdef _WIN32
    (void)extra_dir;
    return 0;
#else
    char roots[10][MCODE_PATH_MAX];
    int root_count = 0;

    if (extra_dir && extra_dir[0] && path_is_dir(extra_dir)) {
        snprintf(roots[root_count++], sizeof roots[0], "%s", extra_dir);
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        static const char *const k_dirs[] = {
            "Developer", "Projects", "projects", "Code", "code", "src", "workspace", "repos"
        };
        for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0] && root_count < 10; ++i) {
            char cand[MCODE_PATH_MAX];
            snprintf(cand, sizeof cand, "%s/%s", home, k_dirs[i]);
            if (!path_is_dir(cand)) continue;
            bool dup = false;
            for (int r = 0; r < root_count; ++r) {
                if (strcmp(roots[r], cand) == 0) { dup = true; break; }
            }
            if (!dup) snprintf(roots[root_count++], sizeof roots[0], "%s", cand);
        }
    }

    int cap = max_count * 4;
    if (cap < 128) cap = 128;
    mcode_discovered_repo *all = calloc((size_t)cap, sizeof *all);
    if (!all) return 0;

    int count = 0;
    for (int r = 0; r < root_count; ++r) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char full[MCODE_PATH_MAX];
            int n = snprintf(full, sizeof full, "%s/%s", roots[r], ent->d_name);
            if (n <= 0 || (size_t)n >= sizeof full) continue;

            char git_marker[MCODE_PATH_MAX + 16];
            snprintf(git_marker, sizeof git_marker, "%s/.git", full);
            struct stat gst;
            if (stat(git_marker, &gst) != 0) continue;

            bool dup = false;
            for (int k = 0; k < count; ++k) {
                if (strcmp(all[k].path, full) == 0) { dup = true; break; }
            }
            if (dup) continue;

            if (count >= cap) {
                if (cap >= 1024) break;
                int ncap = cap * 2;
                mcode_discovered_repo *grown = realloc(all, (size_t)ncap * sizeof *all);
                if (!grown) break;
                all = grown;
                cap = ncap;
            }

            struct stat dst;
            int64_t mtime = (int64_t)gst.st_mtime;
            if (stat(full, &dst) == 0 && (int64_t)dst.st_mtime > mtime) {
                mtime = (int64_t)dst.st_mtime;
            }

            snprintf(all[count].name, sizeof all[count].name, "%s", ent->d_name);
            snprintf(all[count].path, sizeof all[count].path, "%s", full);
            all[count].mtime = mtime;
            ++count;
        }
        closedir(d);
    }

    if (count > 1) {
        qsort(all, (size_t)count, sizeof all[0], compare_discovered_repos);
    }
    int ret = count < max_count ? count : max_count;
    if (ret > 0) {
        memcpy(out, all, (size_t)ret * sizeof out[0]);
    }
    free(all);
    return ret;
#endif
}

static int find_diff_file_staged(mcode_diff_file *files, int count, const char *path, bool staged) {
    for (int i = 0; i < count; ++i) {
        if (files[i].staged == staged && strcmp(files[i].path, path) == 0) return i;
    }
    return -1;
}

static void status_str_from_xy_char(char c, char out[4]) {
    if (c == 'D') strcpy(out, "D");
    else if (c == 'A') strcpy(out, "A");
    else if (c == 'R' || c == 'C') strcpy(out, "R");
    else strcpy(out, "M");
}

static bool append_diff_entry(mcode_diff_file **files_p, int *count_p, int *cap_p,
                              const char *path, size_t path_len,
                              const char *status, bool untracked,
                              bool staged, bool unstaged) {
    if (*count_p >= *cap_p) {
        int ncap = (*cap_p) * 2;
        mcode_diff_file *grown = realloc(*files_p, (size_t)ncap * sizeof *grown);
        if (!grown) return false;
        *files_p = grown;
        *cap_p = ncap;
    }
    mcode_diff_file *df = &(*files_p)[*count_p];
    memset(df, 0, sizeof *df);
    memcpy(df->path, path, path_len);
    df->path[path_len] = '\0';
    snprintf(df->status, sizeof df->status, "%s", status);
    df->untracked = untracked;
    df->staged = staged;
    df->unstaged = unstaged;
    (*count_p)++;
    return true;
}

static void apply_numstat_output(char *numstat_out, mcode_diff_file *files, int count, bool staged) {
    if (!numstat_out) return;
    char *q = numstat_out;
    while (*q) {
        char *eol = strchr(q, '\n');
        if (eol) *eol = '\0';
        size_t len = strlen(q);
        if (len > 0 && q[len - 1] == '\r') q[--len] = '\0';
        char *t1 = strchr(q, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = '\0';
            *t2 = '\0';
            const char *fpath = t2 + 1;
            int add = (q[0] == '-') ? 0 : atoi(q);
            int rem = (t1[1] == '-') ? 0 : atoi(t1 + 1);
            int idx = find_diff_file_staged(files, count, fpath, staged);
            if (idx >= 0) {
                files[idx].added = add;
                files[idx].removed = rem;
            }
        }
        if (!eol) break;
        q = eol + 1;
    }
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
        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;
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
                if (x == '?' && y == '?') {
                    append_diff_entry(&files, &count, &cap, path_start, path_len,
                                      "?", true, false, true);
                } else if (x != '!' && y != '!') {
                    if (x != ' ' && x != '?') {
                        char st[4];
                        status_str_from_xy_char(x, st);
                        append_diff_entry(&files, &count, &cap, path_start, path_len,
                                          st, false, true, false);
                    }
                    if (y != ' ' && y != '?') {
                        char st[4];
                        status_str_from_xy_char(y, st);
                        append_diff_entry(&files, &count, &cap, path_start, path_len,
                                          st, false, false, true);
                    }
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(status_out);

    /* Unstaged numstat (working tree vs index) */
    const char *argv_unstaged[] = { "git", "diff", "--numstat", NULL };
    char *unstaged_out = NULL;
    if (mcode_proc_run_capture(argv_unstaged, workdir, false, &rc, &unstaged_out, NULL) && rc == 0) {
        apply_numstat_output(unstaged_out, files, count, false);
    }
    free(unstaged_out);

    /* Staged numstat (index vs HEAD; works on unborn HEAD too) */
    const char *argv_staged[] = { "git", "diff", "--cached", "--numstat", NULL };
    char *staged_out = NULL;
    if (mcode_proc_run_capture(argv_staged, workdir, false, &rc, &staged_out, NULL) && rc == 0) {
        apply_numstat_output(staged_out, files, count, true);
    }
    free(staged_out);

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

static char *git_show_capped(const char *workdir, const char *spec, size_t max_bytes, bool *ok_out) {
    if (ok_out) *ok_out = false;
    const char *argv[] = { "git", "show", spec, NULL };
    int rc = -1;
    char *text = NULL;
    size_t len = 0;
    if (!mcode_proc_run_capture(argv, workdir, false, &rc, &text, &len) || rc != 0 || !text) {
        free(text);
        return NULL;
    }
    if (len > max_bytes) {
        text[max_bytes] = '\0';
        len = max_bytes;
    }
    if (memchr(text, '\0', len)) {
        free(text);
        if (ok_out) *ok_out = true;
        return strdup("[binary file]\n");
    }
    if (ok_out) *ok_out = true;
    return text;
}

bool mcode_git_load_file_diff(const char *workdir, const mcode_diff_file *file,
                              char **out_old, char **out_new) {
    if (out_old) *out_old = NULL;
    if (out_new) *out_new = NULL;
    if (!workdir || !file || !file->path[0]) return false;

    enum { MAX_DIFF_FILE_BYTES = 512 * 1024 };

    char *old_text = NULL;
    char *new_text = NULL;
    char spec[600];

    if (file->staged) {
        /* Staged diff: HEAD vs Index */
        if (strcmp(file->status, "A") == 0) {
            old_text = strdup("");
        } else {
            snprintf(spec, sizeof spec, "HEAD:%s", file->path);
            old_text = git_show_capped(workdir, spec, MAX_DIFF_FILE_BYTES, NULL);
            if (!old_text) old_text = strdup("");
        }
        if (strcmp(file->status, "D") == 0) {
            new_text = strdup("");
        } else {
            snprintf(spec, sizeof spec, ":%s", file->path);
            new_text = git_show_capped(workdir, spec, MAX_DIFF_FILE_BYTES, NULL);
            if (!new_text) new_text = strdup("");
        }
    } else {
        /* Unstaged / untracked diff: Index (fallback HEAD) vs Working Tree */
        if (file->untracked) {
            old_text = strdup("");
        } else {
            snprintf(spec, sizeof spec, ":%s", file->path);
            old_text = git_show_capped(workdir, spec, MAX_DIFF_FILE_BYTES, NULL);
            if (!old_text) {
                snprintf(spec, sizeof spec, "HEAD:%s", file->path);
                old_text = git_show_capped(workdir, spec, MAX_DIFF_FILE_BYTES, NULL);
            }
            if (!old_text) old_text = strdup("");
        }
        if (strcmp(file->status, "D") == 0) {
            new_text = strdup("");
        } else {
            char full[MCODE_PATH_MAX + 512];
            snprintf(full, sizeof full, "%s/%s", workdir, file->path);
            new_text = read_file_capped(full, MAX_DIFF_FILE_BYTES);
        }
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

bool mcode_git_commit_staged(const char *workdir, const char *message,
                             char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0] || !message || !message[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Commit message cannot be empty.");
        return false;
    }

    const char *argv_commit[] = { "git", "commit", "-m", message, NULL };
    int rc = -1;
    char *out = NULL;
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

bool mcode_git_commit_all(const char *workdir, const char *message,
                          char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0] || !message || !message[0]) {
        if (err && err_cap) snprintf(err, err_cap, "Commit message cannot be empty.");
        return false;
    }

    if (!mcode_git_stage_all(workdir, err, err_cap)) return false;
    return mcode_git_commit_staged(workdir, message, err, err_cap);
}

bool mcode_git_stage_file(const char *workdir, const char *rel_path,
                          char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0] || !rel_path || !rel_path[0]) return false;
    const char *argv[] = { "git", "add", "--", rel_path, NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git add failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

bool mcode_git_unstage_file(const char *workdir, const char *rel_path,
                            char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0] || !rel_path || !rel_path[0]) return false;
    const char *argv[] = { "git", "restore", "--staged", "--", rel_path, NULL };
    int rc = -1;
    char *out = NULL;
    if (mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) && rc == 0) {
        free(out);
        return true;
    }
    free(out);
    out = NULL;
    /* Fallback for unborn HEAD or older git */
    const char *argv_fb[] = { "git", "rm", "--cached", "--", rel_path, NULL };
    if (mcode_proc_run_capture(argv_fb, workdir, true, &rc, &out, NULL) && rc == 0) {
        free(out);
        return true;
    }
    if (err && err_cap) {
        if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
        else snprintf(err, err_cap, "Could not unstage file.");
    }
    free(out);
    return false;
}

bool mcode_git_stage_all(const char *workdir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0]) return false;
    const char *argv[] = { "git", "add", "-A", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git add -A failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

bool mcode_git_unstage_all(const char *workdir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0]) return false;
    const char *argv[] = { "git", "reset", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL) || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) { trim_trailing(out); snprintf(err, err_cap, "%s", out); }
            else snprintf(err, err_cap, "git reset failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

static void extract_git_error_line(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;

    const char *best_fatal = NULL;
    size_t best_fatal_len = 0;
    const char *best_error = NULL;
    size_t best_error_len = 0;
    const char *best_conflict = NULL;
    size_t best_conflict_len = 0;
    const char *best_reject = NULL;
    size_t best_reject_len = 0;
    const char *last_non_hint = NULL;
    size_t last_non_hint_len = 0;
    const char *first_non_empty = NULL;
    size_t first_non_empty_len = 0;

    const char *p = raw;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            --len;
        }
        const char *s = p;
        while (len > 0 && (*s == ' ' || *s == '\t')) {
            ++s;
            --len;
        }
        if (len > 0) {
            if (!first_non_empty) {
                first_non_empty = s;
                first_non_empty_len = len;
            }
            if (!best_fatal && len >= 6 && strncmp(s, "fatal:", 6) == 0) {
                best_fatal = s;
                best_fatal_len = len;
            } else if (!best_conflict && len >= 8 && strncmp(s, "CONFLICT", 8) == 0) {
                best_conflict = s;
                best_conflict_len = len;
            } else if (!best_error && len >= 6 && strncmp(s, "error:", 6) == 0) {
                best_error = s;
                best_error_len = len;
            } else if (!best_reject && len >= 3 && strncmp(s, "! [", 3) == 0) {
                best_reject = s;
                best_reject_len = len;
            }
            if (!(len >= 5 && strncmp(s, "hint:", 5) == 0)) {
                last_non_hint = s;
                last_non_hint_len = len;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }

    const char *chosen = best_fatal ? best_fatal
                       : best_conflict ? best_conflict
                       : best_error ? best_error
                       : best_reject ? best_reject
                       : last_non_hint ? last_non_hint
                       : first_non_empty;
    size_t clen = best_fatal ? best_fatal_len
                : best_conflict ? best_conflict_len
                : best_error ? best_error_len
                : best_reject ? best_reject_len
                : last_non_hint ? last_non_hint_len
                : first_non_empty_len;
    if (!chosen || clen == 0) return;
    if (clen >= cap) clen = cap - 1;
    memcpy(out, chosen, clen);
    out[clen] = '\0';
}

bool mcode_git_get_sync_status(const char *workdir, mcode_git_sync_status *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!workdir || !workdir[0]) return false;

    const char *argv[] = {
        "git", "status", "--porcelain=v2", "--branch", "--untracked-files=no", NULL
    };
    int rc = -1;
    char *text = NULL;
    if (!mcode_proc_run_capture(argv, workdir, false, &rc, &text, NULL) || rc != 0 || !text) {
        free(text);
        return false;
    }

    out->valid = true;

    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            --len;
        }
        if (len == 0 || p[0] != '#') {
            break;
        }
        if (len > 13 && strncmp(p, "# branch.oid ", 13) == 0) {
            const char *val = p + 13;
            size_t vlen = len - 13;
            if (vlen == 9 && strncmp(val, "(initial)", 9) == 0) {
                out->is_unborn = true;
            }
        } else if (len > 14 && strncmp(p, "# branch.head ", 14) == 0) {
            const char *val = p + 14;
            size_t vlen = len - 14;
            if (vlen == 10 && strncmp(val, "(detached)", 10) == 0) {
                out->is_detached = true;
            } else if (vlen > 0) {
                size_t copy = vlen < sizeof(out->branch) - 1 ? vlen : sizeof(out->branch) - 1;
                memcpy(out->branch, val, copy);
                out->branch[copy] = '\0';
            }
        } else if (len > 18 && strncmp(p, "# branch.upstream ", 18) == 0) {
            const char *val = p + 18;
            size_t vlen = len - 18;
            if (vlen > 0) {
                size_t copy = vlen < sizeof(out->upstream) - 1 ? vlen : sizeof(out->upstream) - 1;
                memcpy(out->upstream, val, copy);
                out->upstream[copy] = '\0';
            }
        } else if (len > 12 && strncmp(p, "# branch.ab ", 12) == 0) {
            char ab_buf[64];
            size_t vlen = len - 12;
            if (vlen >= sizeof ab_buf) vlen = sizeof ab_buf - 1;
            memcpy(ab_buf, p + 12, vlen);
            ab_buf[vlen] = '\0';
            int a = 0, b = 0;
            if (sscanf(ab_buf, "+%d -%d", &a, &b) == 2) {
                out->has_upstream = true;
                out->ahead = a > 0 ? a : 0;
                out->behind = b > 0 ? b : 0;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(text);

    if (out->upstream[0] && !out->has_upstream) {
        out->upstream_gone = true;
    }

    char upstream_remote[64] = "";
    if (out->upstream[0]) {
        const char *slash = strchr(out->upstream, '/');
        if (slash && slash > out->upstream) {
            size_t rlen = (size_t)(slash - out->upstream);
            if (rlen >= sizeof upstream_remote) rlen = sizeof upstream_remote - 1;
            memcpy(upstream_remote, out->upstream, rlen);
            upstream_remote[rlen] = '\0';
        }
    }

    if (out->has_upstream) {
        out->has_remote = true;
        snprintf(out->remote, sizeof out->remote, "%s",
                 upstream_remote[0] ? upstream_remote : "origin");
    } else {
        const char *argv_rem[] = { "git", "remote", NULL };
        char *rem_out = NULL;
        if (mcode_proc_run_capture(argv_rem, workdir, false, &rc, &rem_out, NULL) &&
            rc == 0 && rem_out) {
            char first[64] = "";
            bool found_origin = false;
            bool found_upstream_rem = false;
            const char *q = rem_out;
            while (*q) {
                const char *eol = strchr(q, '\n');
                size_t rlen = eol ? (size_t)(eol - q) : strlen(q);
                while (rlen > 0 && (q[rlen - 1] == '\r' || q[rlen - 1] == ' ' || q[rlen - 1] == '\t')) {
                    --rlen;
                }
                if (rlen > 0 && rlen < sizeof first) {
                    char cand[64];
                    memcpy(cand, q, rlen);
                    cand[rlen] = '\0';
                    if (!first[0]) snprintf(first, sizeof first, "%s", cand);
                    if (strcmp(cand, "origin") == 0) found_origin = true;
                    if (upstream_remote[0] && strcmp(cand, upstream_remote) == 0) {
                        found_upstream_rem = true;
                    }
                }
                if (!eol) break;
                q = eol + 1;
            }
            if (found_upstream_rem) {
                out->has_remote = true;
                snprintf(out->remote, sizeof out->remote, "%s", upstream_remote);
            } else if (found_origin) {
                out->has_remote = true;
                snprintf(out->remote, sizeof out->remote, "origin");
            } else if (first[0]) {
                out->has_remote = true;
                snprintf(out->remote, sizeof out->remote, "%s", first);
            }
        }
        free(rem_out);
    }

    return true;
}

bool mcode_git_push(const char *workdir, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!workdir || !workdir[0]) return false;

    mcode_git_sync_status st;
    bool has_st = mcode_git_get_sync_status(workdir, &st);
    const char *remote = (has_st && st.remote[0]) ? st.remote : "origin";

    int rc = -1;
    char *out = NULL;
    bool ran = false;
    if (has_st && st.has_upstream) {
        const char *argv[] = { "git", "push", NULL };
        ran = mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL);
    } else {
        const char *argv[] = { "git", "push", "-u", remote, "HEAD", NULL };
        ran = mcode_proc_run_capture(argv, workdir, true, &rc, &out, NULL);
    }
    if (!ran || rc != 0) {
        if (err && err_cap) {
            if (out && out[0]) {
                extract_git_error_line(out, err, err_cap);
            }
            if (!err[0]) snprintf(err, err_cap, "git push failed.");
        }
        free(out);
        return false;
    }
    free(out);
    return true;
}

#ifdef _WIN32

struct mcode_git_op {
    mcode_git_op_kind kind;
    char workdir[MCODE_PATH_MAX];
    bool ok;
    char message[256];
};

mcode_git_op *mcode_git_op_start(mcode_git_op_kind kind, const char *workdir,
                                 const char *remote, bool has_upstream) {
    (void)remote;
    (void)has_upstream;
    if (kind == MCODE_GIT_OP_NONE || !workdir || !workdir[0]) return NULL;
    mcode_git_op *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    op->kind = kind;
    snprintf(op->workdir, sizeof op->workdir, "%s", workdir);
    op->ok = false;
    snprintf(op->message, sizeof op->message, "Git remote operations are not supported on this platform yet.");
    return op;
}

bool mcode_git_op_poll(mcode_git_op *op, bool *ok_out, char *msg_out, size_t msg_cap) {
    if (!op) return true;
    if (ok_out) *ok_out = op->ok;
    if (msg_out && msg_cap > 0) snprintf(msg_out, msg_cap, "%s", op->message);
    return true;
}

void mcode_git_op_stop(mcode_git_op *op) { (void)op; }
void mcode_git_op_kill(mcode_git_op *op) { (void)op; }
void mcode_git_op_destroy_timeout(mcode_git_op *op, int grace_ms) { (void)grace_ms; free(op); }
void mcode_git_op_destroy(mcode_git_op *op) { free(op); }
mcode_git_op_kind mcode_git_op_kind_of(const mcode_git_op *op) { return op ? op->kind : MCODE_GIT_OP_NONE; }
const char *mcode_git_op_workdir(const mcode_git_op *op) { return op ? op->workdir : ""; }

#else

struct mcode_git_op {
    mcode_git_op_kind kind;
    char workdir[MCODE_PATH_MAX];
    char remote[64];
    bool has_upstream;

    pthread_t thread;
    bool thread_started;
    pthread_mutex_t mu;
    pthread_cond_t done_cond;
    mcode_proc *active_proc;
    bool stop_requested;
    bool kill_requested;
    bool finished;
    bool ok;
    char message[256];
};

typedef struct {
    mcode_str buf;
    pthread_mutex_t mu;
    pthread_cond_t done_cond;
    bool done;
    int exit_code;
    bool killed;
} git_op_cmd_ctx;

static void git_op_cmd_on_bytes(const char *data, size_t len, bool is_stderr, void *user) {
    (void)is_stderr;
    git_op_cmd_ctx *ctx = (git_op_cmd_ctx *)user;
    pthread_mutex_lock(&ctx->mu);
    if (ctx->buf.len < 64 * 1024) {
        size_t take = len;
        if (ctx->buf.len + take > 64 * 1024) take = 64 * 1024 - ctx->buf.len;
        mcode_str_append(&ctx->buf, data, take);
    }
    pthread_mutex_unlock(&ctx->mu);
}

static void git_op_cmd_on_exit(int exit_code, bool killed, void *user) {
    git_op_cmd_ctx *ctx = (git_op_cmd_ctx *)user;
    pthread_mutex_lock(&ctx->mu);
    ctx->exit_code = exit_code;
    ctx->killed = killed;
    ctx->done = true;
    pthread_cond_broadcast(&ctx->done_cond);
    pthread_mutex_unlock(&ctx->mu);
}

static bool git_op_run_cmd(mcode_git_op *op, const char *const *argv,
                           int *out_exit, char **out_buf) {
    if (out_exit) *out_exit = -1;
    if (out_buf) *out_buf = NULL;

    pthread_mutex_lock(&op->mu);
    bool cancelled = op->stop_requested || op->kill_requested;
    pthread_mutex_unlock(&op->mu);
    if (cancelled) return false;

    git_op_cmd_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    mcode_str_init(&ctx.buf);
    pthread_mutex_init(&ctx.mu, NULL);
    pthread_cond_init(&ctx.done_cond, NULL);
    ctx.exit_code = -1;

    mcode_proc *proc = mcode_proc_spawn(&(mcode_proc_desc){
        .argv = argv,
        .cwd = op->workdir,
        .merge_stderr = true,
        .on_bytes = git_op_cmd_on_bytes,
        .on_exit = git_op_cmd_on_exit,
        .user = &ctx,
    });
    if (!proc) {
        pthread_cond_destroy(&ctx.done_cond);
        pthread_mutex_destroy(&ctx.mu);
        mcode_str_free(&ctx.buf);
        return false;
    }

    pthread_mutex_lock(&op->mu);
    op->active_proc = proc;
    bool want_kill = op->kill_requested;
    bool want_stop = op->stop_requested;
    if (want_kill) {
        mcode_proc_kill(proc);
    } else if (want_stop) {
        mcode_proc_stop(proc);
    }
    pthread_mutex_unlock(&op->mu);

    pthread_mutex_lock(&ctx.mu);
    while (!ctx.done) {
        pthread_cond_wait(&ctx.done_cond, &ctx.mu);
    }
    int rc = ctx.exit_code;
    bool killed = ctx.killed;
    pthread_mutex_unlock(&ctx.mu);

    pthread_mutex_lock(&op->mu);
    op->active_proc = NULL;
    pthread_mutex_unlock(&op->mu);

    mcode_proc_destroy_timeout(proc, 0);

    pthread_cond_destroy(&ctx.done_cond);
    pthread_mutex_destroy(&ctx.mu);

    if (out_exit) *out_exit = rc;
    if (out_buf) {
        if (ctx.buf.data) {
            *out_buf = ctx.buf.data;
            ctx.buf.data = NULL;
        } else {
            *out_buf = strdup("");
        }
    }
    mcode_str_free(&ctx.buf);
    return !killed;
}

static void *git_op_worker_main(void *arg) {
    mcode_git_op *op = (mcode_git_op *)arg;
    const char *remote = op->remote[0] ? op->remote : "origin";
    bool ok = false;
    char msg[256] = "";

    int rc = -1;
    char *out = NULL;

    if (op->kind == MCODE_GIT_OP_FETCH) {
        const char *argv[] = { "git", "fetch", NULL };
        if (git_op_run_cmd(op, argv, &rc, &out) && rc == 0) {
            ok = true;
            snprintf(msg, sizeof msg, "Fetched latest changes.");
        } else {
            extract_git_error_line(out, msg, sizeof msg);
            if (!msg[0]) snprintf(msg, sizeof msg, "git fetch failed.");
            if (out && out[0]) app_log_warn("git fetch failed in %s: %s", op->workdir, out);
        }
        free(out);
    } else if (op->kind == MCODE_GIT_OP_PULL) {
        const char *argv[] = { "git", "pull", "--no-edit", NULL };
        if (git_op_run_cmd(op, argv, &rc, &out) && rc == 0) {
            ok = true;
            snprintf(msg, sizeof msg, "Pulled latest changes.");
        } else {
            extract_git_error_line(out, msg, sizeof msg);
            if (!msg[0]) snprintf(msg, sizeof msg, "git pull failed.");
            if (out && out[0]) app_log_warn("git pull failed in %s: %s", op->workdir, out);
        }
        free(out);
    } else if (op->kind == MCODE_GIT_OP_PUBLISH) {
        const char *argv[] = { "git", "push", "-u", remote, "HEAD", NULL };
        if (git_op_run_cmd(op, argv, &rc, &out) && rc == 0) {
            ok = true;
            snprintf(msg, sizeof msg, "Branch published.");
        } else {
            extract_git_error_line(out, msg, sizeof msg);
            if (!msg[0]) snprintf(msg, sizeof msg, "Publish branch failed.");
            if (out && out[0]) app_log_warn("git push -u failed in %s: %s", op->workdir, out);
        }
        free(out);
    } else if (op->kind == MCODE_GIT_OP_PUSH) {
        bool ran = false;
        if (op->has_upstream) {
            const char *argv[] = { "git", "push", NULL };
            ran = git_op_run_cmd(op, argv, &rc, &out);
        } else {
            const char *argv[] = { "git", "push", "-u", remote, "HEAD", NULL };
            ran = git_op_run_cmd(op, argv, &rc, &out);
        }
        if (ran && rc == 0) {
            ok = true;
            snprintf(msg, sizeof msg, "Branch pushed.");
        } else {
            extract_git_error_line(out, msg, sizeof msg);
            if (!msg[0]) snprintf(msg, sizeof msg, "git push failed.");
            if (out && out[0]) app_log_warn("git push failed in %s: %s", op->workdir, out);
        }
        free(out);
    } else if (op->kind == MCODE_GIT_OP_SYNC) {
        const char *argv_pull[] = { "git", "pull", "--no-edit", NULL };
        if (!git_op_run_cmd(op, argv_pull, &rc, &out) || rc != 0) {
            extract_git_error_line(out, msg, sizeof msg);
            if (!msg[0]) snprintf(msg, sizeof msg, "Sync failed during pull.");
            if (out && out[0]) app_log_warn("git sync pull failed in %s: %s", op->workdir, out);
            free(out);
        } else {
            free(out);
            out = NULL;
            mcode_git_sync_status st;
            bool has_st = mcode_git_get_sync_status(op->workdir, &st);
            if (has_st && st.has_upstream && st.ahead == 0) {
                ok = true;
                snprintf(msg, sizeof msg, "Changes synced.");
            } else {
                bool ran = false;
                if (!has_st || st.has_upstream) {
                    const char *argv_push[] = { "git", "push", NULL };
                    ran = git_op_run_cmd(op, argv_push, &rc, &out);
                } else {
                    const char *push_rem = st.remote[0] ? st.remote : remote;
                    const char *argv_push[] = { "git", "push", "-u", push_rem, "HEAD", NULL };
                    ran = git_op_run_cmd(op, argv_push, &rc, &out);
                }
                if (ran && rc == 0) {
                    ok = true;
                    snprintf(msg, sizeof msg, "Changes synced.");
                } else {
                    extract_git_error_line(out, msg, sizeof msg);
                    if (!msg[0]) snprintf(msg, sizeof msg, "Sync failed during push.");
                    if (out && out[0]) app_log_warn("git sync push failed in %s: %s", op->workdir, out);
                }
                free(out);
            }
        }
    }

    pthread_mutex_lock(&op->mu);
    op->ok = ok;
    snprintf(op->message, sizeof op->message, "%s", msg);
    op->finished = true;
    pthread_cond_broadcast(&op->done_cond);
    pthread_mutex_unlock(&op->mu);
    return NULL;
}

mcode_git_op *mcode_git_op_start(mcode_git_op_kind kind, const char *workdir,
                                 const char *remote, bool has_upstream) {
    if (kind == MCODE_GIT_OP_NONE || !workdir || !workdir[0]) return NULL;
    mcode_git_op *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    op->kind = kind;
    snprintf(op->workdir, sizeof op->workdir, "%s", workdir);
    snprintf(op->remote, sizeof op->remote, "%s",
             (remote && remote[0]) ? remote : "origin");
    op->has_upstream = has_upstream;
    pthread_mutex_init(&op->mu, NULL);
    pthread_cond_init(&op->done_cond, NULL);

    if (pthread_create(&op->thread, NULL, git_op_worker_main, op) != 0) {
        pthread_cond_destroy(&op->done_cond);
        pthread_mutex_destroy(&op->mu);
        free(op);
        return NULL;
    }
    op->thread_started = true;
    return op;
}

bool mcode_git_op_poll(mcode_git_op *op, bool *ok_out, char *msg_out, size_t msg_cap) {
    if (ok_out) *ok_out = false;
    if (msg_out && msg_cap > 0) msg_out[0] = '\0';
    if (!op) return true;

    pthread_mutex_lock(&op->mu);
    bool done = op->finished;
    if (done) {
        if (ok_out) *ok_out = op->ok;
        if (msg_out && msg_cap > 0) snprintf(msg_out, msg_cap, "%s", op->message);
    }
    pthread_mutex_unlock(&op->mu);
    return done;
}

void mcode_git_op_stop(mcode_git_op *op) {
    if (!op) return;
    pthread_mutex_lock(&op->mu);
    op->stop_requested = true;
    if (op->active_proc) mcode_proc_stop(op->active_proc);
    pthread_mutex_unlock(&op->mu);
}

void mcode_git_op_kill(mcode_git_op *op) {
    if (!op) return;
    pthread_mutex_lock(&op->mu);
    op->stop_requested = true;
    op->kill_requested = true;
    if (op->active_proc) mcode_proc_kill(op->active_proc);
    pthread_mutex_unlock(&op->mu);
}

void mcode_git_op_destroy_timeout(mcode_git_op *op, int grace_ms) {
    if (!op) return;
    pthread_mutex_lock(&op->mu);
    bool done = op->finished;
    pthread_mutex_unlock(&op->mu);

    if (!done) {
        if (grace_ms > 0) {
            mcode_git_op_stop(op);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += grace_ms / 1000;
            ts.tv_nsec += (long)(grace_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&op->mu);
            while (!op->finished) {
                int wrc = pthread_cond_timedwait(&op->done_cond, &op->mu, &ts);
                if (wrc == ETIMEDOUT) break;
                if (wrc != 0 && wrc != EINTR) break;
            }
            done = op->finished;
            pthread_mutex_unlock(&op->mu);
        }
        if (!done) {
            mcode_git_op_kill(op);
        }
    }
    if (op->thread_started) {
        pthread_join(op->thread, NULL);
        op->thread_started = false;
    }
    pthread_cond_destroy(&op->done_cond);
    pthread_mutex_destroy(&op->mu);
    free(op);
}

void mcode_git_op_destroy(mcode_git_op *op) {
    mcode_git_op_destroy_timeout(op, 150);
}

mcode_git_op_kind mcode_git_op_kind_of(const mcode_git_op *op) {
    return op ? op->kind : MCODE_GIT_OP_NONE;
}

const char *mcode_git_op_workdir(const mcode_git_op *op) {
    return op ? op->workdir : "";
}

#endif

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

static size_t utf8_safe_prefix_len(const char *s, size_t len) {
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) {
        --len;
    }
    return len;
}

static void append_capped_section(mcode_str *dst, const char *header,
                                  const char *body, size_t max_body,
                                  size_t total_cap) {
    if (!dst || !body || !body[0] || dst->len >= total_cap) return;
    size_t hlen = header ? strlen(header) : 0;
    if (dst->len + hlen + 16 >= total_cap) return;

    if (header && hlen > 0) {
        mcode_str_append(dst, header, hlen);
    }

    size_t avail = total_cap - dst->len;
    size_t blen = strlen(body);
    size_t take = blen;
    bool truncated = false;
    if (max_body > 0 && take > max_body) {
        take = max_body;
        truncated = true;
    }
    if (take > avail) {
        take = avail;
        truncated = true;
    }
    if (truncated) {
        static const char k_trunc[] = "\n...[truncated]\n";
        size_t tlen = sizeof(k_trunc) - 1;
        if (take > tlen) {
            take = utf8_safe_prefix_len(body, take - tlen);
            mcode_str_append(dst, body, take);
            mcode_str_append(dst, k_trunc, tlen);
            return;
        }
        take = utf8_safe_prefix_len(body, take);
    }
    mcode_str_append(dst, body, take);
    if (dst->len > 0 && dst->data[dst->len - 1] != '\n' && dst->len + 1 < total_cap) {
        mcode_str_append(dst, "\n", 1);
    }
}

bool mcode_git_collect_commit_context(const char *workdir, bool staged_only,
                                      size_t max_bytes, char **out_context) {
    if (out_context) *out_context = NULL;
    if (!workdir || !workdir[0] || !out_context) return false;
    if (max_bytes < 512) max_bytes = 8192;

    mcode_str ctx;
    mcode_str_init(&ctx);
    bool has_changes = false;

    int rc = -1;
    char *out = NULL;

    /* 1. Recent commit subjects for style consistency (optional, ignored on unborn HEAD). */
    const char *argv_log[] = { "git", "log", "-n", "5", "--oneline", NULL };
    if (mcode_proc_run_capture(argv_log, workdir, false, &rc, &out, NULL) &&
        rc == 0 && out && out[0]) {
        trim_trailing(out);
        if (out[0]) {
            append_capped_section(&ctx, "Recent commits:\n", out, 480, max_bytes);
        }
    }
    free(out);
    out = NULL;

    /* 2. Changed file status and diff stat. */
    if (staged_only) {
        const char *argv_ns[] = { "git", "diff", "--cached", "--name-status", NULL };
        if (mcode_proc_run_capture(argv_ns, workdir, false, &rc, &out, NULL) &&
            rc == 0 && out && out[0]) {
            trim_trailing(out);
            if (out[0]) {
                has_changes = true;
                append_capped_section(&ctx, "\nStaged files:\n", out, 1200, max_bytes);
            }
        }
        free(out);
        out = NULL;

        const char *argv_stat[] = { "git", "diff", "--cached", "--stat", NULL };
        if (mcode_proc_run_capture(argv_stat, workdir, false, &rc, &out, NULL) &&
            rc == 0 && out && out[0]) {
            trim_trailing(out);
            if (out[0]) {
                has_changes = true;
                append_capped_section(&ctx, "\nDiff stat:\n", out, 1200, max_bytes);
            }
        }
        free(out);
        out = NULL;
    } else {
        const char *argv_st[] = { "git", "status", "--short", NULL };
        if (mcode_proc_run_capture(argv_st, workdir, false, &rc, &out, NULL) &&
            rc == 0 && out && out[0]) {
            trim_trailing(out);
            if (out[0]) {
                has_changes = true;
                append_capped_section(&ctx, "\nChanged files:\n", out, 1200, max_bytes);
            }
        }
        free(out);
        out = NULL;

        const char *argv_stat_head[] = { "git", "diff", "HEAD", "--stat", NULL };
        if (mcode_proc_run_capture(argv_stat_head, workdir, false, &rc, &out, NULL) &&
            rc == 0) {
            if (out && out[0]) {
                trim_trailing(out);
                if (out[0]) {
                    has_changes = true;
                    append_capped_section(&ctx, "\nDiff stat:\n", out, 1200, max_bytes);
                }
            }
            free(out);
            out = NULL;
        } else {
            free(out);
            out = NULL;
            /* Unborn HEAD fallback */
            const char *argv_stat_c[] = { "git", "diff", "--cached", "--stat", NULL };
            if (mcode_proc_run_capture(argv_stat_c, workdir, false, &rc, &out, NULL) &&
                rc == 0 && out && out[0]) {
                trim_trailing(out);
                if (out[0]) {
                    has_changes = true;
                    append_capped_section(&ctx, "\nStaged diff stat:\n", out, 800, max_bytes);
                }
            }
            free(out);
            out = NULL;

            const char *argv_stat_u[] = { "git", "diff", "--stat", NULL };
            if (mcode_proc_run_capture(argv_stat_u, workdir, false, &rc, &out, NULL) &&
                rc == 0 && out && out[0]) {
                trim_trailing(out);
                if (out[0]) {
                    has_changes = true;
                    append_capped_section(&ctx, "\nUnstaged diff stat:\n", out, 800, max_bytes);
                }
            }
            free(out);
            out = NULL;
        }
    }

    /* 3. Patch hunks (unified=2 for compact, informative context). */
    size_t diff_budget = (ctx.len < max_bytes) ? (max_bytes - ctx.len) : 0;
    if (!staged_only && diff_budget > 1536) {
        diff_budget -= 1024; /* Leave space for new/untracked file previews */
    }
    if (diff_budget > 64) {
        if (staged_only) {
            const char *argv_diff[] = {
                "git", "diff", "--cached", "--no-color", "--no-ext-diff", "--unified=2", NULL
            };
            if (mcode_proc_run_capture(argv_diff, workdir, false, &rc, &out, NULL) &&
                rc == 0 && out && out[0]) {
                trim_trailing(out);
                if (out[0]) {
                    has_changes = true;
                    append_capped_section(&ctx, "\nDiff:\n", out, diff_budget, max_bytes);
                }
            }
            free(out);
            out = NULL;
        } else {
            const char *argv_diff_head[] = {
                "git", "diff", "HEAD", "--no-color", "--no-ext-diff", "--unified=2", NULL
            };
            if (mcode_proc_run_capture(argv_diff_head, workdir, false, &rc, &out, NULL) &&
                rc == 0) {
                if (out && out[0]) {
                    trim_trailing(out);
                    if (out[0]) {
                        has_changes = true;
                        append_capped_section(&ctx, "\nDiff:\n", out, diff_budget, max_bytes);
                    }
                }
                free(out);
                out = NULL;
            } else {
                free(out);
                out = NULL;
                const char *argv_diff_c[] = {
                    "git", "diff", "--cached", "--no-color", "--no-ext-diff", "--unified=2", NULL
                };
                if (mcode_proc_run_capture(argv_diff_c, workdir, false, &rc, &out, NULL) &&
                    rc == 0 && out && out[0]) {
                    trim_trailing(out);
                    if (out[0]) {
                        has_changes = true;
                        append_capped_section(&ctx, "\nDiff:\n", out, diff_budget, max_bytes);
                    }
                }
                free(out);
                out = NULL;

                const char *argv_diff_u[] = {
                    "git", "diff", "--no-color", "--no-ext-diff", "--unified=2", NULL
                };
                if (mcode_proc_run_capture(argv_diff_u, workdir, false, &rc, &out, NULL) &&
                    rc == 0 && out && out[0]) {
                    trim_trailing(out);
                    if (out[0]) {
                        has_changes = true;
                        append_capped_section(&ctx, "\nUnstaged diff:\n", out, diff_budget, max_bytes);
                    }
                }
                free(out);
                out = NULL;
            }
        }
    }

    /* 4. Untracked file snippets when committing all changes. */
    if (!staged_only && ctx.len + 128 < max_bytes) {
        const char *argv_untracked[] = {
            "git", "ls-files", "--others", "--exclude-standard", NULL
        };
        if (mcode_proc_run_capture(argv_untracked, workdir, false, &rc, &out, NULL) &&
            rc == 0 && out && out[0]) {
            char *p = out;
            int shown = 0;
            while (*p && shown < 5 && ctx.len + 128 < max_bytes) {
                char *nl = strchr(p, '\n');
                size_t llen = nl ? (size_t)(nl - p) : strlen(p);
                while (llen > 0 && p[llen - 1] == '\r') --llen;
                if (llen > 0 && llen < 512) {
                    char rel[512];
                    memcpy(rel, p, llen);
                    rel[llen] = '\0';
                    has_changes = true;

                    char full[MCODE_PATH_MAX + 512];
                    snprintf(full, sizeof full, "%s/%s", workdir, rel);
                    char *snippet = read_file_capped(full, 1024);
                    if (snippet && snippet[0] && strcmp(snippet, "[binary file]\n") != 0) {
                        char hdr[544];
                        snprintf(hdr, sizeof hdr, "\nNew file %s:\n", rel);
                        append_capped_section(&ctx, hdr, snippet, 900, max_bytes);
                        ++shown;
                    }
                    free(snippet);
                }
                if (!nl) break;
                p = nl + 1;
            }
        }
        free(out);
        out = NULL;
    }

    if (!has_changes || ctx.len == 0) {
        mcode_str_free(&ctx);
        return false;
    }

    char *res = strdup(mcode_str_cstr(&ctx));
    mcode_str_free(&ctx);
    if (!res) return false;
    *out_context = res;
    return true;
}
