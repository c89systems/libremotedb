#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch service-test)
status=0
trap 'status=$?; if [[ $status != 0 ]]; then cat "$work"/*.log >&2 || :; fi; rm -rf -- "$work"; exit "$status"' EXIT
"$@" "$work"
