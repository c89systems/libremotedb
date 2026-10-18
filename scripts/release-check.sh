#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
[[ -z ${REMOTEDB_VERSION_OVERRIDE:-} ]] || die 'Final release rejects REMOTEDB_VERSION_OVERRIDE'
[[ -e $root/.git ]] || die 'Final release requires its own Git checkout'
owner=$(git rev-parse --show-toplevel)
[[ $(CDPATH='' cd -- "$owner" && pwd -P) == "$root" ]] || die 'Final release Git ownership does not match the source root'
branch=$(git symbolic-ref --quiet --short HEAD) || die 'Final release requires an attached default branch'
default=$(git symbolic-ref --quiet --short refs/remotes/origin/HEAD) || die 'Set origin/HEAD to the authoritative default branch'
[[ $branch == "${default#origin/}" ]] || die "Final release requires ${default#origin/}, currently $branch"
[[ -z $(git status --porcelain) ]] || die 'Final release requires a clean committed worktree'
[[ $(git remote get-url origin) == "$(git remote get-url --push origin)" ]] || die 'Fetch/push destinations disagree'
git verify-commit HEAD || die 'Final release requires a verified signed commit'
version=$(scripts/release_version.sh)
[[ $version != 0.0.0 ]] || die 'Final release requires an exact lightweight vX.Y.Z tag on HEAD'
[[ $(git cat-file -t "refs/tags/v$version") == commit && $(git rev-parse "refs/tags/v$version") == "$(git rev-parse HEAD)" ]] || die 'Final release tag does not identify HEAD'
printf 'Validated signed %s at %s, tag v%s\n' "$branch" "$(git rev-parse HEAD)" "$version"
