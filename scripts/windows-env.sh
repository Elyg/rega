# Source me. The Windows counterpart of the flake's devShell, since nix doesn't run there:
# puts llvm-mingw (clang for Windows, with libc++) in .sdk/ and on PATH. `just` sources it by
# itself on Windows.
#
# Works from Linux and macOS too, inside the devShell: `just windows-check` cross-compiles.

# LLVM 22: the newest conan 2.32's settings know.
LLVM_MINGW_VER=20260616

# C:/… on Windows: cmake and conan are native programs, and $PWD's /c/… means nothing to them.
export SDK_ROOT="$(pwd -W 2>/dev/null || pwd)/.sdk"
export SHELL_TYPE="host"
# The Linux devShell's SDL3 (nixpkgs) is no use here: conan's is the Windows one.
unset SDL3_ROOT

_sdk_fetch() { # url, file
    curl -fsSL --retry 3 "$1" -o "$2"
}

_sdk_unzip() { # zip, directory
    "$(command -v python3 || command -v python)" -m zipfile -e "$1" "$2"
}

_sdk_setup() {
    local toolchain="$SDK_ROOT/llvm-mingw" asset dir tmp

    if [ ! -x "$toolchain/bin/x86_64-w64-mingw32-clang" ] &&
        [ ! -f "$toolchain/bin/x86_64-w64-mingw32-clang.exe" ]; then
        case "$(uname -s)" in
            Darwin) asset="llvm-mingw-$LLVM_MINGW_VER-ucrt-macos-universal.tar.xz" ;;
            Linux) asset="llvm-mingw-$LLVM_MINGW_VER-ucrt-ubuntu-22.04-$(uname -m).tar.xz" ;;
            *) asset="llvm-mingw-$LLVM_MINGW_VER-ucrt-x86_64.zip" ;;
        esac
        tmp=$(mktemp -d)
        echo "fetching $asset into .sdk/llvm-mingw" >&2
        _sdk_fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VER/$asset" "$tmp/$asset" || return 1
        case "$asset" in
            *.zip) _sdk_unzip "$tmp/$asset" "$tmp" || return 1; dir=${asset%.zip} ;;
            *) tar -xJf "$tmp/$asset" -C "$tmp" || return 1; dir=${asset%.tar.xz} ;;
        esac
        rm -rf "$toolchain"
        mkdir -p "$SDK_ROOT"
        mv "$tmp/$dir" "$toolchain" || return 1
        rm -rf "$tmp"
    fi

    # A POSIX PATH: its separator is ":" here, so no C:/… in it. Not $PWD as it stands: a bash
    # started through a native program, like `just`, inherits it as D:/a/….
    local root
    root=$(cygpath -u "$PWD" 2>/dev/null || pwd)
    export PATH="$root/.sdk/llvm-mingw/bin:$PATH"
}

_sdk_setup || echo "windows-env.sh: setting up .sdk failed" >&2
