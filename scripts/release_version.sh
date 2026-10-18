#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
version=
if [[ -e "$root/.git" ]]; then
  owner=$(git -C "$root" rev-parse --show-toplevel)
  [[ $(CDPATH='' cd -- "$owner" && pwd -P) == "$root" ]] || {
    echo 'Git ownership does not match the physical source root' >&2; exit 1;
  }
  while IFS= read -r tag; do
    [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || continue
    if git -C "$root" symbolic-ref -q "refs/tags/$tag" >/dev/null; then
      echo "Symbolic release tag is unsupported: $tag" >&2; exit 1;
    fi
    [[ $(git -C "$root" cat-file -t "refs/tags/$tag") == commit ]] || {
      echo "Annotated/signed semver tag on HEAD is unsupported: $tag" >&2; exit 1;
    }
    [[ -z $version ]] || { echo 'Multiple exact release tags on HEAD' >&2; exit 1; }
    version=${tag#v}
  done < <(git -C "$root" tag --points-at HEAD)
elif [[ -f "$root/VERSION" ]]; then
  version=$(cat "$root/VERSION")
else
  echo 'Source archive requires an injected VERSION' >&2
  exit 1
fi
version=${version:-${REMOTEDB_VERSION_OVERRIDE:-0.0.0}}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid version: $version" >&2; exit 1; }
printf '%s\n' "$version"
