#!/bin/sh
# Publish a release on the public GitHub mirror (docs/releases/README.md). The Gitea push mirror copies commits and
# tags to GitHub, never releases, so CI creates them there itself.
#
#   GH_TOKEN=... sh tools/ci/github_release.sh TAG SHA NAME PRERELEASE BODY_FILE ASSET[=NAME]...
#
#   GH_TOKEN    a token with Contents: read and write on the GitHub repository (Gitea secret GH_RELEASE_TOKEN)
#   GH_REPO     owner/name on GitHub (default MisterAlex95/Xrecomp; Gitea variable GH_RELEASE_REPO)
#   PRERELEASE  true or false
#   ASSET       a local file, uploaded as its basename or as NAME
#
# An existing release with the same tag is deleted first (the rolling xrecomp-latest is replaced on each main build).
# The tag itself is created at SHA when GitHub does not have it yet; the mirror then pushes the same commit.
# Needs curl and jq (POSIX sh: the CI container is Alpine).
set -eu

[ $# -ge 5 ] || { echo "usage: $0 TAG SHA NAME PRERELEASE BODY_FILE [ASSET[=NAME]...]" >&2; exit 2; }
: "${GH_TOKEN:?GH_TOKEN is not set}"
repo=${GH_REPO:-MisterAlex95/Xrecomp}
tag=$1 sha=$2 name=$3 pre=$4 body_file=$5
shift 5
case "$pre" in true|false) ;; *) echo "PRERELEASE: true or false, not $pre" >&2; exit 2;; esac

api="https://api.github.com/repos/$repo"
gh() {
    curl -fsS -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" "$@"
}

old=$(gh "$api/releases/tags/$tag" 2>/dev/null | jq -r '.id // empty' || true)
if [ -n "$old" ]; then
    gh -X DELETE "$api/releases/$old"
fi

req=$(jq -n --arg tag "$tag" --arg sha "$sha" --arg name "$name" --argjson pre "$pre" --rawfile body "$body_file" \
    '{tag_name: $tag, target_commitish: $sha, name: $name, prerelease: $pre, body: $body}')
rel=$(gh -X POST -H "Content-Type: application/json" -d "$req" "$api/releases")
id=$(printf '%s' "$rel" | jq -r .id)
printf '%s' "$rel" | jq -r .html_url

for a in "$@"; do
    case "$a" in
        *=*) file=${a%%=*} as=${a#*=} ;;
        *) file=$a as=$(basename "$a") ;;
    esac
    case "$as" in
        *.apk) type=application/vnd.android.package-archive ;;
        *) type=application/octet-stream ;;
    esac
    gh -X POST -H "Content-Type: $type" --data-binary "@$file" \
        "https://uploads.github.com/repos/$repo/releases/$id/assets?name=$as" | jq -r .browser_download_url
done
