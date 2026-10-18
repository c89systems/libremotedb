#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
build=$1
work=$(scratch stream-service)
pid=
cleanup() { status=$?; if [[ -n $pid ]]; then kill -TERM "$pid" 2>/dev/null || :; wait "$pid" || :; fi; [[ $status == 0 ]] || cat "$work/server.log" >&2; rm -rf -- "$work"; exit "$status"; }
trap cleanup EXIT
cd "$work"
for kind in pathname abstract; do
  control=control
  [[ $kind == pathname ]] || control=rdb-test-$$
  data=data
  [[ $kind == pathname ]] || data=$control-data
  "$build/stream-service-host" "$kind" "$control" "$work/state" "$work/observations" > server.log 2>&1 & pid=$!
  ready=0
  for ((i=0;i<100;i++)); do
    if "$build/stream-test-client" probe "$kind" "$control" "$data" 1 > /dev/null 2>&1; then ready=1; break; fi
    kill -0 "$pid"; sleep .02
  done
  [[ $ready == 1 ]] || die 'Control readiness timeout'
  "$build/stream-test-client" run "$kind" "$control" "$data" 1
  kill -TERM "$pid"; wait "$pid"; pid=
  awk -v low=1048593 -v rows=5 -f "$root/tests/observations.awk" observations
  [[ ! -e control && ! -e data ]]
  rm -- state state.lock observations
done
