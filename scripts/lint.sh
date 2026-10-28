#!/usr/bin/env bash
# Check naming (.clang-tidy's readability-identifier-naming) across the tree.
#
# `just format-check` covers layout only: whitespace, braces, include order.
# Naming (m_ members, CamelCase types, camelBack functions, UPPER_CASE
# globals and constants) needs clang-tidy, which clangd already runs in the
# editor. This is the same check from the command line, with every other
# clang-tidy check off, so it stays fast (a few seconds, in parallel).
#
# clang-tidy needs each file's compile flags, so only files in the build's
# compile_commands.json are checked: the sources and tests.
set -uo pipefail

build_dir="${1:?usage: lint.sh <build_dir>}"
root="${REGA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"

db="$build_dir/compile_commands.json"
if [ ! -f "$db" ]; then
    echo "[lint] no $db yet (run: just setup)" >&2
    exit 1
fi

clang_tidy="${CLANG_TIDY:-clang-tidy}"
if ! command -v "$clang_tidy" >/dev/null; then
    echo "[lint] $clang_tidy not found (it comes with the devShell: direnv allow)" >&2
    exit 1
fi

files=$(find src tests -type f -name '*.cpp' | sort | while read -r f; do
    grep -q "\"file\": \"$root/$f\"" "$db" && echo "$f"
done)
[ -z "$files" ] && { echo "[lint] nothing in $db to check" >&2; exit 1; }

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
raw=$(echo "$files" | xargs -P "$jobs" -n 1 "$clang_tidy" -p "$build_dir" --quiet \
    --checks='-*,readability-identifier-naming' \
    --header-filter="$root/(src|include|tests)/.*" 2>&1)
status=$?

# A file clang-tidy cannot parse (a flag it does not know, a missing header)
# is checked for nothing, so treat it as a failure, not as "no warnings".
errors=$(echo "$raw" | grep -E "(^|: )error:" | sed "s|^$root/||" | sort -u)
if [ -n "$errors" ]; then
    echo "$errors"
    echo "[lint] clang-tidy could not parse the files above, so they were not checked" >&2
    exit 1
fi

# A header is checked once per file that includes it, hence sort -u.
out=$(echo "$raw" | grep "warning:" | sed "s|^$root/||" | sort -u)
if [ -n "$out" ]; then
    echo "$out"
    echo "[lint] naming issues above; clangd shows the same in the editor, with fixes" >&2
    exit 1
fi
# clang-tidy failing with neither (it did not start, or crashed) checked
# nothing either.
if [ "$status" -ne 0 ]; then
    echo "$raw" | tail -20
    echo "[lint] $clang_tidy failed without reporting a diagnostic; nothing was checked" >&2
    exit 1
fi
echo "[lint] $(echo "$files" | wc -l | tr -d ' ') files, no naming issues"
