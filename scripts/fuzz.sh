#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
seconds=${1:-5}
[[ $seconds =~ ^[1-9][0-9]*$ ]] || die 'Fuzz duration must be a positive integer'
scripts/build.sh fuzz
description=$(scripts/cpkt-aflpp.sh discover)
fuzzer=$(sed -n 's/^afl_fuzz=//p' <<< "$description")
work=$(scratch fuzz-results)
export AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_NO_AFFINITY=1
run "$fuzzer" -V "$seconds" -m none -i fuzz/seeds -o "$work" -- "$root/build/fuzz/fuzz-header"
[[ -z $(find "$work" -path '*/crashes/id:*' -print -quit) ]] || die "AFL++ found a crash: $work"
