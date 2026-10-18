#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
args=(--dry-run --Werror)
case ${1:-check} in check) ;; write) args=(-i);; *) die 'Usage: format.sh [check|write]';; esac
mapfile -d '' files < <(find src include tests examples fuzz -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z)
run clang-format "${args[@]}" "${files[@]}"
