#!/usr/bin/env bash
# Install the rega release binary for this machine (Linux x86_64 / aarch64,
# macOS Apple Silicon) into ~/.local/bin.
#
#   curl -fsSL https://raw.githubusercontent.com/Elyg/rega/main/scripts/install.sh | bash
#
# REGA_VERSION  a release to install, e.g. 0.0.15 (default: the latest)
# REGA_BIN_DIR  where to put the binary (default: ~/.local/bin)
# REGA_SKIP_VERIFY=1  install without checking the download against the release's
#               SHA256SUMS (releases older than the checksums have none)
#
# Its licence and third-party notices go in ~/.local/share/doc/rega.
set -euo pipefail

repo="https://github.com/Elyg/rega"

# Everything in a function, called on the last line: piped into bash, a
# download cut short runs nothing rather than half a script.
main() {
    local os arch platform version dir doc
    os=$(uname -s)
    arch=$(uname -m)
    case "$os/$arch" in
        Linux/x86_64) platform=linux-x86_64 ;;
        Linux/aarch64 | Linux/arm64) platform=linux-aarch64 ;;
        Darwin/arm64) platform=macos-arm64 ;;
        *)
            echo "[install] no rega release for $os $arch; build from source: $repo" >&2
            exit 1
            ;;
    esac

    version="${REGA_VERSION:-}"
    if [ -z "$version" ]; then
        # /releases/latest redirects to /releases/tag/vX.Y.Z.
        version=$(curl -fsSLo /dev/null -w '%{url_effective}' "$repo/releases/latest")
        version="${version##*/v}"
    fi
    version="${version#v}"

    dir="${REGA_BIN_DIR:-$HOME/.local/bin}"
    doc="${XDG_DATA_HOME:-$HOME/.local/share}/doc/rega"
    mkdir -p "$dir" "$doc"
    echo "[install] rega $version ($platform) → $dir"
    local tmp base archive want got
    tmp=$(mktemp -d)
    # Expanded now: $tmp is local and gone by the time the trap runs.
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" EXIT
    base="$repo/releases/download/v$version"
    archive="rega-$version-$platform.tar.gz"
    curl -fsSL -o "$tmp/$archive" "$base/$archive"

    # Nothing is unpacked until the download matches the release's SHA256SUMS.
    if [ "${REGA_SKIP_VERIFY:-}" = 1 ]; then
        echo "[install] REGA_SKIP_VERIFY=1: not checking the download" >&2
    else
        if ! curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"; then
            echo "[install] release v$version has no SHA256SUMS to check against;" >&2
            echo "[install] pick a newer release, or set REGA_SKIP_VERIFY=1" >&2
            exit 1
        fi
        want=$(awk -v f="$archive" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
        if command -v sha256sum >/dev/null 2>&1; then
            got=$(sha256sum "$tmp/$archive" | awk '{ print $1 }')
        else
            got=$(shasum -a 256 "$tmp/$archive" | awk '{ print $1 }')
        fi
        if [ -z "$want" ] || [ "$want" != "$got" ]; then
            echo "[install] checksum mismatch for $archive: not installed" >&2
            echo "[install]   expected ${want:-<not listed>}" >&2
            echo "[install]   got      $got" >&2
            exit 1
        fi
        echo "[install] sha256 ok"
    fi

    tar -xzf "$tmp/$archive" -C "$doc"
    mv -f "$doc/rega" "$dir/rega"

    case ":$PATH:" in
        *":$dir:"*) ;;
        *) echo "[install] add $dir to your PATH to run rega" ;;
    esac
}

main "$@"
