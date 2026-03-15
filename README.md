# nextstep-git

A native Git client for **NeXTSTEP 3.3** on Motorola 68040 hardware.

Connects directly to the GitHub REST API over TLS 1.2, letting a vintage NeXTstation clone, modify, commit, and push to GitHub repositories -- no modern proxy or middleware required.

## Features

### Core Git Operations
- **clone** -- clone a GitHub repository (auto-detects default branch)
- **status** -- show tracked, modified, staged, and new files
- **add** -- stage files for commit (individual files or `git add .`)
- **commit** -- create a local commit (pending push)
- **push** -- upload commits to GitHub via the Git Data API
- **pull** -- download remote changes
- **log** -- show commit history
- **diff** -- show local file differences

### Branch Operations
- **branch** -- list, create, and delete remote branches
- **checkout** -- switch between branches (downloads changed files)
- **merge** -- server-side merge via GitHub API (handles conflicts)
- **diff base..head** -- compare branches with unified patches via Compare API

### Tags and Releases
- **tag** -- list, create lightweight, and create annotated tags
- **release list** -- list GitHub releases
- **release create** -- create a release from an existing tag

### Repository Operations
- **fork** -- fork a repository to your account
- **rm** -- delete a file from the repository (server-side)

### Additional Features
- **.gitignore support** -- filters ignored files from status and add
- **Large file support** -- files up to ~1.3 MB via Git Blobs API
- **Path traversal protection** -- rejects unsafe paths in clone/pull
- **Buffer overflow prevention** -- bounds-checked string formatting throughout

## Command Reference

```
git clone owner/repo [branch] [dir]     Clone a repository
git status                              Show file status
git add <file> [file2 ...]              Stage files
git add .                               Stage all changes
git commit -m "message"                 Commit staged changes
git push                                Push to GitHub
git pull                                Pull from GitHub
git log [-n count]                      Show commit history
git diff [file]                         Show local differences
git diff base..head                     Compare branches
git branch                              List branches
git branch <name>                       Create branch
git branch -d <name>                    Delete branch
git checkout <branch>                   Switch branch
git merge <branch> [-m "msg"]           Merge branch (server-side)
git tag                                 List tags
git tag <name>                          Create lightweight tag
git tag -a <name> -m "msg"              Create annotated tag
git release list                        List releases
git release create <tag> [-t -m]        Create release
git fork                                Fork repository
git rm <file>                           Delete file
git help                                Show help
```

## Requirements

- NeXTSTEP 3.3 (m68k)
- A GitHub personal access token (classic, with `repo` scope)
- Network connectivity from the NeXT to the internet

## Quick Start

1. Place your GitHub token in a file called `.github_token`:
   ```
   echo "ghp_your_token_here" > ~/.github_token
   chmod 600 ~/.github_token
   ```
2. Clone a repository:
   ```
   ./git-nextstep clone owner/repo
   ```

## Building from Source

Building requires the [Crypto Ancienne](https://github.com/classilla/cryanc) TLS library by Cameron Kaiser.

On a modern machine, fetch the library:

```sh
./fetch_cryanc.sh
```

Transfer `cryanc.c`, `cryanc.h`, `git_core.c`, `git_core.h`, and `git_cli.c` to the NeXT, then compile:

```sh
cc -O -o git-nextstep git_cli.c git_core.c
```

This takes approximately 10 minutes on a 25 MHz 68040.

## How It Works

The client speaks directly to the GitHub REST API (`api.github.com`) over HTTPS. TLS 1.2 is provided by Crypto Ancienne, a TLS library designed for vintage and embedded systems.

All git operations are implemented through GitHub's API:
- **Clone/Pull**: Trees API to list files, Contents API (or Blobs API for large files) to download
- **Push**: Blobs API to upload file content, Trees API to build a tree, Commits API to create a commit, Refs API to update the branch
- **Branch/Merge**: Refs API for branch management, Merges API for server-side merge
- **Tags/Releases**: Git Tags API for annotated tags, Refs API for lightweight tags, Releases API for GitHub releases
- **Compare**: Compare API for cross-branch diffs with unified patches
- **Fork**: Forks API (single POST, async creation)
- **Delete**: Contents API DELETE for server-side file removal

Local state is tracked in a `.nextstep_git` file (not a real `.git` directory). File change detection uses CRC32 hashing.

## Architecture

The codebase is split into two layers:

- **`git_core.c` / `git_core.h`** -- Pure logic layer. No printf, no stdin. Returns data via structs and reports progress via callbacks. Designed to be reusable by a GUI frontend.
- **`git_cli.c`** -- Command-line interface. Parses arguments, calls the core layer, prints output.

## Limitations

- File size limited to ~1.3 MB for clone/pull (2 MB API response buffer)
- Maximum 500 tracked files per repository
- Maximum 50 branches, 50 tags, 20 releases per listing
- Uses CRC32 for local change detection, not SHA-1
- Merge conflicts must be resolved on GitHub (no local merge)
- TLS certificate validation is not implemented (Crypto Ancienne limitation)

## Acknowledgments

- [Crypto Ancienne](https://github.com/classilla/cryanc) by Cameron Kaiser ([@classilla](https://github.com/classilla)) -- TLS 1.2 library for vintage/embedded systems. This project would not be possible without it.
- Built with assistance from [Claude](https://claude.ai) by Anthropic.

## License

MIT License. See source file headers for details.
