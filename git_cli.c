/*
 * git_cli.c - Git CLI frontend for NeXTSTEP 3.3
 *
 * A simplified git command-line client that talks to GitHub
 * via the git_core library.
 *
 * Build: cc -O -o git git_cli.c git_core.c
 * Usage: ./git clone owner/repo [branch] [dir]
 *        ./git status
 *        ./git add file [file2 ...]
 *        ./git commit -m "message"
 *        ./git push
 *        ./git pull
 *        ./git log [-n count]
 *        ./git diff [file]
 *        ./git diff base..head
 *        ./git branch
 *        ./git branch <name>
 *        ./git branch -d <name>
 *        ./git checkout <branch>
 *        ./git merge <branch> [-m "message"]
 *        ./git tag
 *        ./git tag <name>
 *        ./git tag -a <name> -m "message"
 *        ./git release list
 *        ./git release create <tag> [-t "title"] [-m "body"]
 *        ./git fork
 *        ./git rm <file>
 *
 * (c) 2026 ARNLTony & Claude. MIT License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include "git_core.h"

/* NeXTSTEP uses getwd() instead of getcwd() */
#define getcwd(buf, size) getwd(buf)

#define TOKEN_FILE    ".github_token"
#define PROG_NAME     "git"

/* --- Token loading --- */

/*
 * Load GitHub token from .github_token file.
 * Search order: cwd, $HOME, /me/.github_token
 * Returns 0 on success, -1 on failure.
 */
static int load_token(buf, buf_size)
char *buf;
int buf_size;
{
    FILE *fp;
    int len;
    char path[GIT_MAX_PATH];
    char *home;

    /* Try current directory first */
    fp = fopen(TOKEN_FILE, "r");

    /* Try home directory */
    if (!fp) {
        home = getenv("HOME");
        if (home) {
            sprintf(path, "%s/%s", home, TOKEN_FILE);
            fp = fopen(path, "r");
        }
    }

    /* Try /me/.github_token */
    if (!fp) {
        sprintf(path, "/me/%s", TOKEN_FILE);
        fp = fopen(path, "r");
    }

    if (!fp) return -1;

    if (fgets(buf, buf_size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Strip trailing whitespace */
    len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
           buf[len-1] == ' ' || buf[len-1] == '\t'))
        buf[--len] = '\0';

    return (len > 0) ? 0 : -1;
}

/* --- Repo discovery --- */

/*
 * Find a .nextstep_git state file in the current directory.
 * Loads the repo state into *r and file list into *fl.
 * Returns 0 on success, -1 if not in a repo.
 */
static int find_repo(r, fl, token)
GitRepo *r;
GitFileList *fl;
char *token;
{
    GitResult res;
    char cwd[GIT_MAX_PATH];

    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "fatal: cannot determine current directory\n");
        return -1;
    }

    /* Check if .nextstep_git exists in cwd */
    {
        char state_path[GIT_MAX_PATH];
        sprintf(state_path, "%s/%s", cwd, GIT_STATE_FILE);
        if (!git_is_file(state_path)) {
            fprintf(stderr,
                "fatal: not a git repository (no %s found)\n",
                GIT_STATE_FILE);
            return -1;
        }
    }

    /* Set local_path and token so git_load_state can find the file */
    strncpy(r->local_path, cwd, GIT_MAX_PATH - 1);
    r->local_path[GIT_MAX_PATH - 1] = '\0';
    strncpy(r->token, token, GIT_MAX_TOKEN - 1);
    r->token[GIT_MAX_TOKEN - 1] = '\0';

    res = git_load_state(r, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "fatal: %s\n", res.message);
        return -1;
    }

    return 0;
}

/* --- Progress callback --- */

static int cli_progress(progress, userdata)
GitProgress *progress;
void *userdata;
{
    switch (progress->event) {
    case GIT_PROGRESS_START:
        printf("  %s\n", progress->message);
        fflush(stdout);
        break;

    case GIT_PROGRESS_DOWNLOAD:
        if (progress->total > 0) {
            printf("  Downloading file %d of %d: %s\r",
                   progress->current, progress->total,
                   progress->message);
        } else {
            printf("  Downloading: %s\r", progress->message);
        }
        fflush(stdout);
        break;

    case GIT_PROGRESS_UPLOAD:
        if (progress->total > 0) {
            printf("  Uploading file %d of %d: %s\r",
                   progress->current, progress->total,
                   progress->message);
        } else {
            printf("  Uploading: %s\r", progress->message);
        }
        fflush(stdout);
        break;

    case GIT_PROGRESS_STATUS:
        printf("  %s\r", progress->message);
        fflush(stdout);
        break;

    case GIT_PROGRESS_DONE:
        printf("  %s\n", progress->message);
        fflush(stdout);
        break;

    case GIT_PROGRESS_ERROR:
        fprintf(stderr, "  error: %s\n", progress->message);
        fflush(stderr);
        break;

    default:
        break;
    }

    return 0;
}

/* --- Subcommand: clone --- */

static int cmd_clone(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitResult res;
    char owner[GIT_MAX_OWNER];
    char reponame[GIT_MAX_REPO];
    char branch[GIT_MAX_BRANCH];
    char local_path[GIT_MAX_PATH];
    char cwd[GIT_MAX_PATH];
    char *slash;
    int owner_len;
    int argi;

    /* argv[0] = "clone", argv[1] = "owner/repo", ... */
    if (argc < 2) {
        fprintf(stderr, "usage: %s clone owner/repo [branch] [directory]\n",
                PROG_NAME);
        return 1;
    }

    /* Parse owner/repo */
    slash = strchr(argv[1], '/');
    if (!slash) {
        fprintf(stderr, "fatal: repository '%s' not in owner/repo format\n",
                argv[1]);
        return 1;
    }

    owner_len = slash - argv[1];
    if (owner_len <= 0 || owner_len >= GIT_MAX_OWNER) {
        fprintf(stderr, "fatal: owner name too long\n");
        return 1;
    }
    strncpy(owner, argv[1], owner_len);
    owner[owner_len] = '\0';

    strncpy(reponame, slash + 1, GIT_MAX_REPO - 1);
    reponame[GIT_MAX_REPO - 1] = '\0';

    if (strlen(reponame) == 0) {
        fprintf(stderr, "fatal: empty repository name\n");
        return 1;
    }

    /* Default branch */
    branch[0] = '\0';

    /* Default local path: ./reponame */
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "fatal: cannot determine current directory\n");
        return 1;
    }
    sprintf(local_path, "%s/%s", cwd, reponame);

    /* Parse optional branch and directory */
    argi = 2;
    if (argi < argc) {
        /* Could be branch or directory -- if there's another arg after,
           this one is branch and next is directory */
        if (argi + 1 < argc) {
            /* Two more args: branch then dir */
            strncpy(branch, argv[argi], GIT_MAX_BRANCH - 1);
            branch[GIT_MAX_BRANCH - 1] = '\0';
            argi++;

            /* Directory: absolute or relative */
            if (argv[argi][0] == '/') {
                strncpy(local_path, argv[argi], GIT_MAX_PATH - 1);
                local_path[GIT_MAX_PATH - 1] = '\0';
            } else {
                sprintf(local_path, "%s/%s", cwd, argv[argi]);
            }
        } else {
            /* One more arg: could be branch name or directory.
               Heuristic: if it contains '/' or '.', treat as dir.
               Otherwise treat as branch. */
            if (strchr(argv[argi], '/') || strchr(argv[argi], '.')) {
                /* Looks like a directory */
                if (argv[argi][0] == '/') {
                    strncpy(local_path, argv[argi], GIT_MAX_PATH - 1);
                    local_path[GIT_MAX_PATH - 1] = '\0';
                } else {
                    sprintf(local_path, "%s/%s", cwd, argv[argi]);
                }
            } else {
                /* Treat as branch name */
                strncpy(branch, argv[argi], GIT_MAX_BRANCH - 1);
                branch[GIT_MAX_BRANCH - 1] = '\0';
            }
        }
    }

    printf("Cloning %s/%s into %s...\n", owner, reponame, local_path);
    if (branch[0])
        printf("Branch: %s\n", branch);

    /* Initialize repo struct */
    res = git_init_repo(&repo, owner, reponame,
                        branch[0] ? branch : NULL,
                        local_path, token);
    if (res.code != GIT_OK) {
        fprintf(stderr, "fatal: %s\n", res.message);
        return 1;
    }

    /* Perform clone */
    res = git_clone(&repo, cli_progress, NULL);
    if (res.code != GIT_OK) {
        fprintf(stderr, "fatal: %s\n", res.message);
        return 1;
    }

    printf("\nClone complete. %d files downloaded.\n", res.files_affected);
    return 0;
}

/* --- Subcommand: status --- */

static int cmd_status(token)
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    int i;
    int n_modified, n_new, n_deleted, n_staged, n_untracked;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    res = git_status(&repo, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("On branch %s\n", repo.branch);
    printf("Repository: %s/%s\n\n", repo.owner, repo.repo);

    n_modified = 0;
    n_new = 0;
    n_deleted = 0;
    n_staged = 0;
    n_untracked = 0;

    /* Print staged files first */
    for (i = 0; i < fl->count; i++) {
        if (fl->files[i].status == GIT_STATUS_STAGED) {
            if (n_staged == 0)
                printf("Changes to be committed:\n");
            printf("  Staged:   %s\n", fl->files[i].path);
            n_staged++;
        }
    }
    if (n_staged > 0)
        printf("\n");

    /* Print modified/new/deleted */
    for (i = 0; i < fl->count; i++) {
        if (fl->files[i].status == GIT_STATUS_MODIFIED) {
            if (n_modified == 0 && n_new == 0 && n_deleted == 0)
                printf("Changes not staged for commit:\n");
            printf("  Modified: %s\n", fl->files[i].path);
            n_modified++;
        } else if (fl->files[i].status == GIT_STATUS_NEW) {
            if (n_modified == 0 && n_new == 0 && n_deleted == 0)
                printf("Changes not staged for commit:\n");
            printf("  New file: %s\n", fl->files[i].path);
            n_new++;
        } else if (fl->files[i].status == GIT_STATUS_DELETED) {
            if (n_modified == 0 && n_new == 0 && n_deleted == 0)
                printf("Changes not staged for commit:\n");
            printf("  Deleted:  %s\n", fl->files[i].path);
            n_deleted++;
        }
    }
    if (n_modified + n_new + n_deleted > 0)
        printf("\n");

    /* Print untracked files */
    for (i = 0; i < fl->count; i++) {
        if (fl->files[i].status == GIT_STATUS_UNTRACKED) {
            if (n_untracked == 0)
                printf("Untracked files:\n");
            printf("  %s\n", fl->files[i].path);
            n_untracked++;
        }
    }
    if (n_untracked > 0)
        printf("\n");

    if (n_modified + n_new + n_deleted + n_staged + n_untracked == 0)
        printf("nothing to commit, working tree clean\n");

    free(fl);
    return 0;
}

/* --- Subcommand: add --- */

static int cmd_add(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    int i;

    /* argv[0] = "add", argv[1..] = files */
    if (argc < 2) {
        fprintf(stderr, "usage: %s add <file> [file2 ...]\n", PROG_NAME);
        fprintf(stderr, "   or: %s add .\n", PROG_NAME);
        return 1;
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    /* "git add ." stages everything */
    if (argc == 2 && strcmp(argv[1], ".") == 0) {
        res = git_add_all(&repo, fl);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(fl);
            return 1;
        }
        printf("Staged all changes (%d files)\n", res.files_affected);
    } else {
        /* Stage individual files */
        for (i = 1; i < argc; i++) {
            res = git_add(&repo, argv[i], fl);
            if (res.code != GIT_OK) {
                fprintf(stderr, "error: %s: %s\n", argv[i], res.message);
                /* Continue with remaining files */
            }
        }
    }

    /* Save updated state */
    res = git_save_state(&repo, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "warning: could not save state: %s\n", res.message);
    }

    free(fl);
    return 0;
}

/* --- Subcommand: commit --- */

static int cmd_commit(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    char *message;
    char author_name[GIT_MAX_AUTHOR];
    char author_email[GIT_MAX_AUTHOR];
    char *user;
    int i;

    /* Parse -m "message" */
    message = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                message = argv[i + 1];
                break;
            } else {
                fprintf(stderr, "error: switch 'm' requires a value\n");
                return 1;
            }
        }
    }

    if (!message) {
        fprintf(stderr, "usage: %s commit -m \"message\"\n", PROG_NAME);
        return 1;
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    /* Get author info from environment or defaults */
    user = getenv("GIT_AUTHOR_NAME");
    if (user) {
        strncpy(author_name, user, sizeof(author_name) - 1);
        author_name[sizeof(author_name) - 1] = '\0';
    } else {
        user = getenv("USER");
        if (user) {
            strncpy(author_name, user, sizeof(author_name) - 1);
            author_name[sizeof(author_name) - 1] = '\0';
        } else {
            strcpy(author_name, "NeXTSTEP User");
        }
    }

    user = getenv("GIT_AUTHOR_EMAIL");
    if (user) {
        strncpy(author_email, user, sizeof(author_email) - 1);
        author_email[sizeof(author_email) - 1] = '\0';
    } else {
        sprintf(author_email, "%s@nextstep.local", author_name);
    }

    res = git_commit(&repo, message, author_name, author_email, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    /* Save updated state */
    res = git_save_state(&repo, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "warning: could not save state: %s\n", res.message);
    }

    printf("[%s] %s\n", repo.branch, message);
    printf(" %d file(s) changed\n", res.files_affected);
    free(fl);
    return 0;
}

/* --- Subcommand: push --- */

static int cmd_push(token)
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    printf("Pushing to %s/%s (%s)...\n", repo.owner, repo.repo, repo.branch);

    res = git_push(&repo, fl, cli_progress, NULL);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    /* Save updated state after push */
    res = git_save_state(&repo, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "warning: could not save state: %s\n", res.message);
    }

    printf("\nPush complete.\n");
    free(fl);
    return 0;
}

/* --- Subcommand: pull --- */

static int cmd_pull(token)
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    printf("Pulling from %s/%s (%s)...\n",
           repo.owner, repo.repo, repo.branch);

    res = git_pull(&repo, cli_progress, NULL);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("\nPull complete. %d files updated.\n", res.files_affected);
    free(fl);
    return 0;
}

/* --- Subcommand: log --- */

static int cmd_log(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    GitCommitLog *log;
    int max_count;
    int i;

    /* Default: show 10 commits */
    max_count = 10;

    /* Parse -n count */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 < argc) {
                max_count = atoi(argv[i + 1]);
                if (max_count <= 0) max_count = 10;
                if (max_count > GIT_MAX_COMMITS) max_count = GIT_MAX_COMMITS;
                break;
            }
        }
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    log = (GitCommitLog *)malloc(sizeof(GitCommitLog));
    if (!fl || !log) {
        if (fl) free(fl);
        if (log) free(log);
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));
    memset(log, 0, sizeof(GitCommitLog));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl); free(log);
        return 1;
    }

    res = git_log(&repo, max_count, log);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl); free(log);
        return 1;
    }

    if (log->count == 0) {
        printf("No commits yet.\n");
        free(fl); free(log);
        return 0;
    }

    for (i = 0; i < log->count; i++) {
        printf("commit %s\n", log->commits[i].sha);
        printf("Author: %s\n", log->commits[i].author);
        printf("Date:   %s\n", log->commits[i].date);
        printf("\n");
        printf("    %s\n", log->commits[i].message);
        printf("\n");
    }

    free(fl);
    free(log);
    return 0;
}

/* --- Subcommand: diff --- */

static int cmd_diff(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    GitDiffResult diff;
    char *filepath;
    int i;

    /* Check for branch compare syntax: diff base..head */
    if (argc >= 2 && strstr(argv[1], "..") != NULL) {
        GitCompareResult cmp;
        char base[GIT_MAX_BRANCH];
        char head[GIT_MAX_BRANCH];
        char *dots;
        int base_len;

        dots = strstr(argv[1], "..");
        base_len = dots - argv[1];
        if (base_len <= 0 || base_len >= GIT_MAX_BRANCH) {
            fprintf(stderr, "error: invalid base branch\n");
            return 1;
        }
        strncpy(base, argv[1], base_len);
        base[base_len] = '\0';
        strncpy(head, dots + 2, GIT_MAX_BRANCH - 1);
        head[GIT_MAX_BRANCH - 1] = '\0';

        if (head[0] == '\0') {
            fprintf(stderr,
                "usage: %s diff <base>..<head>\n", PROG_NAME);
            return 1;
        }

        fl = (GitFileList *)malloc(sizeof(GitFileList));
        if (!fl) {
            fprintf(stderr, "fatal: out of memory\n");
            return 1;
        }
        memset(&repo, 0, sizeof(repo));
        memset(fl, 0, sizeof(GitFileList));

        if (find_repo(&repo, fl, token) < 0) {
            free(fl);
            return 1;
        }
        free(fl);

        printf("Comparing %s...%s\n\n", base, head);

        memset(&cmp, 0, sizeof(cmp));
        res = git_compare(&repo, base, head, &cmp);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            return 1;
        }

        /* Summary */
        printf("%s\n", res.message);
        if (cmp.ahead_by > 0)
            printf("  %s is %d commit(s) ahead of %s\n",
                   head, cmp.ahead_by, base);
        if (cmp.behind_by > 0)
            printf("  %s is %d commit(s) behind %s\n",
                   head, cmp.behind_by, base);
        printf("\n");

        /* File list */
        for (i = 0; i < cmp.file_count; i++) {
            printf("%-10s %s", cmp.files[i].status,
                   cmp.files[i].filename);
            if (cmp.files[i].additions > 0 ||
                cmp.files[i].deletions > 0) {
                printf("  (+%d/-%d)",
                       cmp.files[i].additions,
                       cmp.files[i].deletions);
            }
            printf("\n");

            /* Show patch if available */
            if (cmp.files[i].patch) {
                printf("%s\n\n", cmp.files[i].patch);
            }
        }

        git_compare_free(&cmp);
        return 0;
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));
    memset(&diff, 0, sizeof(diff));

    /* Optional: specific file */
    filepath = NULL;
    if (argc >= 2) {
        filepath = argv[1];
    }

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    /* Refresh status so we know what changed */
    res = git_status(&repo, fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    res = git_diff(&repo, filepath, fl, &diff, cli_progress, NULL);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    if (diff.count == 0) {
        printf("No differences found.\n");
        git_diff_free(&diff);
        free(fl);
        return 0;
    }

    for (i = 0; i < diff.count; i++) {
        char *status_str;
        char *old_p, *new_p;
        char *old_line, *new_line;

        /* Header */
        switch (diff.entries[i].status) {
        case GIT_STATUS_MODIFIED:
            status_str = "modified";
            break;
        case GIT_STATUS_NEW:
            status_str = "new file";
            break;
        case GIT_STATUS_DELETED:
            status_str = "deleted";
            break;
        default:
            status_str = "changed";
            break;
        }

        printf("diff -- %s (%s)\n", diff.entries[i].path, status_str);
        printf("--- a/%s\n", diff.entries[i].path);
        printf("+++ b/%s\n", diff.entries[i].path);

        if (diff.entries[i].status == GIT_STATUS_NEW) {
            /* Show all lines as added */
            new_p = diff.entries[i].new_content;
            if (new_p) {
                while (*new_p) {
                    printf("+");
                    while (*new_p && *new_p != '\n') {
                        putchar(*new_p);
                        new_p++;
                    }
                    putchar('\n');
                    if (*new_p == '\n') new_p++;
                }
            }
        } else if (diff.entries[i].status == GIT_STATUS_DELETED) {
            /* Show all lines as removed */
            old_p = diff.entries[i].old_content;
            if (old_p) {
                while (*old_p) {
                    printf("-");
                    while (*old_p && *old_p != '\n') {
                        putchar(*old_p);
                        old_p++;
                    }
                    putchar('\n');
                    if (*old_p == '\n') old_p++;
                }
            }
        } else {
            /* Modified: show simple line-by-line diff.
               This is a basic comparison -- not a real unified diff
               algorithm, but useful enough on a m68k. We show old
               lines prefixed with '-' and new lines with '+'. */
            old_p = diff.entries[i].old_content;
            new_p = diff.entries[i].new_content;

            if (old_p && new_p) {
                /* Walk both line by line */
                while (*old_p || *new_p) {
                    char old_line_buf[512];
                    char new_line_buf[512];
                    int olen, nlen;

                    /* Extract old line */
                    olen = 0;
                    if (*old_p) {
                        while (*old_p && *old_p != '\n' &&
                               olen < (int)sizeof(old_line_buf) - 1) {
                            old_line_buf[olen++] = *old_p++;
                        }
                        if (*old_p == '\n') old_p++;
                    }
                    old_line_buf[olen] = '\0';

                    /* Extract new line */
                    nlen = 0;
                    if (*new_p) {
                        while (*new_p && *new_p != '\n' &&
                               nlen < (int)sizeof(new_line_buf) - 1) {
                            new_line_buf[nlen++] = *new_p++;
                        }
                        if (*new_p == '\n') new_p++;
                    }
                    new_line_buf[nlen] = '\0';

                    if (olen == 0 && nlen == 0 && !*old_p && !*new_p)
                        break;

                    if (strcmp(old_line_buf, new_line_buf) == 0) {
                        /* Lines match */
                        printf(" %s\n", old_line_buf);
                    } else {
                        if (olen > 0 || *old_p)
                            printf("-%s\n", old_line_buf);
                        if (nlen > 0 || *new_p)
                            printf("+%s\n", new_line_buf);
                    }
                }
            } else if (old_p) {
                while (*old_p) {
                    printf("-");
                    while (*old_p && *old_p != '\n') {
                        putchar(*old_p);
                        old_p++;
                    }
                    putchar('\n');
                    if (*old_p == '\n') old_p++;
                }
            } else if (new_p) {
                while (*new_p) {
                    printf("+");
                    while (*new_p && *new_p != '\n') {
                        putchar(*new_p);
                        new_p++;
                    }
                    putchar('\n');
                    if (*new_p == '\n') new_p++;
                }
            }
        }

        printf("\n");
    }

    git_diff_free(&diff);
    free(fl);
    return 0;
}

/* --- Subcommand: branch --- */

static int cmd_branch(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    int i;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        /* Delete branch: git branch -d <name> */
        if (argc < 3) {
            fprintf(stderr, "usage: %s branch -d <branch-name>\n", PROG_NAME);
            free(fl);
            return 1;
        }
        res = git_branch_delete(&repo, argv[2]);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(fl);
            return 1;
        }
        printf("%s\n", res.message);
    } else if (argc >= 2) {
        /* Create branch: git branch <name> */
        res = git_branch_create(&repo, argv[1], NULL);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(fl);
            return 1;
        }
        printf("%s\n", res.message);
    } else {
        /* List branches: git branch */
        GitBranchList *bl;

        bl = (GitBranchList *)malloc(sizeof(GitBranchList));
        if (!bl) {
            fprintf(stderr, "fatal: out of memory\n");
            free(fl);
            return 1;
        }
        memset(bl, 0, sizeof(GitBranchList));

        res = git_branch_list(&repo, bl);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(bl);
            free(fl);
            return 1;
        }

        if (bl->count == 0) {
            printf("No branches found.\n");
        } else {
            for (i = 0; i < bl->count; i++) {
                if (bl->branches[i].is_current)
                    printf("* %s\n", bl->branches[i].name);
                else
                    printf("  %s\n", bl->branches[i].name);
            }
        }

        free(bl);
    }

    free(fl);
    return 0;
}

/* --- Subcommand: checkout --- */

static int cmd_checkout(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;

    if (argc < 2) {
        fprintf(stderr, "usage: %s checkout <branch>\n", PROG_NAME);
        return 1;
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    printf("Switching to branch '%s'...\n", argv[1]);

    res = git_branch_switch(&repo, argv[1], fl, cli_progress, NULL);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("\n%s\n", res.message);
    free(fl);
    return 0;
}

/* --- Subcommand: merge --- */

static int cmd_merge(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitMergeResult mres;
    GitResult res;
    char *branch_name;
    char *message;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s merge <branch> [-m \"message\"]\n",
                PROG_NAME);
        return 1;
    }

    branch_name = argv[1];

    /* Parse optional -m "message" */
    message = NULL;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                message = argv[i + 1];
                break;
            } else {
                fprintf(stderr, "error: switch 'm' requires a value\n");
                return 1;
            }
        }
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    printf("Merging '%s' into '%s'...\n", branch_name, repo.branch);

    mres = git_merge(&repo, branch_name, message);

    if (mres.code == GIT_ERR_CONFLICT) {
        fprintf(stderr, "error: %s\n", mres.message);
        fprintf(stderr,
            "Resolve conflicts on GitHub, then run '%s pull'.\n",
            PROG_NAME);
        free(fl);
        return 1;
    }

    if (mres.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", mres.message);
        free(fl);
        return 1;
    }

    printf("%s\n", mres.message);

    /* If merge created a new commit, pull to sync local files */
    if (mres.sha[0]) {
        printf("Pulling merged changes...\n");
        res = git_pull(&repo, cli_progress, NULL);
        if (res.code != GIT_OK) {
            fprintf(stderr, "warning: pull after merge failed: %s\n",
                    res.message);
            fprintf(stderr, "Run '%s pull' manually to sync.\n",
                    PROG_NAME);
        } else {
            printf("Pull complete. %d files updated.\n",
                   res.files_affected);
        }
    }

    free(fl);
    return 0;
}

/* --- Subcommand: tag --- */

static int cmd_tag(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    int i;
    char *tag_name;
    char *message;
    int annotated;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    if (argc < 2) {
        /* List tags: git tag */
        GitTagList *tl;

        tl = (GitTagList *)malloc(sizeof(GitTagList));
        if (!tl) {
            fprintf(stderr, "fatal: out of memory\n");
            free(fl);
            return 1;
        }
        memset(tl, 0, sizeof(GitTagList));

        res = git_tag_list(&repo, tl);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(tl);
            free(fl);
            return 1;
        }

        if (tl->count == 0) {
            printf("No tags found.\n");
        } else {
            for (i = 0; i < tl->count; i++) {
                printf("  %s\n", tl->tags[i].name);
            }
        }

        free(tl);
        free(fl);
        return 0;
    }

    /* Parse: git tag [-a] <name> [-m "message"] */
    tag_name = NULL;
    message = NULL;
    annotated = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            annotated = 1;
        } else if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                message = argv[i + 1];
                i++;
            } else {
                fprintf(stderr,
                    "error: switch 'm' requires a value\n");
                free(fl);
                return 1;
            }
        } else if (!tag_name) {
            tag_name = argv[i];
        }
    }

    if (!tag_name) {
        fprintf(stderr,
            "usage: %s tag <name>\n"
            "   or: %s tag -a <name> -m \"message\"\n",
            PROG_NAME, PROG_NAME);
        free(fl);
        return 1;
    }

    /* If -a flag used, require message */
    if (annotated && !message) {
        fprintf(stderr,
            "error: annotated tag requires -m \"message\"\n");
        free(fl);
        return 1;
    }

    res = git_tag_create(&repo, tag_name, message);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("%s\n", res.message);
    free(fl);
    return 0;
}

/* --- Subcommand: release --- */

static int cmd_release(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;
    int i;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        /* List releases */
        GitReleaseList *rl;

        rl = (GitReleaseList *)malloc(sizeof(GitReleaseList));
        if (!rl) {
            fprintf(stderr, "fatal: out of memory\n");
            free(fl);
            return 1;
        }
        memset(rl, 0, sizeof(GitReleaseList));

        res = git_release_list(&repo, rl);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(rl);
            free(fl);
            return 1;
        }

        if (rl->count == 0) {
            printf("No releases found.\n");
        } else {
            for (i = 0; i < rl->count; i++) {
                printf("  %s", rl->releases[i].tag_name);
                if (rl->releases[i].name[0])
                    printf("  \"%s\"", rl->releases[i].name);
                if (rl->releases[i].draft)
                    printf("  [draft]");
                if (rl->releases[i].prerelease)
                    printf("  [prerelease]");
                printf("\n");
            }
        }

        free(rl);
    } else if (strcmp(argv[1], "create") == 0) {
        /* Create release: git release create <tag> [-t title] [-m body] */
        char *tag;
        char *title;
        char *body;

        if (argc < 3) {
            fprintf(stderr,
                "usage: %s release create <tag> "
                "[-t \"title\"] [-m \"body\"]\n", PROG_NAME);
            free(fl);
            return 1;
        }

        tag = argv[2];
        title = NULL;
        body = NULL;

        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
                title = argv[++i];
            } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
                body = argv[++i];
            }
        }

        res = git_release_create(&repo, tag, title, body);
        if (res.code != GIT_OK) {
            fprintf(stderr, "error: %s\n", res.message);
            free(fl);
            return 1;
        }

        printf("%s\n", res.message);
    } else {
        fprintf(stderr,
            "usage: %s release list\n"
            "   or: %s release create <tag> "
            "[-t \"title\"] [-m \"body\"]\n",
            PROG_NAME, PROG_NAME);
        free(fl);
        return 1;
    }

    free(fl);
    return 0;
}

/* --- Subcommand: fork --- */

static int cmd_fork(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    printf("Forking %s/%s...\n", repo.owner, repo.repo);

    res = git_fork(&repo);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("%s\n", res.message);
    free(fl);
    return 0;
}

/* --- Subcommand: rm --- */

static int cmd_rm(argc, argv, token)
int argc;
char **argv;
char *token;
{
    GitRepo repo;
    GitFileList *fl;
    GitResult res;

    if (argc < 2) {
        fprintf(stderr, "usage: %s rm <file>\n", PROG_NAME);
        return 1;
    }

    fl = (GitFileList *)malloc(sizeof(GitFileList));
    if (!fl) {
        fprintf(stderr, "fatal: out of memory\n");
        return 1;
    }

    memset(&repo, 0, sizeof(repo));
    memset(fl, 0, sizeof(GitFileList));

    if (find_repo(&repo, fl, token) < 0) {
        free(fl);
        return 1;
    }

    res = git_rm(&repo, argv[1], fl);
    if (res.code != GIT_OK) {
        fprintf(stderr, "error: %s\n", res.message);
        free(fl);
        return 1;
    }

    printf("%s\n", res.message);
    free(fl);
    return 0;
}

/* --- Help --- */

static void cmd_help()
{
    printf("\n");
    printf("  NeXTSTEP Git Client\n");
    printf("  Usage: %s <command> [options]\n\n", PROG_NAME);
    printf("  Commands:\n");
    printf("    clone owner/repo [branch] [dir]   Clone a repository\n");
    printf("    status                             Show file status\n");
    printf("    add <file> [file2 ...]             Stage files\n");
    printf("    add .                              Stage all changes\n");
    printf("    commit -m \"message\"                Commit staged changes\n");
    printf("    push                               Push to GitHub\n");
    printf("    pull                               Pull from GitHub\n");
    printf("    log [-n count]                     Show commit history\n");
    printf("    diff [file]                        Show local differences\n");
    printf("    diff <base>..<head>                Compare branches\n");
    printf("    branch                             List branches\n");
    printf("    branch <name>                      Create branch\n");
    printf("    branch -d <name>                   Delete branch\n");
    printf("    checkout <branch>                  Switch branch\n");
    printf("    merge <branch> [-m \"msg\"]           Merge branch\n");
    printf("    tag                                List tags\n");
    printf("    tag <name>                         Create lightweight tag\n");
    printf("    tag -a <name> -m \"msg\"              Create annotated tag\n");
    printf("    release list                       List releases\n");
    printf("    release create <tag> [-t -m]       Create release\n");
    printf("    fork                               Fork repository\n");
    printf("    rm <file>                          Delete file\n");
    printf("    help                               Show this help\n");
    printf("\n");
    printf("  Token: place your GitHub token in .github_token\n");
    printf("  (checked in cwd, $HOME, and /me/)\n");
    printf("\n");
}

/* --- Main --- */

int main(argc, argv)
int argc;
char **argv;
{
    char token[GIT_MAX_TOKEN];
    char *subcmd;
    int sub_argc;
    char **sub_argv;

    if (argc < 2) {
        cmd_help();
        return 1;
    }

    subcmd = argv[1];

    /* Help */
    if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0 ||
        strcmp(subcmd, "-h") == 0) {
        cmd_help();
        return 0;
    }

    /* Load token */
    if (load_token(token, sizeof(token)) != 0) {
        fprintf(stderr,
            "fatal: GitHub token not found.\n"
            "Create a file called '%s' containing your\n"
            "GitHub personal access token.\n"
            "Searched: ./%s, $HOME/%s, /me/%s\n",
            TOKEN_FILE, TOKEN_FILE, TOKEN_FILE, TOKEN_FILE);
        return 1;
    }

    /* Set up sub-command args (skip program name and subcommand) */
    sub_argc = argc - 1;
    sub_argv = argv + 1;

    /* Dispatch */
    if (strcmp(subcmd, "clone") == 0) {
        return cmd_clone(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "status") == 0) {
        return cmd_status(token);
    }
    else if (strcmp(subcmd, "add") == 0) {
        return cmd_add(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "commit") == 0) {
        return cmd_commit(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "push") == 0) {
        return cmd_push(token);
    }
    else if (strcmp(subcmd, "pull") == 0) {
        return cmd_pull(token);
    }
    else if (strcmp(subcmd, "log") == 0) {
        return cmd_log(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "diff") == 0) {
        return cmd_diff(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "branch") == 0) {
        return cmd_branch(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "checkout") == 0) {
        return cmd_checkout(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "merge") == 0) {
        return cmd_merge(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "tag") == 0) {
        return cmd_tag(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "release") == 0) {
        return cmd_release(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "fork") == 0) {
        return cmd_fork(sub_argc, sub_argv, token);
    }
    else if (strcmp(subcmd, "rm") == 0) {
        return cmd_rm(sub_argc, sub_argv, token);
    }
    else {
        fprintf(stderr, "'%s' is not a git command. See '%s help'.\n",
                subcmd, PROG_NAME);
        return 1;
    }
}
