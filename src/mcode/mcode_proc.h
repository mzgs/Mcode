#ifndef MCODE_PROC_H
#define MCODE_PROC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct mcode_proc mcode_proc;

typedef void (*mcode_proc_bytes_fn)(const char *data, size_t len, bool is_stderr, void *user);
typedef void (*mcode_proc_exit_fn)(int exit_code, bool killed, void *user);

typedef struct {
    const char *const *argv;   /* NULL-terminated argument vector */
    const char *cwd;           /* Optional working directory */
    bool merge_stderr;         /* Redirect stderr to stdout pipe */
    bool open_stdin;           /* Keep a writable stdin pipe open instead of /dev/null */
    mcode_proc_bytes_fn on_bytes;
    mcode_proc_exit_fn on_exit;
    void *user;
} mcode_proc_desc;

/* Spawn an asynchronous subprocess. Callbacks fire on a reader thread. */
mcode_proc *mcode_proc_spawn(const mcode_proc_desc *desc);

/* Write bytes to the subprocess's stdin (requires open_stdin=true). Thread-safe. */
bool mcode_proc_write(mcode_proc *proc, const char *data, size_t len);

/* Close the subprocess's stdin pipe (signals EOF). Thread-safe. */
void mcode_proc_close_stdin(mcode_proc *proc);

/* Spawn an interactive shell command (`sh -c <cmd>`) with merged output. */
mcode_proc *mcode_proc_spawn_shell(const char *command, const char *cwd,
                                   mcode_proc_bytes_fn on_bytes,
                                   mcode_proc_exit_fn on_exit, void *user);

/* Request termination (SIGTERM to process group). */
void mcode_proc_stop(mcode_proc *proc);

/* Immediately force-terminate (SIGKILL to process group) and abort reader I/O. */
void mcode_proc_kill(mcode_proc *proc);

/* True while the process has not yet finished its exit callback. */
bool mcode_proc_is_running(const mcode_proc *proc);

/* Wait for reader thread to finish and release resources. */
void mcode_proc_destroy(mcode_proc *proc);

/* Destroy with an explicit graceful-SIGTERM wait budget in ms (0 = immediate SIGKILL). */
void mcode_proc_destroy_timeout(mcode_proc *proc, int grace_ms);

/* Run a command synchronously and capture stdout (and optionally stderr).
 * Returns true if the process ran; *out_exit receives exit code.
 * Caller frees *out_buf (if non-NULL). */
bool mcode_proc_run_capture(const char *const *argv, const char *cwd,
                            bool merge_stderr, int *out_exit,
                            char **out_buf, size_t *out_len);

/* Resolve an executable name or path (checks explicit path, PATH, ~/.local/bin, /opt/homebrew/bin, /usr/local/bin).
 * Returns true if executable was found and writes its resolved path to out. */
bool mcode_find_executable(const char *name_or_path, char *out, size_t cap);

#endif
