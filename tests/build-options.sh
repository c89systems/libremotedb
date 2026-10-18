#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
target=$(cache_value "$1" REMOTEDB_TARGET_ID)
work=$(scratch build-options); trap 'rm -rf -- "$work"' EXIT
for name in shared fixtures; do
  static=OFF binary=ON
  [[ $name == shared ]] || { static=ON; binary=OFF; }
  build=$work/$name
  cmake -S "$root" -B "$build" -G Ninja -Werror=dev -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DREMOTEDB_TARGET_ID=$target" \
    "-DREMOTEDB_BUILD_STATIC=$static" "-DREMOTEDB_BUILD_BINARY=$binary" -DREMOTEDB_BUILD_EXAMPLES=OFF
  cmake --build "$build" --parallel 1
  ctest --test-dir "$build" --output-on-failure --stop-on-failure --no-tests=error -R '^(control|streaming-service)$'
  [[ ! -e $build/stream-example ]]
  if [[ $binary == OFF ]]; then [[ ! -e $build/remotedbd && ! -e $build/remotedbctl ]]; else
    cmake --install "$build" --prefix "$work/installed"
    for executable in remotedbd remotedbctl; do "$root/scripts/inspect-binary.sh" "$build" "$work/installed/bin/$executable" loader; done
    [[ $(find "$work/installed/bin" -type f | wc -l) == 2 ]]
  fi
done
