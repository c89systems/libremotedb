#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
build=$1
work=$(scratch lifecycle-contract); trap 'rm -rf -- "$work"' EXIT
tools=$(scripts/discover_target_tools.sh "$build"); eval "$tools"
cmake "-DBUILD=$build" -P tests/compile-flags.cmake
if [[ $build == */debug ]]; then clangd "--check=$root/src/client.c" "--compile-commands-dir=$build" > "$work/clangd.log" 2>&1; fi
for component in client server; do
  library=libremotedb.so; [[ $component == client ]] || library=libremotedbsrv.so
  [[ ! -e $build/$library ]] || scripts/inspect-binary.sh "$build" "$build/$library" exports "$component"
done
args=(cmake -S "$root" -G Ninja -DREMOTEDB_BUILD_TESTS=OFF -DREMOTEDB_BUILD_EXAMPLES=OFF -DREMOTEDB_VERSION_OVERRIDE=8.7.6 "-DREMOTEDB_TARGET_ID=$TARGET")
if "${args[@]}" -B "$work/iodbc" -DREMOTEDB_DEPENDENCY_MODE=iodbc > "$work/error" 2>&1; then die 'Unimplemented dependency mode accepted'; fi
grep -q 'iODBC backend is not implemented' "$work/error"
"${args[@]}" -B "$work/candidate" > "$work/configure.log" 2>&1
expected=$(REMOTEDB_VERSION_OVERRIDE=8.7.6 scripts/release_version.sh)
grep -Fq "\"$expected\"" "$work/candidate/generated/remotedb/version.h"
grep -Fq "Version: $expected" "$work/candidate/remotedb.pc"
if CPKT_TOOLCHAIN_CACHE="$work/missing cache" "${args[@]}" -B "$work/missing" > "$work/error" 2>&1; then die 'Configure acquired an unprepared toolchain'; fi
[[ ! -e $work/missing\ cache ]]
for source in 'int main(void) { long long x = 0; return (int)x; }' 'int main(void) { int unused; return 0; }'; do
  printf '%s\n' "$source" > "$work/invalid.c"
  if "$CC" -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror -c "$work/invalid.c" -o "$work/invalid.o" > "$work/error" 2>&1; then die 'Invalid owned C89 source accepted'; fi
done
mkdir -p "$work/sdk"
for leak in "$root" "$HOME" /"tmp/private-fixture" cpkt/"toolchains/roots/private" cpkt/"deps/archives/private"; do
  printf '%s\n' "$leak" > "$work/sdk/leak"
  if cmake "-DREMOTEDB_SOURCE_ROOT=$root" "-DTREE=$work/sdk" -P cmake/Privacy.cmake > "$work/error" 2>&1; then die "Privacy gate accepted $leak"; fi
done
rm -- "$work/sdk/leak"
printf '\177ELF/%s\0' 'tmp/tmpfile_XXXXXX' > "$work/sdk/elf"
cmake "-DREMOTEDB_SOURCE_ROOT=$root" "-DTREE=$work/sdk" -P cmake/Privacy.cmake
printf '/%s\0' 'tmp/private' >> "$work/sdk/elf"
if cmake "-DREMOTEDB_SOURCE_ROOT=$root" "-DTREE=$work/sdk" -P cmake/Privacy.cmake > "$work/error" 2>&1; then die 'Runtime template exemption hid a leak'; fi
printf 'int main(void) { return 0; }\n' > "$work/loader.c"
"$CC" -std=c89 -Werror "$work/loader.c" -Wl,-rpath,/opt/private-local -o "$work/loader"
if scripts/inspect-binary.sh "$build" "$work/loader" loader > "$work/error" 2>&1; then die 'Private ELF runtime path accepted'; fi
"$CC" -std=c89 -Werror "$work/loader.c" -Wl,--dynamic-linker=/opt/private-local/ld.so -o "$work/loader"
if scripts/inspect-binary.sh "$build" "$work/loader" loader > "$work/error" 2>&1; then die 'Private ELF interpreter accepted'; fi
fake=$work/consumer
mkdir -p "$fake/scripts" "$fake/build/.hidden" "$fake/.cache/.hidden" "$fake/dist/.hidden" "$work/shared"
cp scripts/{clean.sh,common.sh,require-host-bash.sh,devenv.sh} "$fake/scripts/"
printf shared > "$work/shared/sentinel"
"$fake/scripts/clean.sh" all
[[ ! -e $fake/build && ! -e $fake/.cache && ! -e $fake/dist && -f $work/shared/sentinel ]]
ln -s "$work/shared" "$fake/.cache"
if "$fake/scripts/clean.sh" all > "$work/error" 2>&1; then die 'Clean accepted a redirected generated root'; fi
[[ -f $work/shared/sentinel ]]
if cmake -DCPKT_DEPENDENCY_CACHE=/var/tmp -P cmake/CachePreflight.cmake > "$work/error" 2>&1; then die 'Borrowed cache path accepted'; fi
echo 'C89, configured tools, dependency modes, version propagation, privacy, loaders and cleanup passed'
