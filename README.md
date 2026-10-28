# rega

*Rega* means sights. Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), TIFF, PNG and JPEG.
Draws images with the kitty graphics protocol, or half-blocks on any terminal.

<h2 align="center"><a href="https://elyg.github.io/rega">📖 User guide</a></h2>
<p align="center">Features with a short clip · terminal and tmux setup · colour (OCIO, ACES) · keybinds</p>

## Demo

https://github.com/user-attachments/assets/e3305c5d-d0ce-436d-8682-f4f18394bde8

## Usage

```bash
rega                      # browse the current directory
rega shots/               # browse a directory
rega 'shot.####.exr' ...  # view sequences
rega 'shot.#.exr'         # a lone # finds the padding itself
rega --diff old.exr new.exr ...    # A, B and |A - B| side by side; n / p: next pair
rega a.exr:label=top:main ...      # a label on the image (tl tc tr bl bc br, top, bottom)
rega --dump foo.exr       # print layers, channels and metadata
rega --doctor             # check the terminal / tmux setup
rega --tutorial           # a guided tour on sample images (rendered into ~/.cache/rega/tutorial)
```

In the browser, mark two images (space) and press `d` to diff them; with
more marked, they pair up in marking order (1st with 2nd, 3rd with 4th, …).

## Supported terminals

- **[Ghostty](https://ghostty.org), [kitty](https://sw.kovidgoyal.net/kitty/):**
  full-resolution images.
- **Any other terminal:** ▦ half-blocks, a coarser fallback where each character cell
  shows 2 pixels.

rega detects which one to use. To choose yourself: `--graphics kitty` or
`--graphics halfblock`.

## Install

### Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rega/main/scripts/install.sh | bash
```

[scripts/install.sh](scripts/install.sh) puts the latest
[release](https://github.com/Elyg/rega/releases) into `~/.local/bin`: a fully static
binary on Linux (x86_64, aarch64; any distro, no dependencies), or the Apple Silicon build
on macOS. To pin a release or pick another folder, set them for `bash`:
`… | REGA_VERSION=0.0.15 REGA_BIN_DIR=~/bin bash`. On NixOS, use the flake instead
(below).

Nix, Home Manager and pinning a release: [Install](https://elyg.github.io/rega/install.html).

## Setup

Ghostty and kitty just work, directly or over ssh. In tmux, add
`set -g allow-passthrough on`. **`rega --doctor`** says what's missing. Every case
(tmux before or after ssh, other terminals): [Terminal setup](https://elyg.github.io/rega/setup.html).

## Learn more

`rega --tutorial` walks you through sample images. The [user guide](https://elyg.github.io/rega) covers the
[browser](https://elyg.github.io/rega/browser.html), the [viewer](https://elyg.github.io/rega/viewer/index.html), its
[panes](https://elyg.github.io/rega/panes/index.html), [colour (OCIO)](https://elyg.github.io/rega/panes/colour.html),
[annotations](https://elyg.github.io/rega/panes/annotations.html) and [every key](https://elyg.github.io/rega/reference/keys.html).

## Build from source

Prerequisites:

- [Nix](https://nixos.org/download) with flakes enabled. Its dev shell provides `just`,
  cmake, ninja, conan and clang-format/clang-tidy, so there's nothing else to install.
- [direnv](https://direnv.net) (optional): enters the dev shell automatically on `cd`
  (`direnv allow` once). Without it, run `nix develop`.
- macOS: Xcode Command Line Tools (`xcode-select --install`); the build uses Apple clang.

Then:

```bash
just build        # conan deps + configure + build
just run demos/   # build and run
just test         # unit tests
just              # list every recipe
```

## Development

Developed with assistance from [Claude Code](https://claude.com/claude-code). All changes
are reviewed, tested, and maintained by me.
