#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
preset=${1:-debug}
case "$preset" in
  debug|valgrind|fuzz) target=x86_64-linux-gnu;;
  e2e-gnu|e2e-musl) target=x86_64-linux-${preset#e2e-};;
  *-release) target=${preset%-release};;
  *) die "Unsupported preset: $preset";;
esac
generated_roots
cmake -P cmake/CachePreflight.cmake
scripts/cpkt-toolchains.sh ensure "$target"
if [[ $target == arm64-apple-darwin ]]; then
  target_environment "$target"
  description=$(scripts/cpkt-toolchains.sh discover "$target")
  cc=$(sed -n 's/^cc=//p' <<< "$description")
  ld=$(sed -n 's/^ld=//p' <<< "$description")
  otool=$(sed -n 's/^otool=//p' <<< "$description")
  work=$(scratch darwin-toolchain-smoke); trap 'rm -rf -- "$work"' EXIT
  printf 'int main(void) { return 0; }\n' > "$work/smoke.c"
  "$cc" --ld-path="$ld" -mmacosx-version-min=11.0 -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
    -Wl,-fatal_warnings "$work/smoke.c" -o "$work/smoke"
  "$otool" -hv "$work/smoke" | grep -q 'ARM64'
fi
[[ $preset != fuzz ]] || scripts/cpkt-aflpp.sh ensure
scripts/build.sh "$preset" remotedb_deps_lonejson
