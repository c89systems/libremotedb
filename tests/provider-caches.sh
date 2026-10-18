#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$root"
mkdir -p build
work=$(mktemp -d "$root/build/provider-caches.XXXXXX")
fixture_home=$work/home
trap 'rm -rf -- "$work"' EXIT
cache_environment=(env -u CPKT_DEPENDENCY_CACHE -u CPKT_TOOLCHAIN_CACHE -u XDG_CACHE_HOME -u HOME "HOME=$fixture_home")
check() {
  local dependency=$1 toolchain=$2 description
  shift 2
  "${cache_environment[@]}" cmake "-DEXPECTED=$dependency" "$@" -P tests/provider-cache.cmake
  description=$("${cache_environment[@]}" scripts/cpkt-toolchains.sh discover x86_64-linux-gnu)
  [[ "$description" == *"cache=$toolchain"$'\n'* && "$description" == *$'status=missing\n'* ]]
}
check "$fixture_home/.cache/cpkt/deps" "$fixture_home/.cache/cpkt/toolchains"
cache_environment+=("XDG_CACHE_HOME=$work/xdg cache")
check "$work/xdg cache/cpkt/deps" "$work/xdg cache/cpkt/toolchains"
cache_environment+=("CPKT_DEPENDENCY_CACHE=$work/dependency override" "CPKT_TOOLCHAIN_CACHE=$work/toolchain override")
check "$work/dependency override" "$work/toolchain override"
check "$work/cmake override" "$work/toolchain override" "-DCPKT_DEPENDENCY_CACHE=$work/cmake override"
cache_environment+=(CPKT_DEPENDENCY_CACHE= CPKT_TOOLCHAIN_CACHE= XDG_CACHE_HOME=)
check "$fixture_home/.cache/cpkt/deps" "$fixture_home/.cache/cpkt/toolchains"
cache_environment=(env -u CPKT_DEPENDENCY_CACHE -u CPKT_TOOLCHAIN_CACHE -u XDG_CACHE_HOME -u HOME)
if "${cache_environment[@]}" cmake -P tests/provider-cache.cmake >"$work/error" 2>&1; then
  echo 'Dependency cache accepted missing prerequisites' >&2; exit 1
fi
[[ $(<"$work/error") == *'HOME, XDG_CACHE_HOME, or CPKT_DEPENDENCY_CACHE is required'* ]]
if "${cache_environment[@]}" scripts/cpkt-toolchains.sh discover x86_64-linux-gnu >"$work/error" 2>&1; then
  echo 'Toolchain cache accepted missing prerequisites' >&2; exit 1
fi
[[ $(<"$work/error") == *'HOME, XDG_CACHE_HOME, or CPKT_TOOLCHAIN_CACHE is required'* ]]
echo 'Provider cache defaults, overrides, empty values and missing prerequisites passed'
