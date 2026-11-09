#!/usr/bin/env bash
# Merges new upstream commits (sp00nznet/xboxrecomp) into our main branch.
#
#   tools/sync_upstream.sh [--push]
#
# 1. fetches upstream/main;
# 2. merges it into main with a merge commit;
# 3. on the first conflict, aborts the merge, lists the conflicting files and exits 1;
# 4. with --push, pushes main to origin (the Gitea fork).
# The merge runs in the worktree that has main checked out (it must be clean), or in a temporary worktree.
set -euo pipefail

PUSH=0
[ "${1:-}" = "--push" ] && PUSH=1

cd "$(git rev-parse --show-toplevel)"
git remote get-url upstream >/dev/null 2>&1 \
    || git remote add upstream https://github.com/sp00nznet/xboxrecomp

git fetch -q upstream main
git fetch -q origin 2>/dev/null || true

new=$(git rev-list --count main..upstream/main)
if [ "$new" -eq 0 ]; then
    echo "main is up to date with upstream ($(git rev-parse --short upstream/main))."
else
    echo "Merging $new upstream commit(s) into main ..."
    wt=$(git worktree list --porcelain | awk '/^worktree /{w=$2} /^branch refs\/heads\/main$/{print w}')
    tmp=""
    if [ -z "$wt" ]; then
        tmp=$(mktemp -d); wt="$tmp/main"
        git worktree add -q "$wt" main
        trap 'git worktree remove --force "$wt" 2>/dev/null; rm -rf "$tmp"' EXIT
    elif [ -n "$(git -C "$wt" status --porcelain --untracked-files=no)" ]; then
        echo "The main worktree ($wt) has uncommitted changes: commit or set them aside first." >&2
        exit 2
    fi
    if ! git -C "$wt" merge --no-ff -q -m "Merge upstream $(git describe --tags --always upstream/main) into main" upstream/main; then
        echo "Conflict merging upstream into main. Files:" >&2
        git -C "$wt" diff --name-only --diff-filter=U | sed 's/^/  /' >&2
        git -C "$wt" merge --abort
        exit 1
    fi
    echo "Merged: $(git log -1 --format='%h %s' main)"
fi

if [ "$PUSH" -eq 1 ]; then
    git push -q origin main
    echo "Pushed main to origin."
fi
