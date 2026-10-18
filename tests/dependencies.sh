#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch dependency-cache)
pid=
status=0
trap 'status=$?; if [[ -n $pid ]]; then kill "$pid"; wait "$pid" 2>/dev/null || :; fi; rm -rf -- "$work"; exit "$status"' EXIT
printf 'verified local archive fixture\n' > "$work/payload"
digest=$(sha256sum < "$work/payload" | cut -d' ' -f1)
python3 tests/archive-origin.py "$work/payload" "$work/port" "$work/requests" & pid=$!
for ((i=0;i<100;i++)); do [[ ! -s $work/port ]] || break; kill -0 "$pid"; sleep .01; done
[[ -s $work/port ]]
url=http://127.0.0.1:$(cat "$work/port")/fixture.tar.gz
acquire() { cmake "-DCPKT_DEPENDENCY_CACHE=$work/cache" "-DTEST_URL=${1:-$url}" "-DTEST_DIGEST=${2:-$digest}" "-DTEST_OUTPUT=$work/result" -P tests/acquire.cmake; }
acquire
archive=$(cat "$work/result")
[[ $(wc -l < "$work/requests") == 1 && $(sha256sum < "$archive" | cut -d' ' -f1) == "$digest" ]]
acquire; [[ $(wc -l < "$work/requests") == 1 ]]
acquire "${url%/*}/renamed.tar.gz"; [[ $(wc -l < "$work/requests") == 1 ]]
# Corruption is never accepted and recovery performs exactly one acquisition.
printf corrupt > "$archive"
rm -- "${archive%/*}/renamed.tar.gz"
acquire; [[ $(wc -l < "$work/requests") == 2 ]]
if acquire "$url" "$(printf '%064d' 0)" > "$work/error" 2>&1; then die 'Bad checksum accepted'; fi
[[ $(wc -l < "$work/requests") == 5 ]]
[[ -z $(find "$work/cache" -name '*.partial' -print -quit) ]]
if acquire "$url" invalid > "$work/error" 2>&1; then die 'Malformed digest accepted'; fi
[[ $(wc -l < "$work/requests") == 5 ]]
kill "$pid"; wait "$pid" 2>/dev/null || :; pid=
acquire 'http://127.0.0.1:1/fixture.tar.gz'
echo 'Verified miss, zero-request hits/renames, corruption, hash rejection and offline reuse'
