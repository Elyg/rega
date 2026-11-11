#!/usr/bin/env bash
# Run every loader fuzz target (rega_fuzz_*, built with -DREGA_BUILD_FUZZERS=ON) for a
# while; fails when one of them finds a crash. `just fuzz` builds them first.
#
#   scripts/fuzz.sh <build-dir> [seconds each, default 60]
#
# Corpora and crash files live in <build-dir>/corpus and <build-dir>/crashes; the movie
# corpus starts from tests/data.
set -euo pipefail

build=${1:?usage: fuzz.sh <build-dir> [seconds]}
seconds=${2:-60}
root=$(cd "$(dirname "$0")/.." && pwd)

failed=0
for target in "$build"/rega_fuzz_*; do
    [ -x "$target" ] || continue
    name=${target##*/rega_fuzz_}
    corpus="$build/corpus/$name"
    crashes="$build/crashes"
    mkdir -p "$corpus" "$crashes"
    if [ "$name" = mov ]; then
        cp -n "$root"/tests/data/*.mov "$root"/tests/data/*.mp4 "$corpus"/
    fi
    echo "[fuzz] $name: $seconds s"
    # Small frames only: the size limit is not what is being tested.
    if ! REGA_MAX_MEGAPIXELS=64 ASAN_OPTIONS=detect_leaks=0 "$target" "$corpus" \
        -max_total_time="$seconds" -max_len=65536 -timeout=20 -rss_limit_mb=6000 \
        -artifact_prefix="$crashes/$name-" -print_final_stats=1 2>&1 | tail -n 25; then
        echo "[fuzz] $name: FAILED, input kept in $crashes" >&2
        failed=1
    fi
done
exit "$failed"
