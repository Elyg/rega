# rvtui — command interface. `just` with no arguments lists everything.
#
# One active build config at a time, so no recipe takes it as an argument.
# Change it with `just set-config Release`; everything else follows.

set shell := ["bash", "-c"]

# Apple Silicon on macOS, x64 elsewhere (presets: macos-arm64-*, linux-x64-*).
arch := if os() == "macos" { "macos-arm64" } else { "linux-x64" }

# The active config: whatever `just set-config` last wrote, else $RVTUI_CONFIG, else Release.
state_file := ".rvtui-config"
config := if path_exists(state_file) == "true" { trim(`cat .rvtui-config`) } else { env('RVTUI_CONFIG', 'Release') }

preset := arch + "-" + config
build_dir := "build" / preset
conan_dir := "conan-build" / "conan-" + arch + "-clang" / config

# `just` and `just help` both print this.
alias help := default

# List recipes, grouped
default:
    @just --list --unsorted --list-heading $'\nrvtui — building {{ config }} for {{ arch }}\n'

# --- build -------------------------------------------------------------------

# Install conan dependencies for the active config
[group('build')]
deps:
    conan install . -pr:h=./conan-profiles/conan-{{ arch }}-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
        -s build_type={{ config }} --output-folder={{ conan_dir }} --build=missing

# Configure the build tree (re-run after adding files or changing presets)
[group('build')]
setup: _deps-once
    cmake --preset {{ preset }}
    ln -sf {{ build_dir }}/compile_commands.json compile_commands.json

# Deps and configure only when missing, so plain `just build` works on a fresh clone.
[private]
_deps-once:
    @{{ if path_exists(conan_dir / "conan_toolchain.cmake") == "true" { "" } else { "just deps" } }}

[private]
configure: _deps-once
    @{{ if path_exists(build_dir / "CMakeCache.txt") == "true" { "" } else { "just setup" } }}

# Build rvtui and the tests
[group('build')]
build: configure
    cmake --build --preset {{ preset }}

# --- config ------------------------------------------------------------------

# Switch the active config: `just set-config Debug` (Debug | Release; Release is the default)
[group('config')]
set-config value:
    @[[ "{{ value }}" == "Debug" || "{{ value }}" == "Release" ]] || { echo "Debug or Release" >&2; exit 1; }
    @echo "{{ value }}" > {{ state_file }}
    @echo "active config: {{ value }}"

# --- run ---------------------------------------------------------------------

# Run rvtui: `just run`, `just run demos/`, `just run --dump foo.exr`
[group('run')]
run *args: build
    ./{{ build_dir }}/rvtui {{ args }}

# Download the OpenEXR test images into demos/ (gitignored)
[group('run')]
fetch-demos:
    @[ -d demos/openexr-images ] || git clone --depth 1 https://github.com/AcademySoftwareFoundation/openexr-images.git demos/openexr-images
    @echo "demo images in demos/openexr-images"

# Render the multi-layer demo sequence into demos/rvtui-sequence/ (numpy ray tracer)
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
demo-buckets source="demos/rvtui-sequence/shot.1001.exr": configure
    cmake --build --preset {{ preset }} --target rvtui_buckets
    ./{{ build_dir }}/rvtui_buckets {{ source }} demos/buckets

# Browse the demo images: `just demo`
[group('run')]
demo: fetch-demos build
    ./{{ build_dir }}/rvtui demos/openexr-images

# --- test --------------------------------------------------------------------

# Build and run the unit tests
[group('test')]
test: build
    ctest --preset {{ preset }}

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
    RVTUI_VERSION=$(git describe --tags --abbrev=0 2>/dev/null || echo dev) doxygen Doxyfile
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
    ./{{ build_dir }}/rvtui --help-keys > docs/src/reference/keys.md

# Record the guide's clips on this Mac (Ghostty + quickrecord): `just guide-record [name…]`
[group('docs')]
guide-record *names: build
    RVTUI_BIN={{ build_dir }}/rvtui ./scripts/guide-record.sh {{ names }}

# --- package -----------------------------------------------------------------

# Release tarball: dist/rvtui-<version>-<arch>.tar.gz (conan, Release)
[group('package')]
package:
    #!/usr/bin/env bash
    set -euo pipefail
    [ -f "conan-build/conan-{{ arch }}-clang/Release/conan_toolchain.cmake" ] || \
        conan install . -pr:h=./conan-profiles/conan-{{ arch }}-clang -pr:b=./conan-profiles/conan-{{ arch }}-clang \
            -s build_type=Release --output-folder=conan-build/conan-{{ arch }}-clang/Release --build=missing
    cmake --preset {{ arch }}-Release
    cmake --build --preset {{ arch }}-Release --target rvtui
    version=$(git describe --tags --abbrev=0 2>/dev/null || echo dev)
    mkdir -p dist
    tar -C build/{{ arch }}-Release -czf "dist/rvtui-${version#v}-{{ arch }}.tar.gz" rvtui
    echo "dist/rvtui-${version#v}-{{ arch }}.tar.gz"

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
