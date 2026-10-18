#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
[[ $# -ge 1 && $# -le 2 ]] || die 'Usage: build.sh PRESET [check-local|TARGET]'
generated_roots
preset=$1
case "$preset" in
  debug|valgrind|fuzz) target=x86_64-linux-gnu;;
  e2e-gnu|e2e-musl) target=x86_64-linux-${preset#e2e-};;
  *-release) target=${preset%-release};;
  *) die "Unsupported preset: $preset";;
esac
target_environment "$target"
run cmake --preset "$preset" -Werror=dev "-DREMOTEDB_VERSION_OVERRIDE=${REMOTEDB_VERSION_OVERRIDE:-}"
args=(cmake --build "build/$preset" --parallel 1)
[[ $# == 1 ]] || args+=(--target "$2")
run "${args[@]}"
