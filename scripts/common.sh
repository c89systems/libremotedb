#!/usr/bin/env bash
# Project helpers; sequencing belongs to Make and dependency tracking to CMake.
source "$(dirname -- "${BASH_SOURCE[0]}")/require-host-bash.sh"
root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root" || exit 1
die() { printf 'remotedb: %s\n' "$*" >&2; exit 1; }
run() { printf '+ '; printf '%q ' "$@"; printf '\n'; "$@"; }
cache_value() { sed -n "s/^$2:[^=]*=//p" "$1/CMakeCache.txt"; }
generated_roots() {
  local path
  for path in build dist .cache; do
    [[ ! -L "$root/$path" ]] || die "Refusing redirected generated root: $path"
  done
}
scratch() { generated_roots; mkdir -p build; mktemp -d "$root/build/$1.XXXXXXXX"; }
target_environment() {
  local description path prefix
  description=$(scripts/cpkt-toolchains.sh discover "$1") || return
  [[ $(sed -n 's/^status=//p' <<< "$description") == ready ]] || die "Missing toolchain; run make deps PRESET=$1-release"
  if [[ $1 == arm64-apple-darwin ]]; then
    path=$(sed -n 's/^root=//p' <<< "$description")
    prefix=$(sed -n 's/^prefix=//p' <<< "$description")
    export PATH="$path/bin:$PATH" CPKT_OSXCROSS_HOST="$prefix"
  fi
}
