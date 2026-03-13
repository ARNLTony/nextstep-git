# nextstep-git

A native Git client for **NeXTSTEP 3.3** on Motorola 68040 hardware.

Connects directly to the GitHub REST API over TLS 1.2, letting a vintage NeXTstation clone, modify, commit, and push to GitHub repositories -- no modern proxy or middleware required.

## Features

- **clone** -- clone a GitHub repository (auto-detects default branch)
- **status** -- show tracked, modified, staged, and new files
- **add** -- stage files for commit (individual files or `git add .`)
- **commit** -- create a local commit (pending push)
- **push** -- upload commits to GitHub via the Git Data API
- **pull** -- download remote changes
- **log** -- show commit history
- **diff** -- show file differences

Supports files of any size up to ~1.3 MB (uses the Git Blobs API for files too large for the Contents API).

## Requirements

- NeXTSTEP 3.3 (m68k)
- A GitHub personal access token (classic, with `repo` scope)
- Network connectivity from the NeXT to the internet

## Quick start

A prebuilt m68k binary (`git-nextstep`) is included in the repository.

1. Download or clone this repo on a modern machine
2. Transfer `git-nextstep` to your NeXTstation (via FTP, NFS, or floppy)
3. Set your GitHub token:
   ```
   setenv GITHUB_TOKEN ghp_your_token_here
   ```
4. Clone a repository:
   ```
   ./git-nextstep clone owner/repo
   ```

Building from source produces a binary called `git` (see below).

## Building from source

Building requires the [Crypto Ancienne](https://github.com/classilla/cryanc) TLS library by Cameron Kaiser.

On a modern machine, fetch the library:

```sh
./fetch_cryanc.sh
```

Transfer `cryanc.c`, `cryanc.h`, `git_core.c`, `git_core.h`, and `git_cli.c` to the NeXT, then compile:

```sh
cc -O -o git git_cli.c git_core.c -lNeXT_s
```

This takes approximately 10 minutes on a 25 MHz 68040.

## How it works

The client speaks directly to the GitHub REST API (`api.github.com`) over HTTPS. TLS 1.2 is provided by Crypto Ancienne, a TLS library designed for vintage and embedded systems.

All git operations are implemented through GitHub's API:
- **Clone/Pull**: Trees API to list files, Contents API (or Blobs API for large files) to download
- **Push**: Blobs API to upload file content, Trees API to build a tree, Commits API to create a commit, Refs API to update the branch

Local state is tracked in a `.nextstep_git` file (not a real `.git` directory). File change detection uses CRC32 hashing.

## Architecture

The codebase is split into two layers:

- **`git_core.c` / `git_core.h`** -- Pure logic layer. No printf, no stdin. Returns data via structs and reports progress via callbacks. Designed to be reusable by a GUI frontend.
- **`git_cli.c`** -- Command-line interface. Parses arguments, calls the core layer, prints output.

## Limitations

- No merge support (fast-forward pull only)
- No branching operations (works on a single branch)
- No `.gitignore` pattern matching
- File size limited to ~1.3 MB for clone/pull (2 MB API response buffer)
- Maximum 500 tracked files
- Uses CRC32 for change detection, not SHA-1

## Acknowledgments

- [Crypto Ancienne](https://github.com/classilla/cryanc) by Cameron Kaiser ([@classilla](https://github.com/classilla)) -- TLS 1.2 library for vintage/embedded systems. This project would not be possible without it.
- Built with assistance from [Claude](https://claude.ai) by Anthropic.

## License

MIT License. See source file headers for details.
