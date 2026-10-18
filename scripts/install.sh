#!/usr/bin/env bash
# Install the rvtui release binary for this machine (Linux x86_64 / aarch64,
# macOS Apple Silicon) into ~/.local/bin.
#
#   curl -fsSL https://raw.githubusercontent.com/Elyg/rvtui/main/scripts/install.sh | bash
#
# RVTUI_VERSION  a release to install, e.g. 0.0.15 (default: the latest)
# RVTUI_BIN_DIR  where to put the binary (default: ~/.local/bin)
set -euo pipefail

repo="https://github.com/Elyg/rvtui"

# Everything in a function, called on the last line: piped into bash, a
# download cut short runs nothing rather than half a script.
main() {
    local os arch platform version dir
    os=$(uname -s)
    arch=$(uname -m)
    case "$os/$arch" in
        Linux/x86_64) platform=linux-x86_64 ;;
        Linux/aarch64 | Linux/arm64) platform=linux-aarch64 ;;
        Darwin/arm64) platform=macos-arm64 ;;
        *)
            echo "[install] no rvtui release for $os $arch; build from source: $repo" >&2
            exit 1
            ;;
    esac

    version="${RVTUI_VERSION:-}"
    if [ -z "$version" ]; then
        # /releases/latest redirects to /releases/tag/vX.Y.Z.
        version=$(curl -fsSLo /dev/null -w '%{url_effective}' "$repo/releases/latest")
        version="${version##*/v}"
    fi
    version="${version#v}"

    dir="${RVTUI_BIN_DIR:-$HOME/.local/bin}"
    mkdir -p "$dir"
    echo "[install] rvtui $version ($platform) → $dir"
    curl -fsSL "$repo/releases/download/v$version/rvtui-$version-$platform.tar.gz" |
        tar -xz -C "$dir" rvtui

    case ":$PATH:" in
        *":$dir:"*) ;;
        *) echo "[install] add $dir to your PATH to run rvtui" ;;
    esac
}

main "$@"
