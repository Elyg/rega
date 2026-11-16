# rega — command interface. `just` with no arguments lists everything.
#
# One active build config at a time, so no recipe takes it as an argument.
# Change it with `just set-config Release`; everything else follows.

set shell := ["bash", "-c"]
# Spelled out: a plain "bash" finds the WSL launcher in System32 before Git's.
set windows-shell := ["C:/Program Files/Git/bin/bash.exe", "-c"]

# Apple Silicon on macOS, x64 elsewhere (presets: macos-arm64-*, linux-x64-*, windows-x64-*).
arch := if os() == "macos" { "macos-arm64" } else if os() == "windows" { "windows-x64" } else { "linux-x64" }
# Windows has no nix: its stand-in puts llvm-mingw on PATH (scripts/windows-env.sh).
env := if os() == "windows" { "source scripts/windows-env.sh && " } else { "" }

# The active config: whatever `just set-config` last wrote, else $REGA_CONFIG, else Release.
state_file := ".rega-config"
config := if path_exists(state_file) == "true" { trim(`cat .rega-config`) } else { env('REGA_CONFIG', 'Release') }

preset := arch + "-" + config
build_dir := "build" / preset
conan_dir := "conan-build" / "conan-" + arch + "-clang" / config

# `just` and `just help` both print this.
alias help := default

# List recipes, grouped
default:
    @just --list --unsorted --list-heading $'\nrega — building {{ config }} for {{ arch }}\n'

# --- build -------------------------------------------------------------------

# Install conan dependencies for the active config
[group('build')]
deps: _recipes
    {{ env }}conan install . -pr:h=./conan-profiles/conan-{{ arch }}-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
        -s build_type={{ config }} --output-folder={{ conan_dir }} --build=missing

# conancenter recipes rega needs changed (conan-recipes/README.md), into the conan cache
[private]
_recipes:
    @conan export conan-recipes/opencolorio --version 2.5.2 >/dev/null 2>&1

# Configure the build tree (re-run after adding files or changing presets)
[group('build')]
setup: _deps-once
    {{ env }}cmake --preset {{ preset }}
    ln -sf {{ build_dir }}/compile_commands.json compile_commands.json

# Deps and configure only when missing, so plain `just build` works on a fresh clone.
[private]
_deps-once:
    @{{ if path_exists(conan_dir / "conan_toolchain.cmake") == "true" { "" } else { "just deps" } }}

[private]
configure: _deps-once
    @{{ if path_exists(build_dir / "CMakeCache.txt") == "true" { "" } else { "just setup" } }}

# Build rega and the tests
[group('build')]
build: configure
    {{ env }}cmake --build --preset {{ preset }}

# --- config ------------------------------------------------------------------

# Switch the active config: `just set-config Debug` (Debug | Release; Release is the default)
[group('config')]
set-config value:
    @[[ "{{ value }}" == "Debug" || "{{ value }}" == "Release" ]] || { echo "Debug or Release" >&2; exit 1; }
    @echo "{{ value }}" > {{ state_file }}
    @echo "active config: {{ value }}"

# --- run ---------------------------------------------------------------------

# Run rega: `just run`, `just run demos/`, `just run --dump foo.exr`
[group('run')]
run *args: build
    ./{{ build_dir }}/rega {{ args }}

# Run rega in its own window: `just gui`, `just gui demos/` (what artists get, no kitty needed)
[group('run')]
gui *args: build
    ./{{ build_dir }}/rega --gui {{ args }}

# Draw the window off screen and save it: `just gui-shot out.png --gui-keys 24 shot.####.exr`
[group('run')]
gui-shot out *args: build
    SDL_VIDEO_DRIVER=offscreen ./{{ build_dir }}/rega --gui-screenshot {{ out }} {{ args }}
    @echo "{{ out }}"

# Download the OpenEXR test images into demos/ (gitignored)
[group('run')]
fetch-demos:
    @[ -d demos/openexr-images ] || git clone --depth 1 https://github.com/AcademySoftwareFoundation/openexr-images.git demos/openexr-images
    @echo "demo images in demos/openexr-images"

# Render the multi-layer demo sequence into demos/rega-sequence/ (numpy ray tracer)
[group('run')]
demo-sequence *args:
    nix shell --impure --expr 'let p = (builtins.getFlake "nixpkgs").legacyPackages.${builtins.currentSystem}; in p.python3.withPackages (ps: [ ps.openexr ps.numpy ])' \
        -c python3 scripts/demo-sequence.py {{ args }}

# Cut the demo sequence into incomplete EXRs (truncated copies, renders in progress) in demos/incomplete/
[group('run')]
demo-incomplete *args:
    python3 scripts/incomplete-exrs.py {{ args }}

# Fake bucket renders killed mid-frame (tiled, in progress) in demos/buckets/: `just demo-buckets [source.exr]`
[group('run')]
demo-buckets source="demos/rega-sequence/shot.1001.exr": configure
    cmake --build --preset {{ preset }} --target rega_buckets
    ./{{ build_dir }}/rega_buckets {{ source }} demos/buckets

# Browse the demo images: `just demo`
[group('run')]
demo: fetch-demos build
    ./{{ build_dir }}/rega demos/openexr-images

# --- test --------------------------------------------------------------------

# Build and run the unit tests
[group('test')]
test: build
    ctest --preset {{ preset }}

# Fuzz the image loaders (libFuzzer + ASan/UBSan) for `seconds` each: `just fuzz 300`
[group('test')]
fuzz seconds="60": _deps-once
    cmake -S . -B build/fuzz -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/{{ conan_dir }}/conan_toolchain.cmake" \
        -DCMAKE_BUILD_TYPE={{ config }} -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DREGA_BUILD_FUZZERS=ON -DREGA_BUILD_TESTS=OFF
    cmake --build build/fuzz
    ./scripts/fuzz.sh build/fuzz {{ seconds }}

# Regenerate the small movies under tests/data (committed)
[group('test')]
test-movies:
    ./scripts/make-test-movies.sh

# --- check -------------------------------------------------------------------

# Format all C/C++ sources in place (.clang-format)
[group('check')]
format:
    @./scripts/format.sh

# Report unformatted sources without changing anything (what CI runs)
[group('check')]
format-check:
    @./scripts/format.sh --check

# Check naming (m_, CamelCase, UPPER_CASE) with clang-tidy, in the nixpkgs `ci` shell
[group('check')]
lint:
    nix develop .#ci -c bash -c 'cmake -S . -B build/ci -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null && ./scripts/lint.sh build/ci'

# --- docs --------------------------------------------------------------------

# API docs from the headers' /// comments: build/docs/index.html
[group('docs')]
docs:
    rm -rf build/docs
    REGA_VERSION=$(git describe --tags --abbrev=0 2>/dev/null || echo dev) doxygen Doxyfile
    @echo "build/docs/index.html"

# User guide (mdBook in docs/): docs/book/index.html
[group('docs')]
guide:
    mdbook build docs
    @echo "docs/book/index.html"

# User guide with live reload: http://localhost:3000
[group('docs')]
guide-serve:
    mdbook serve docs --open

# Regenerate the guide's keys reference from the help overlay's table
[group('docs')]
guide-keys: build
    ./{{ build_dir }}/rega --help-keys > docs/src/reference/keys.md

# Record the guide's clips on this Mac (Ghostty + quickrecord): `just guide-record [name…]`
[group('docs')]
guide-record *names: build
    REGA_BIN={{ build_dir }}/rega ./scripts/guide-record.sh {{ names }}

# --- package -----------------------------------------------------------------

# Release tarball: dist/rega-<version>-<arch>.tar.gz (conan, Release)
[group('package')]
package:
    #!/usr/bin/env bash
    set -euo pipefail
    [ -f "conan-build/conan-{{ arch }}-clang/Release/conan_toolchain.cmake" ] || \
        conan install . -pr:h=./conan-profiles/conan-{{ arch }}-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
            -s build_type=Release --output-folder=conan-build/conan-{{ arch }}-clang/Release --build=missing
    cmake --preset {{ arch }}-Release
    cmake --build --preset {{ arch }}-Release --target rega
    version=$(git describe --tags --abbrev=0 2>/dev/null || echo dev)
    mkdir -p dist
    tar -czf "dist/rega-${version#v}-{{ arch }}.tar.gz" \
        -C build/{{ arch }}-Release rega -C "$PWD" LICENSE licenses
    echo "dist/rega-${version#v}-{{ arch }}.tar.gz"

# The window's download for this OS in dist/: AppImage (Linux), .dmg (macOS), zip + setup.exe (Windows)
[group('package')]
package-gui: _recipes
    just _package-gui-{{ os() }}

[private]
_release-build target="rega":
    #!/usr/bin/env bash
    set -euo pipefail
    {{ env }}true
    [ -f "conan-build/conan-{{ arch }}-clang/Release/conan_toolchain.cmake" ] || \
        {{ env }}conan install . -pr:h=./conan-profiles/conan-{{ arch }}-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
            -s build_type=Release --output-folder=conan-build/conan-{{ arch }}-clang/Release --build=missing
    {{ env }}cmake --preset {{ arch }}-Release
    {{ env }}cmake --build --preset {{ arch }}-Release --target {{ target }}

# rega-<version>-<arch>.AppImage: the nix glibc and libstdc++ the build used go in with SDL3
# and what it loads; AppRun starts rega through that loader, with --gui (quickrecord's way).
[private]
_package-gui-linux: _release-build
    #!/usr/bin/env bash
    set -euo pipefail
    : "${GLIBC_LIB_PATH:?run inside the nix devShell}"
    VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)
    export LINUXDEPLOY_OUTPUT_VERSION="${VERSION:-dev}"
    APPDIR=build/AppDir
    rm -rf "$APPDIR"
    mkdir -p "$APPDIR/usr/lib" "$APPDIR/usr/bin" "$APPDIR/usr/share/licenses/rega" dist
    # The whole glibc set (libmvec, libresolv, nss, ...), not just libc: a host's older
    # copies of the rest are picked up from /usr/lib64 and fail on GLIBC_2.xx versions.
    cp "$GLIBC_LIB_PATH"/*.so.[0-9]* "$APPDIR/usr/lib/"
    # The C++ runtime is too new for most hosts' (e.g. Rocky 9 ships GCC 11), so bundle it.
    cp "$GCC_LIB_PATH/libstdc++.so.6" "$GCC_LIB_PATH/libgcc_s.so.1" "$APPDIR/usr/lib/"
    # Read-only (as copied from the nix store): linuxdeploy would otherwise patchelf an
    # $ORIGIN rpath into ld-linux/libc, which makes the loader crash.
    chmod a-w "$APPDIR"/usr/lib/*
    cp -r LICENSE licenses/* assets/fonts/OFL.txt "$APPDIR/usr/share/licenses/rega/"
    cp assets/icons/rega.png build/rega.png
    linuxdeploy \
        --appdir "$APPDIR" \
        --executable build/linux-x64-Release/rega \
        --custom-apprun scripts/AppRun.template \
        --desktop-file scripts/rega.desktop \
        --icon-file build/rega.png \
        $APPIMAGE_LIBS_ARGS \
        --output appimage
    OUT="rega-$LINUXDEPLOY_OUTPUT_VERSION-$(uname -m).AppImage"
    mv "$OUT" dist/
    echo "dist/$OUT"

# rega.app (ad-hoc signed) in rega-<version>-macos-arm64.dmg. Opening it from Finder opens
# the window (main.cpp: inAppBundle).
[private]
_package-gui-macos: _release-build
    #!/usr/bin/env bash
    set -euo pipefail
    VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)
    VERSION="${VERSION:-dev}"
    rm -rf build/dmg build/rega.iconset
    APP=build/dmg/rega.app
    mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/licenses" build/rega.iconset dist
    cp build/macos-arm64-Release/rega "$APP/Contents/MacOS/"
    sed "s/@VERSION@/$VERSION/g" scripts/macos/Info.plist.in > "$APP/Contents/Info.plist"
    for s in 16 32 128 256 512; do
        rsvg-convert -w $s -h $s assets/icons/rega.svg -o build/rega.iconset/icon_${s}x${s}.png
        rsvg-convert -w $((2 * s)) -h $((2 * s)) assets/icons/rega.svg -o build/rega.iconset/icon_${s}x${s}@2x.png
    done
    iconutil -c icns build/rega.iconset -o "$APP/Contents/Resources/rega.icns"
    cp -r LICENSE licenses/* assets/fonts/OFL.txt "$APP/Contents/Resources/licenses/"
    codesign --force --deep -s - "$APP"
    ln -s /Applications build/dmg/Applications
    OUT="dist/rega-$VERSION-macos-arm64.dmg"
    rm -f "$OUT"
    hdiutil create -volname rega -srcfolder build/dmg -ov -format UDZO "$OUT"
    echo "$OUT"

[private]
_package-gui-windows: (_release-build "rega")
    just _package-windows-from build/windows-x64-Release/rega.exe

# The Windows zip and setup.exe in dist/ from a built rega.exe (e.g. after `just windows-check`)
[group('package')]
package-windows exe="build/windows-x64-Release/rega.exe": _recipes
    just _package-windows-from {{ exe }}

[private]
_package-windows-from exe:
    #!/usr/bin/env bash
    set -euo pipefail
    {{ env }}true
    VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)
    VERSION="${VERSION:-dev}"
    DIR=build/zip/rega
    rm -rf build/zip
    mkdir -p "$DIR/licenses" dist
    cp "{{ exe }}" "$DIR/rega.exe"
    # The symbols are half its size.
    STRIP=$(command -v llvm-strip || command -v x86_64-w64-mingw32-strip || echo .sdk/llvm-mingw/bin/llvm-strip)
    "$STRIP" "$DIR/rega.exe"
    cp -r LICENSE licenses/* assets/fonts/OFL.txt "$DIR/licenses/"
    OUT="dist/rega-$VERSION-windows-x64.zip"
    rm -f "$OUT"
    (cd build/zip && "$(command -v python3 || command -v python)" -m zipfile -c "../../$OUT" rega)
    echo "$OUT"
    just windows-installer "$DIR" "$VERSION"

# The per-user Windows setup.exe in dist/ from a packaged rega/ folder (makensis runs anywhere)
[group('package')]
windows-installer dir version="dev":
    #!/usr/bin/env bash
    set -euo pipefail
    command -v makensis >/dev/null || { echo "makensis not found: the nix devShell has it (or choco install nsis)"; exit 1; }
    VERSION="{{ version }}"
    # makensis wants four numbers here; anything but X.Y.Z (e.g. dev) gets 0.0.0.0.
    if [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then VI_VERSION="$VERSION.0"; else VI_VERSION=0.0.0.0; fi
    # Absolute and native (C:\… on Windows): makensis runs from the script's directory.
    native() { (cd "$1" && (cygpath -w "$PWD" 2>/dev/null || pwd)); }
    mkdir -p dist
    SRC=$(native "{{ dir }}")
    OUT="$(native dist)/rega-$VERSION-windows-x64-setup.exe"
    [ -f "$SRC/rega.exe" ] || { echo "No rega.exe in $SRC"; exit 1; }
    makensis -V2 -DVERSION="$VERSION" -DVI_VERSION="$VI_VERSION" -DSRC_DIR="$SRC" -DOUT_FILE="$OUT" \
        scripts/windows-installer.nsi
    echo "dist/$(basename "$OUT")"

# Cross-compile rega.exe and its tests for Windows here (llvm-mingw into .sdk/), before a release
[group('package')]
windows-check: _recipes
    #!/usr/bin/env bash
    set -euo pipefail
    source scripts/windows-env.sh
    out=conan-build/conan-windows-x64-clang/Release
    [ -f "$out/conan_toolchain.cmake" ] || \
        conan install . -pr:h=./conan-profiles/conan-windows-x64-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
            -s build_type=Release --output-folder="$out" --build=missing
    cmake --preset windows-x64-Release
    cmake --build --preset windows-x64-Release

# Run the cross-built Windows tests under Wine (Linux; after `just windows-check`)
[group('package')]
windows-test:
    nix shell nixpkgs#wineWow64Packages.stable nixpkgs#xvfb-run -c \
        xvfb-run -a wine build/windows-x64-Release/rega_tests.exe --gtest_brief=1

# --- clean -------------------------------------------------------------------

# Remove build trees (conan deps cache in ~/.conan2 is kept)
[group('clean')]
clean:
    rm -rf build dist compile_commands.json

# Remove everything, including conan-build/
[group('clean')]
clean-all: clean
    rm -rf conan-build

# --- release -----------------------------------------------------------------

# Tag a release and push it (triggers the CI build on the v* tag).
# With no argument, auto-bumps the patch of the latest v* tag (v0.0.7 → v0.0.8).
#
# This recipe is IDENTICAL across every one of the user's projects — copy it verbatim
# into any new project's .justfile. Pass an explicit tag for minor/major bumps:
#   just release           # auto patch bump
#   just release v1.0.0    # explicit
[group('release')]
release tag="":
    #!/usr/bin/env bash
    set -euo pipefail
    tag="{{tag}}"
    if [ -z "$tag" ]; then
        last=$(git tag --list 'v*' --sort=-v:refname | head -1)
        last="${last:-v0.0.0}"
        IFS=. read -r major minor patch <<< "${last#v}"
        tag="v${major}.${minor}.$((patch + 1))"
        echo "Auto-bumping: ${last} → ${tag}"
    fi
    git tag "$tag"
    git push origin "$tag"
