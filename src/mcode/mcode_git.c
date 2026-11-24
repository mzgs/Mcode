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
#include <sys/stat.h>
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
