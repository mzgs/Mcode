#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mcode_files.h"
#include "mcode_proc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <windows.h>
#include <sys/stat.h>
#endif

enum {
    MCODE_FS_MAX_DIR_ENTRIES = 2048,
    MCODE_FS_MAX_NODES = 32768,
    MCODE_FS_PREVIEW_MAX_BYTES = 512 * 1024,
};

typedef struct {
    char name[256];
    bool is_dir;
} fs_dir_entry;

void mcode_file_tree_init(mcode_file_tree *tree) {
    if (!tree) return;
    memset(tree, 0, sizeof *tree);
    tree->first_root = -1;
}

void mcode_file_tree_clear(mcode_file_tree *tree) {
    if (!tree) return;
    free(tree->nodes);
    tree->nodes = NULL;
    tree->count = 0;
    tree->cap = 0;
    tree->first_root = -1;

    free(tree->ignored_paths);
    tree->ignored_paths = NULL;
    tree->ignored_count = 0;
    tree->ignored_cap = 0;

    free(tree->git_files);
    tree->git_files = NULL;
    tree->git_file_count = 0;

    tree->root_path[0] = '\0';
    tree->is_git_repo = false;
}

void mcode_file_tree_free(mcode_file_tree *tree) {
    mcode_file_tree_clear(tree);
}

static int compare_icase_str(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
    if (diff != 0) return diff;
    return strcmp(a, b);
}

static int compare_dir_entries(const void *va, const void *vb) {
    const fs_dir_entry *a = (const fs_dir_entry *)va;
    const fs_dir_entry *b = (const fs_dir_entry *)vb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return compare_icase_str(a->name, b->name);
}

static bool path_is_ignored(const mcode_file_tree *tree, const char *rel_path) {
    if (!tree || !rel_path || !rel_path[0]) return false;
    size_t rlen = strlen(rel_path);
    for (int i = 0; i < tree->ignored_count; ++i) {
        const char *ig = tree->ignored_paths[i];
        size_t ilen = strlen(ig);
        if (ilen == rlen && strcmp(rel_path, ig) == 0) return true;
        if (rlen > ilen && rel_path[ilen] == '/' && memcmp(rel_path, ig, ilen) == 0) {
            return true;
        }
    }
    return false;
}

static char status_char_from_diff(const mcode_diff_file *f) {
    if (!f) return 0;
    if (f->untracked || f->status[0] == '?') return 'U';
    return f->status[0];
}

static void apply_git_status_to_nodes(mcode_file_tree *tree) {
    if (!tree || tree->count <= 0) return;

    for (int i = 0; i < tree->count; ++i) {
        tree->nodes[i].git_status = 0;
        tree->nodes[i].has_changed_descendant = false;
    }

    if (!tree->git_files || tree->git_file_count <= 0) return;

    for (int i = 0; i < tree->count; ++i) {
        mcode_fs_node *n = &tree->nodes[i];
        size_t nlen = strlen(n->rel_path);
        if (n->is_dir) {
            for (int j = 0; j < tree->git_file_count; ++j) {
                const char *gp = tree->git_files[j].path;
                if (strncmp(gp, n->rel_path, nlen) == 0 && gp[nlen] == '/') {
                    n->has_changed_descendant = true;
                    break;
                }
            }
        } else {
            for (int j = 0; j < tree->git_file_count; ++j) {
                if (strcmp(tree->git_files[j].path, n->rel_path) == 0) {
                    n->git_status = status_char_from_diff(&tree->git_files[j]);
                    break;
                }
            }
        }
    }
}

void mcode_file_tree_apply_git_status(mcode_file_tree *tree,
                                      const mcode_diff_file *git_files, int git_file_count) {
    if (!tree) return;
    free(tree->git_files);
    tree->git_files = NULL;
    tree->git_file_count = 0;

    if (git_files && git_file_count > 0) {
        tree->git_files = malloc((size_t)git_file_count * sizeof *tree->git_files);
        if (tree->git_files) {
            memcpy(tree->git_files, git_files, (size_t)git_file_count * sizeof *tree->git_files);
            tree->git_file_count = git_file_count;
        }
    }
    apply_git_status_to_nodes(tree);
}

static void load_git_ignored(mcode_file_tree *tree) {
    free(tree->ignored_paths);
    tree->ignored_paths = NULL;
    tree->ignored_count = 0;
    tree->ignored_cap = 0;

    if (!tree->is_git_repo || !tree->root_path[0]) return;

    const char *argv[] = { "git", "status", "--ignored=matching", "--porcelain=v1", NULL };
    int rc = -1;
    char *out = NULL;
    if (!mcode_proc_run_capture(argv, tree->root_path, false, &rc, &out, NULL) || rc != 0 || !out) {
        free(out);
        return;
    }

    char *p = out;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len >= 4 && p[0] == '!' && p[1] == '!' && p[2] == ' ') {
            const char *path = p + 3;
            size_t plen = line_len - 3;
            while (plen > 0 && (path[plen - 1] == '\r' || path[plen - 1] == '\n')) --plen;
            if (plen >= 2 && path[0] == '"' && path[plen - 1] == '"') {
                path++;
                plen -= 2;
            }
            while (plen > 0 && path[plen - 1] == '/') --plen;
            if (plen > 0 && plen < 512) {
                if (tree->ignored_count >= tree->ignored_cap) {
                    int ncap = tree->ignored_cap ? tree->ignored_cap * 2 : 32;
                    if (ncap > 4096) break;
                    char (*grown)[512] = realloc(tree->ignored_paths, (size_t)ncap * sizeof *grown);
                    if (!grown) break;
                    tree->ignored_paths = grown;
                    tree->ignored_cap = ncap;
                }
                memcpy(tree->ignored_paths[tree->ignored_count], path, plen);
                tree->ignored_paths[tree->ignored_count][plen] = '\0';
                tree->ignored_count++;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(out);
}

static int append_node(mcode_file_tree *tree, int parent_idx, int depth,
                       const char *name, const char *rel_path,
                       bool is_dir, bool git_ignored) {
    if (!tree || tree->count >= MCODE_FS_MAX_NODES) return -1;
    if (tree->count >= tree->cap) {
        int ncap = tree->cap ? tree->cap * 2 : 64;
        if (ncap > MCODE_FS_MAX_NODES) ncap = MCODE_FS_MAX_NODES;
        mcode_fs_node *grown = realloc(tree->nodes, (size_t)ncap * sizeof *grown);
        if (!grown) return -1;
        tree->nodes = grown;
        tree->cap = ncap;
    }

    int idx = tree->count++;
    mcode_fs_node *n = &tree->nodes[idx];
    memset(n, 0, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->rel_path, sizeof n->rel_path, "%s", rel_path);
    n->parent_idx = parent_idx;
    n->first_child = -1;
    n->next_sibling = -1;
    n->depth = depth;
    n->is_dir = is_dir;
    n->git_ignored = git_ignored;
    return idx;
}

static void load_directory_children(mcode_file_tree *tree, int parent_idx) {
    if (!tree || !tree->root_path[0]) return;
    if (parent_idx >= 0) {
        if (parent_idx >= tree->count || !tree->nodes[parent_idx].is_dir) return;
        if (tree->nodes[parent_idx].children_loaded) return;
    }

    char dir_path[MCODE_PATH_MAX + 512];
    char parent_rel[512] = "";
    int depth = 0;
    bool parent_ignored = false;

    if (parent_idx >= 0) {
        snprintf(parent_rel, sizeof parent_rel, "%s", tree->nodes[parent_idx].rel_path);
        depth = tree->nodes[parent_idx].depth + 1;
        parent_ignored = tree->nodes[parent_idx].git_ignored;
        snprintf(dir_path, sizeof dir_path, "%s/%s", tree->root_path, parent_rel);
    } else {
        snprintf(dir_path, sizeof dir_path, "%s", tree->root_path);
    }

    int ent_cap = 64;
    int ent_count = 0;
    fs_dir_entry *entries = malloc((size_t)ent_cap * sizeof *entries);
    if (!entries) return;

#ifndef _WIN32
    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strcmp(de->d_name, ".git") == 0) continue;
            if (ent_count >= MCODE_FS_MAX_DIR_ENTRIES) break;

            bool is_dir = false;
#ifdef DT_DIR
            if (de->d_type == DT_DIR) {
                is_dir = true;
            } else if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
                char full[MCODE_PATH_MAX + 768];
                snprintf(full, sizeof full, "%s/%s", dir_path, de->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
            }
#else
            char full[MCODE_PATH_MAX + 768];
            snprintf(full, sizeof full, "%s/%s", dir_path, de->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
#endif
            if (ent_count >= ent_cap) {
                int ncap = ent_cap * 2;
                fs_dir_entry *grown = realloc(entries, (size_t)ncap * sizeof *grown);
                if (!grown) break;
                entries = grown;
                ent_cap = ncap;
            }
            snprintf(entries[ent_count].name, sizeof entries[ent_count].name, "%s", de->d_name);
            entries[ent_count].is_dir = is_dir;
            ent_count++;
        }
        closedir(d);
    }
#else
    char pattern[MCODE_PATH_MAX + 520];
    snprintf(pattern, sizeof pattern, "%s\\*", dir_path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (strcmp(fd.cFileName, ".git") == 0) continue;
            if (ent_count >= MCODE_FS_MAX_DIR_ENTRIES) break;
            if (ent_count >= ent_cap) {
                int ncap = ent_cap * 2;
                fs_dir_entry *grown = realloc(entries, (size_t)ncap * sizeof *grown);
                if (!grown) break;
                entries = grown;
                ent_cap = ncap;
            }
            snprintf(entries[ent_count].name, sizeof entries[ent_count].name, "%s", fd.cFileName);
            entries[ent_count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            ent_count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif

    if (ent_count > 1) {
        qsort(entries, (size_t)ent_count, sizeof entries[0], compare_dir_entries);
    }

    int first_child = -1;
    int prev_child = -1;
    for (int i = 0; i < ent_count; ++i) {
        char rel[512];
        if (parent_rel[0]) {
            int n = snprintf(rel, sizeof rel, "%s/%s", parent_rel, entries[i].name);
            if (n <= 0 || (size_t)n >= sizeof rel) continue;
        } else {
            int n = snprintf(rel, sizeof rel, "%s", entries[i].name);
            if (n <= 0 || (size_t)n >= sizeof rel) continue;
        }
        bool ignored = parent_ignored || path_is_ignored(tree, rel);
        int idx = append_node(tree, parent_idx, depth, entries[i].name, rel,
                              entries[i].is_dir, ignored);
        if (idx < 0) break;
        if (first_child < 0) first_child = idx;
        if (prev_child >= 0) tree->nodes[prev_child].next_sibling = idx;
        prev_child = idx;
    }

    free(entries);

    if (parent_idx >= 0) {
        tree->nodes[parent_idx].first_child = first_child;
        tree->nodes[parent_idx].children_loaded = true;
    } else {
        tree->first_root = first_child;
    }

    apply_git_status_to_nodes(tree);
}

static int find_node_by_rel_path(const mcode_file_tree *tree, const char *rel_path) {
    if (!tree || !rel_path) return -1;
    for (int i = 0; i < tree->count; ++i) {
        if (strcmp(tree->nodes[i].rel_path, rel_path) == 0) return i;
    }
    return -1;
}

static int compare_path_depth(const void *va, const void *vb) {
    const char *a = *(const char *const *)va;
    const char *b = *(const char *const *)vb;
    int da = 0, db = 0;
    for (const char *p = a; *p; ++p) if (*p == '/') ++da;
    for (const char *p = b; *p; ++p) if (*p == '/') ++db;
    if (da != db) return da - db;
    return strcmp(a, b);
}

void mcode_file_tree_refresh(mcode_file_tree *tree, const char *root_path, bool is_git_repo,
                             const mcode_diff_file *git_files, int git_file_count) {
    if (!tree) return;
    if (!root_path || !root_path[0]) {
        mcode_file_tree_clear(tree);
        return;
    }

    char norm_root[MCODE_PATH_MAX];
    mcode_normalize_path(norm_root, sizeof norm_root, root_path);
    if (!norm_root[0]) {
        mcode_file_tree_clear(tree);
        return;
    }

    bool same_root = (strcmp(tree->root_path, norm_root) == 0);
    char **saved_expanded = NULL;
    int saved_count = 0;

    if (same_root && tree->count > 0) {
        saved_expanded = malloc((size_t)tree->count * sizeof *saved_expanded);
        if (saved_expanded) {
            for (int i = 0; i < tree->count; ++i) {
                if (tree->nodes[i].is_dir && tree->nodes[i].expanded) {
                    char *dup = strdup(tree->nodes[i].rel_path);
                    if (dup) saved_expanded[saved_count++] = dup;
                }
            }
            if (saved_count > 1) {
                qsort(saved_expanded, (size_t)saved_count, sizeof saved_expanded[0],
                      compare_path_depth);
            }
        }
    }

    mcode_file_tree_clear(tree);
    snprintf(tree->root_path, sizeof tree->root_path, "%s", norm_root);
    tree->is_git_repo = is_git_repo;

    if (git_files && git_file_count > 0) {
        tree->git_files = malloc((size_t)git_file_count * sizeof *tree->git_files);
        if (tree->git_files) {
            memcpy(tree->git_files, git_files, (size_t)git_file_count * sizeof *tree->git_files);
            tree->git_file_count = git_file_count;
        }
    }

    load_git_ignored(tree);
    load_directory_children(tree, -1);

    if (saved_expanded) {
        for (int i = 0; i < saved_count; ++i) {
            int idx = find_node_by_rel_path(tree, saved_expanded[i]);
            if (idx >= 0 && tree->nodes[idx].is_dir) {
                tree->nodes[idx].expanded = true;
                load_directory_children(tree, idx);
            }
            free(saved_expanded[i]);
        }
        free(saved_expanded);
    }
}

bool mcode_file_tree_toggle(mcode_file_tree *tree, int node_idx) {
    if (!tree || node_idx < 0 || node_idx >= tree->count) return false;
    if (!tree->nodes[node_idx].is_dir) return false;
    tree->nodes[node_idx].expanded = !tree->nodes[node_idx].expanded;
    if (tree->nodes[node_idx].expanded && !tree->nodes[node_idx].children_loaded) {
        load_directory_children(tree, node_idx);
    }
    return tree->nodes[node_idx].expanded;
}

void mcode_file_tree_collapse_all(mcode_file_tree *tree) {
    if (!tree) return;
    for (int i = 0; i < tree->count; ++i) {
        tree->nodes[i].expanded = false;
    }
}

bool mcode_file_read_preview(const char *root_path, const char *rel_path,
                             char **out_buf, size_t *out_cap,
                             bool *out_is_binary, bool *out_truncated,
                             int64_t *out_file_size) {
    if (out_buf) *out_buf = NULL;
    if (out_cap) *out_cap = 0;
    if (out_is_binary) *out_is_binary = false;
    if (out_truncated) *out_truncated = false;
    if (out_file_size) *out_file_size = 0;
    if (!root_path || !root_path[0] || !rel_path || !rel_path[0]) return false;

    char full[MCODE_PATH_MAX + 512];
    int fn = snprintf(full, sizeof full, "%s/%s", root_path, rel_path);
    if (fn <= 0 || (size_t)fn >= sizeof full) return false;

    FILE *f = fopen(full, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    fseek(f, 0, SEEK_SET);
    if (out_file_size) *out_file_size = (int64_t)sz;

    bool truncated = false;
    size_t to_read = (size_t)sz;
    if (to_read > MCODE_FS_PREVIEW_MAX_BYTES) {
        to_read = MCODE_FS_PREVIEW_MAX_BYTES;
        truncated = true;
    }

    size_t alloc_cap = to_read + 1;
    char *buf = malloc(alloc_cap);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t n = (to_read > 0) ? fread(buf, 1, to_read, f) : 0;
    buf[n] = '\0';
    fclose(f);

    /* Detect binary files by scanning for NUL bytes in the first 8KB (or full read). */
    size_t probe_len = n < 8192 ? n : 8192;
    bool is_binary = (probe_len > 0 && memchr(buf, '\0', probe_len) != NULL);
    if (!is_binary && n > probe_len && memchr(buf + probe_len, '\0', n - probe_len) != NULL) {
        is_binary = true;
    }

    if (is_binary) {
        free(buf);
        buf = strdup("");
        alloc_cap = 1;
    }

    if (out_buf) *out_buf = buf;
    else free(buf);
    if (out_cap) *out_cap = alloc_cap;
    if (out_is_binary) *out_is_binary = is_binary;
    if (out_truncated) *out_truncated = truncated;
    return true;
}
