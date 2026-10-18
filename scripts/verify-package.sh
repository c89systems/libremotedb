#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
[[ $# == 2 ]] || die 'Usage: verify-package.sh ARCHIVE TARGET|source'
archive=$1 target=$2
name=${archive##*/}; name=${name%.tar.gz}
work=$(scratch package-verify)
trap 'rm -rf -- "$work"' EXIT
scripts/extract.sh "$archive" "$work" "$name"
tree=$work/$name
cmake "-DREMOTEDB_SOURCE_ROOT=$root" "-DTREE=$tree" -P cmake/Privacy.cmake
version=$(scripts/release_version.sh)
if [[ $target == source ]]; then
  for path in VERSION RELEASE_MANIFEST LICENSE CMakeLists.txt; do
    [[ -f $tree/$path ]] || die "Missing source input: $path"
  done
  [[ $(cat "$tree/VERSION") == "$version" ]] || die 'Source version mismatch'
  [[ -z $(find "$tree" -type l -print -quit) ]] || die 'Unexpected source symlink'
  (cd "$tree"; find . -type f -printf '%P\n' | LC_ALL=C sort) > "$work/actual"
  cmp "$tree/RELEASE_MANIFEST" "$work/actual" || die 'Source manifest does not match archive payload'
  awk -F/ 'NF>1 { path=$1; print path; for(i=2;i<NF;i++) { path=path "/" $i; print path } }' \
    "$tree/RELEASE_MANIFEST" | LC_ALL=C sort -u > "$work/expected-directories"
  (cd "$tree"; find . -mindepth 1 -type d -printf '%P\n' | LC_ALL=C sort) > "$work/directories"
  cmp "$work/expected-directories" "$work/directories" || die 'Source directory inventory mismatch'
  exit 0
fi
build=$root/build/$target-release
[[ -f $build/CMakeCache.txt ]] || die "Missing matching build: $build"
target_environment "$target"
for path in include/remotedb/{protocol.h,remotedb.h,server.h,endpoint.h,control.h,service.h,version.h} \
  lib/{libremotedb.a,libremotedbsrv.a,liblonejson.a} bin/{remotedbd,remotedbctl} include/lonejson.h \
  lib/cmake/remotedb/remotedb-config.cmake lib/cmake/lonejson/lonejsonConfig.cmake \
  lib/pkgconfig/{remotedb.pc,remotedbsrv.pc,lonejson.pc} \
  share/doc/remotedb/LICENSE share/remotedb/licenses/lonejson/LICENSE share/remotedb/dependencies.json; do
  [[ -f $tree/$path ]] || die "Missing SDK payload: $path"
done
cmake "-DTREE=$tree" "-DTARGET_ID=$target" "-DEXPECTED_VERSION=$version" "-DRECIPE=$root/cmake/Dependencies.cmake" -P cmake/PackageMetadata.cmake
if [[ $target == *darwin ]]; then shared=libremotedb.dylib; server_shared=libremotedbsrv.dylib; else shared=libremotedb.so; server_shared=libremotedbsrv.so; fi
for path in "$tree"/lib/*.a "$tree"/bin/* "$tree/lib/$shared" "$tree/lib/$server_shared"; do
  scripts/inspect-binary.sh "$build" "$path" architecture
  [[ $path == *.a ]] || scripts/inspect-binary.sh "$build" "$path" loader
done
scripts/inspect-binary.sh "$build" "$tree/lib/$shared" exports client
scripts/inspect-binary.sh "$build" "$tree/lib/$server_shared" exports server
if [[ $target == *darwin ]]; then
  tools=$(scripts/discover_target_tools.sh "$build"); eval "$tools"
  "$OTOOL" -l "$tree/lib/liblonejson.a" | grep -q 'minos 11.0'
  for path in "$tree"/bin/* "$tree/lib/$shared" "$tree/lib/$server_shared"; do
    cmp "$path" "$build/${path##*/}" || die 'Darwin installation changed signed link output'
  done
fi
# Repeat consumers after relocation; standard metadata supplies the full closure.
for location in original relocated; do
  for method in cmake pkgconfig; do
    for variant in static shared; do
      consumer=$work/$location-$method-$variant
      PKG_CONFIG_LIBDIR="$tree/lib/pkgconfig" PKG_CONFIG_PATH='' cmake -S tests/sdk-consumer -B "$consumer" -G Ninja \
        "-DCMAKE_TOOLCHAIN_FILE=$root/cmake/toolchain.cmake" "-DREMOTEDB_TARGET_ID=$target" \
        "-DREMOTEDB_SOURCE_ROOT=$root" "-DCMAKE_PREFIX_PATH=$tree" "-DSDK_PREFIX=$tree" \
        "-DMETHOD=$method" "-DVARIANT=$variant" "-DSHARED_LIBRARY=$shared" -DCMAKE_BUILD_TYPE=Release -Werror=dev
      cmake --build "$consumer" --parallel 1
      if [[ $target == x86_64-linux-* ]]; then "$consumer/consumer"; fi
    done
  done
  if cmake --build "$consumer" --target private-negative --parallel 1 > "$work/negative.log" 2>&1; then die 'Private ABI sentinel linked'; fi
  grep -q rdb_io_header "$work/negative.log" || die 'Private ABI check failed for another reason'
  if [[ $location == original ]]; then mv "$tree" "$work/relocated sdk"; tree=$work/relocated\ sdk; fi
done
if [[ $target == x86_64-linux-* ]]; then "$tree/bin/remotedbd" --version; fi
printf 'Verified %s (architecture, loaders, exports, provenance, relocation and consumers)\n' "$name"
