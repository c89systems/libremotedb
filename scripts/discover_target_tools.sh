#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
[[ $# == 1 ]] || { echo 'Usage: discover_target_tools.sh CONFIGURED_BUILD_DIR' >&2; exit 2; }
build=$1
target=$(cache_value "$build" REMOTEDB_TARGET_ID)
[[ -n $target ]] || die 'Configured build must identify REMOTEDB_TARGET_ID'
case "$target" in x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin) ;; *) die "Unsupported configured target: $target";; esac
printf 'TARGET=%q\n' "$target"
compiler=$(cache_value "$build" CMAKE_C_COMPILER)
prefix=${compiler##*/}; prefix=${prefix%-gcc}; prefix=${prefix%-clang}; prefix=${prefix%-cc}
for pair in CC:CMAKE_C_COMPILER NM:CMAKE_NM STRIP:CMAKE_STRIP READELF:CMAKE_READELF OTOOL:CPKT_OTOOL LD:CMAKE_LINKER; do
  name=${pair%%:*}
  value=$(cache_value "$build" "${pair#*:}")
  if [[ $target == *darwin && $name == READELF || $target != *darwin && $name == OTOOL ]]; then continue; fi
  override=REMOTEDB_$name
  value=${!override:-$value}
  if [[ -z $value ]]; then value=${compiler%/*}/$prefix-${name,,}; fi
  [[ -x $value ]] || die "Configured target tool missing: $name for $target"
  if [[ $target == *darwin && $(uname -s) != Darwin && $value != "${compiler%/*}/"* ]]; then
    die "Refusing host tool for Darwin $name: $value"
  fi
  printf '%s=%q\n' "$name" "$value"
done
