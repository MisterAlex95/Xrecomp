#!/bin/sh
# The toolkit folder of a desktop download (.github/workflows/desktop.yml, docs/releases/README.md): the tree of a
# commit as git has it (git archive: no build output, no .git, nothing ignored), plus tools/VERSION, the commit,
# which prepare and the Mac app read when there is no git checkout, and the frame generation source tarballs. No game,
# no game data, no generated C.
#
#   sh tools/ci/package_toolkit.sh DEST [COMMIT]
#
#   DEST     the folder to create (must not exist); the macOS job puts XRecomp.app inside it
#   COMMIT   default HEAD
set -eu

[ $# -ge 1 ] || { echo "usage: $0 DEST [COMMIT]" >&2; exit 2; }
dest=$1
commit=$(git rev-parse --verify "${2:-HEAD}^{commit}")
[ ! -e "$dest" ] || { echo "$dest already exists" >&2; exit 1; }

mkdir -p "$dest"
git archive --format=tar "$commit" | tar -C "$dest" -xf -
printf '%s\n' "$commit" > "$dest/tools/VERSION"
# The pinned frame generation sources (XBA-187: framegen + volk, MIT; DXVK + SPIRV-Headers, zlib / MIT), as the
# tarballs src/framegen/lsfg_sources.cmake looks for in third_party/lsfg-sources: a game builds with frame
# generation without fetching anything. Their licences are in NOTICE.
sh "$dest/tools/lsfg/fetch_sources.sh" "$dest/third_party/lsfg-sources"

# What prepare needs to find (the Mac app's Toolkit.isValid, tools.prepare itself).
for f in tools/prepare/__main__.py templates/new-game/CMakeLists.txt; do
    [ -e "$dest/$f" ] || { echo "the package has no $f" >&2; exit 1; }
done
echo "$dest: $(find "$dest" -type f | wc -l | tr -d ' ') files, commit $commit"
