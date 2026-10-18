#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch darwin-resolver); trap 'rm -rf -- "$work"' EXIT
fake=$work/osxcross/bin
mkdir -p "$fake"
for prefix in arm64-apple-darwin25.4 arm64-apple-darwin25.9 arm64-apple-darwin25.10; do
  for tool in clang clang++ ld ar ranlib strip nm otool; do
    [[ $prefix != *.10 || $tool != otool ]] || continue
    ln -s /usr/bin/true "$fake/$prefix-$tool"
  done
done
resolve() { env -u CPKT_OSXCROSS_HOST "OSXCROSS_ROOT=$work/osxcross" "CPKT_TOOLCHAIN_CACHE=$work/cache" "$@" scripts/cpkt-toolchains.sh discover arm64-apple-darwin; }
description=$(resolve env)
grep -Fxq 'prefix=arm64-apple-darwin25.9' <<< "$description"
description=$(resolve env CPKT_OSXCROSS_HOST=arm64-apple-darwin25.4)
grep -Fxq 'prefix=arm64-apple-darwin25.4' <<< "$description"
description=$(resolve env CPKT_OSXCROSS_HOST=arm64-apple-darwin25.10)
grep -Fxq 'status=missing' <<< "$description"
if grep -q '^prefix=' <<< "$description"; then die 'Incomplete override fell back to another prefix'; fi
mkdir "$work/build"
cat > "$work/build/CMakeCache.txt" <<CACHE
REMOTEDB_TARGET_ID:STRING=arm64-apple-darwin
CMAKE_C_COMPILER:FILEPATH=$fake/arm64-apple-darwin25.9-clang
CACHE
tools=$(scripts/discover_target_tools.sh "$work/build"); eval "$tools"
[[ $OTOOL == "$fake/arm64-apple-darwin25.9-otool" && $NM == "$fake/arm64-apple-darwin25.9-nm" ]]
if REMOTEDB_NM=/usr/bin/nm scripts/discover_target_tools.sh "$work/build" > "$work/error" 2>&1; then die 'Darwin inspection accepted host nm'; fi
[[ $(<"$work/error") == *'Refusing host tool for Darwin'* ]]
echo 'Newest complete Darwin prefix, exact override and matching inspection tools passed'
