#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
generated_roots
case ${1:-all} in
  all) scripts/devenv.sh reset; rm -rf -- "$root/build" "$root/dist" "$root/.cache";;
  dist) rm -rf -- "$root/dist";;
  *) die 'Usage: clean.sh [all|dist]';;
esac
