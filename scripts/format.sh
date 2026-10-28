#!/usr/bin/env bash
# Format every hand-written C/C++ source in place, per .clang-format.
#
#   --check   report what is unformatted and fail, changing nothing (CI)
#
# CLANG_FORMAT overrides the binary, which CI uses to pin a version: the
# devShell has clang-format 20, and a different major version formats
# differently enough to fail a check for no real reason.
set -uo pipefail

root="${REGA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"

check=0
[ "${1:-}" = "--check" ] && check=1

# clang-format changes its output between major versions, so the version is
# part of the project's configuration, not a detail of your machine. 20 is what
# the flake pins (llvmPackages_20.clang-tools).
expected_major=20

clang_format="${CLANG_FORMAT:-clang-format}"
if ! command -v "$clang_format" >/dev/null; then
    echo "[format] $clang_format not found" >&2
    exit 1
fi

major=$("$clang_format" --version | grep -oE '[0-9]+' | head -1)
if [ "$major" != "$expected_major" ]; then
    if [ $check -eq 1 ]; then
        echo "[format] clang-format $major, but this project formats with $expected_major." >&2
        echo "[format] A check with the wrong version says nothing useful. Set CLANG_FORMAT." >&2
        exit 1
    fi
    echo "[format] WARNING: clang-format $major, but this project formats with $expected_major."
    echo "[format]          Formatting now will churn the tree and fail CI's check."
fi

echo "[format] $("$clang_format" --version)"

files=$(find include src tests tools -type f \( -name '*.h' -o -name '*.cpp' \) 2>/dev/null || true)
[ -z "$files" ] && { echo "[format] nothing to format"; exit 0; }

if [ $check -eq 1 ]; then
    if echo "$files" | xargs "$clang_format" --style=file --dry-run --Werror; then
        echo "[format] $(echo "$files" | wc -l) files are formatted"
    else
        echo "[format] run: just format" >&2
        exit 1
    fi
else
    echo "$files" | xargs "$clang_format" --style=file -i
    echo "[format] formatted $(echo "$files" | wc -l) files"
fi
