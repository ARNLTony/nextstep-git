/*
 * git_core.h - Git client core for NeXTSTEP 3.3
 *
 * Pure logic layer: no printf, no stdin, no GUI calls.
 * Returns data via structs; reports progress via callback.
 *
 * (c) 2026 ARNLTony & Claude. MIT License.
 */

#ifndef GIT_CORE_H
#define GIT_CORE_H

/* --- Limits --- */

#define GIT_MAX_FILES       500
#define GIT_MAX_PATH        512
#define GIT_MAX_SHA          41     /* 40 hex + null */
#define GIT_MAX_MSG        1024
#define GIT_MAX_OWNER       128
#define GIT_MAX_REPO        128
#define GIT_MAX_BRANCH       64
#define GIT_MAX_TOKEN       256
#define GIT_MAX_AUTHOR      128
#define GIT_MAX_DATE         32
#define GIT_MAX_ERRMSG      512
#define GIT_MAX_COMMITS     100
#define GIT_STATE_FILE      ".nextstep_git"
#define GIT_PENDING_FILE    ".nextstep_git_pending"

#define GIT_RESPONSE_BUF  131072   /* 128KB for API responses */
#define GIT_HTTP_BUF       4096

/* --- File status --- */

#define GIT_STATUS_TRACKED     0    /* clean, matches remote */
#define GIT_STATUS_MODIFIED    1    /* local differs from tracked */
#define GIT_STATUS_STAGED      2    /* staged for commit */
#define GIT_STATUS_DELETED     3    /* tracked but removed locally */
#define GIT_STATUS_NEW         4    /* on disk, not tracked */
#define GIT_STATUS_UNTRACKED   5    /* not tracked at all */

/* --- Error codes --- */

#define GIT_OK                 0
#define GIT_ERR_NETWORK       -1
#define GIT_ERR_AUTH          -2
#define GIT_ERR_NOTFOUND      -3
#define GIT_ERR_DISK          -4
#define GIT_ERR_MEMORY        -5
#define GIT_ERR_STATE         -6
#define GIT_ERR_CONFLICT      -7
#define GIT_ERR_TOOLARGE      -8
#define GIT_ERR_NOCHANGES     -9
#define GIT_ERR_PARAM        -10

/* --- Progress events --- */

#define GIT_PROGRESS_START     0
#define GIT_PROGRESS_DOWNLOAD  1
#define GIT_PROGRESS_UPLOAD    2
#define GIT_PROGRESS_STATUS    3
#define GIT_PROGRESS_DONE      4
#define GIT_PROGRESS_ERROR     5

/* --- Data structures --- */

typedef struct GitProgress {
    int     event;
    int     current;
    int     total;
    char    message[GIT_MAX_ERRMSG];
} GitProgress;

/* Progress callback: return 0 to continue, -1 to cancel */
typedef int (*GitProgressFn)(GitProgress *progress, void *userdata);

typedef struct GitRepo {
    char    owner[GIT_MAX_OWNER];
    char    repo[GIT_MAX_REPO];
    char    branch[GIT_MAX_BRANCH];
    char    local_path[GIT_MAX_PATH];
    char    token[GIT_MAX_TOKEN];
    char    head_sha[GIT_MAX_SHA];
    char    tree_sha[GIT_MAX_SHA];
} GitRepo;

typedef struct GitFileEntry {
    char    path[GIT_MAX_PATH];
    char    sha[GIT_MAX_SHA];
    int     status;
    long    size;
} GitFileEntry;

typedef struct GitFileList {
    GitFileEntry files[GIT_MAX_FILES];
    int         count;
} GitFileList;

typedef struct GitCommit {
    char    sha[GIT_MAX_SHA];
    char    message[GIT_MAX_MSG];
    char    author[GIT_MAX_AUTHOR];
    char    date[GIT_MAX_DATE];
} GitCommit;

typedef struct GitCommitLog {
    GitCommit   commits[GIT_MAX_COMMITS];
    int         count;
} GitCommitLog;

typedef struct GitDiffEntry {
    char    path[GIT_MAX_PATH];
    int     status;
    char    *old_content;   /* from GitHub, NULL if new file */
    char    *new_content;   /* from disk, NULL if deleted */
    int     old_size;
    int     new_size;
} GitDiffEntry;

typedef struct GitDiffResult {
    GitDiffEntry *entries;  /* heap-allocated array */
    int          count;
} GitDiffResult;

typedef struct GitResult {
    int     code;
    char    message[GIT_MAX_ERRMSG];
    int     files_affected;
} GitResult;

/* --- Core operations --- */

GitResult git_init_repo(GitRepo *r, char *owner, char *repo,
                        char *branch, char *local_path, char *token);

GitResult git_clone(GitRepo *r, GitProgressFn progress_fn, void *userdata);

GitResult git_pull(GitRepo *r, GitProgressFn progress_fn, void *userdata);

GitResult git_status(GitRepo *r, GitFileList *out);

GitResult git_add(GitRepo *r, char *path, GitFileList *state);

GitResult git_add_all(GitRepo *r, GitFileList *state);

GitResult git_commit(GitRepo *r, char *message,
                     char *author_name, char *author_email,
                     GitFileList *state);

GitResult git_push(GitRepo *r, GitFileList *state,
                   GitProgressFn progress_fn, void *userdata);

GitResult git_log(GitRepo *r, int max_count, GitCommitLog *out);

GitResult git_diff(GitRepo *r, char *path, GitFileList *state,
                   GitDiffResult *out,
                   GitProgressFn progress_fn, void *userdata);

/* --- State persistence --- */

GitResult git_load_state(GitRepo *r, GitFileList *out);
GitResult git_save_state(GitRepo *r, GitFileList *state);
GitResult git_load_pending(GitRepo *r, char *message_out, int message_size);
GitResult git_save_pending(GitRepo *r, char *message);
GitResult git_clear_pending(GitRepo *r);

/* --- Memory management --- */

void git_diff_free(GitDiffResult *diff);

/* --- File utilities --- */

char *git_read_file(char *filepath, long *size_out);
int   git_write_file(char *filepath, char *data, long size);
int   git_is_directory(char *path);
int   git_is_file(char *path);
int   git_mkdir_p(char *path);
int   git_scan_directory(char *dirpath, char paths[][GIT_MAX_PATH],
                         int max_entries);

/* --- GitHub API (shared with gh) --- */

int   gh_api_request(char *token, char *method, char *path,
                     char *post_body, char *response, int response_size);

/* JSON helpers */
char *json_find_string(char *json, char *key, int *out_len);
long  json_find_number(char *json, char *key);
int   json_find_bool(char *json, char *key);
char *json_array_first(char *json, char **end);
char *json_array_next(char *pos, char **end);
void  json_unescape(char *src, int src_len, char *dst, int dst_size);
void  json_escape(char *src, char *dst, int dst_size);
int   gh_base64_decode(char *src, int src_len, char *dst, int dst_size);
int   gh_base64_encode(char *src, int src_len, char *dst, int dst_size);

#endif /* GIT_CORE_H */
