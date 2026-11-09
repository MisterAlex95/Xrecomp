#!/usr/bin/env bash
# Merges new upstream commits (sp00nznet/xboxrecomp) into our android branch.
#
#   tools/sync_upstream.sh [--push]
#
# 1. fetches upstream, fast-forwards our main to upstream/main (main is a pure mirror: never commit on it);
# 2. merges main into android with a merge commit;
# 3. on the first conflict, aborts the merge, lists the conflicting files and exits 1;
# 4. with --push, pushes main and android to origin (the Gitea fork).
# The merge runs in the worktree that has android checked out (it must be clean), or in a temporary worktree.
set -euo pipefail

PUSH=0
[ "${1:-}" = "--push" ] && PUSH=1

cd "$(git rev-parse --show-toplevel)"
git remote get-url upstream >/dev/null 2>&1 \
    || git remote add upstream https://github.com/sp00nznet/xboxrecomp

git fetch -q upstream main
git fetch -q origin 2>/dev/null || true

# main mirrors upstream; a worktree may hold it, so update the ref without checking it out.
if git merge-base --is-ancestor main upstream/main; then
    git update-ref refs/heads/main upstream/main
else
    echo "main has commits that are not upstream's: main must stay a mirror. Fix it by hand." >&2
    exit 2
fi

new=$(git rev-list --count android..main)
if [ "$new" -eq 0 ]; then
    echo "android is up to date with upstream ($(git rev-parse --short main))."
else
    echo "Merging $new upstream commit(s) into android ..."
    wt=$(git worktree list --porcelain | awk '/^worktree /{w=$2} /^branch refs\/heads\/android$/{print w}')
    tmp=""
    if [ -z "$wt" ]; then
        tmp=$(mktemp -d); wt="$tmp/android"
        git worktree add -q "$wt" android
        trap 'git worktree remove --force "$wt" 2>/dev/null; rm -rf "$tmp"' EXIT
    elif [ -n "$(git -C "$wt" status --porcelain --untracked-files=no)" ]; then
        echo "The android worktree ($wt) has uncommitted changes: commit or set them aside first." >&2
        exit 2
    fi
    if ! git -C "$wt" merge --no-ff -q -m "Merge upstream $(git describe --tags --always main) into android" main; then
        echo "Conflict merging upstream into android. Files:" >&2
        git -C "$wt" diff --name-only --diff-filter=U | sed 's/^/  /' >&2
        git -C "$wt" merge --abort
        exit 1
    fi
    echo "Merged: $(git log -1 --format='%h %s' android)"
fi

if [ "$PUSH" -eq 1 ]; then
    git push -q origin main android
    echo "Pushed main and android to origin."
fi
