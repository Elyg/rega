# rvtui

Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), TIFF, PNG and JPEG.
Draws images with the kitty graphics protocol, or half-blocks on any terminal.

<h2 align="center"><a href="https://elyg.github.io/rvtui">📖 User guide</a></h2>
<p align="center">Every feature with a short clip · terminal and tmux setup · colour (OCIO, ACES) · every key</p>

## Demo

### **Overview** ###
Browsing with live previews, opening and playing a sequence, its layers one by one and
all tiled, the metadata. The images are from `rvtui --tutorial`.

![rvtui overview](docs/src/media/intro.mp4)

### **Viewer** ###
Playing a multi-layer EXR sequence, switching layers (AOVs), every layer
side by side (`t`), metadata, the pixel inspector, exposure and single channels.

https://github.com/user-attachments/assets/ba04978b-8df7-4eb1-9357-f6db4f5c5835



### **Browser** ###
Live previews while moving through a folder, then marked images opened
together in the tile view.

https://github.com/user-attachments/assets/bd076e40-9d24-4ed3-b529-719efcaca671



Recorded in Ghostty through tmux. The sample images are the
[OpenEXR test images](https://github.com/AcademySoftwareFoundation/openexr-images)

## Usage

```bash
rvtui                      # browse the current directory
rvtui shots/               # browse a directory
rvtui 'shot.####.exr' ...  # view sequences
rvtui 'shot.#.exr'         # a lone # finds the padding itself
rvtui --diff old.exr new.exr ...    # A, B and |A - B| side by side; n / p: next pair
rvtui a.exr:label=top:main ...      # a label on the image (tl tc tr bl bc br, top, bottom)
rvtui --dump foo.exr       # print layers, channels and metadata
rvtui --doctor             # check the terminal / tmux setup
rvtui --tutorial           # a guided tour on sample images (rendered into ~/.cache/rvtui/tutorial)
```

In the browser, mark two images (space) and press `d` to diff them; with
more marked, they pair up in marking order (1st with 2nd, 3rd with 4th, …).

## Supported terminals

- **[Ghostty](https://ghostty.org), [kitty](https://sw.kovidgoyal.net/kitty/):**
  full-resolution images.
- **Any other terminal:** ▦ half-blocks, a coarser fallback where each character cell
  shows 2 pixels.

rvtui detects which one to use. To choose yourself: `--graphics kitty` or
`--graphics halfblock`.

## Install

### Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rvtui/main/scripts/install.sh | bash
```

[scripts/install.sh](scripts/install.sh) puts the latest
[release](https://github.com/Elyg/rvtui/releases) into `~/.local/bin`: a fully static
binary on Linux (x86_64, aarch64; any distro, no dependencies), or the Apple Silicon build
on macOS. To pin a release or pick another folder, set them for `bash`:
`… | RVTUI_VERSION=0.0.15 RVTUI_BIN_DIR=~/bin bash`. On NixOS, use the flake instead
(below).

Nix, Home Manager and pinning a release: [Install](https://elyg.github.io/rvtui/install.html).

## Setup

Ghostty and kitty just work, directly or over ssh. In tmux, add
`set -g allow-passthrough on`. **`rvtui --doctor`** says what's missing. Every case
(tmux before or after ssh, other terminals): [Terminal setup](https://elyg.github.io/rvtui/setup.html).

## Learn more

`rvtui --tutorial` walks you through sample images. The [user guide](https://elyg.github.io/rvtui) covers the
[browser](https://elyg.github.io/rvtui/browser.html), the [viewer](https://elyg.github.io/rvtui/viewer/index.html), its
[panes](https://elyg.github.io/rvtui/panes/index.html), [colour (OCIO)](https://elyg.github.io/rvtui/panes/colour.html),
[annotations](https://elyg.github.io/rvtui/panes/annotations.html) and [every key](https://elyg.github.io/rvtui/reference/keys.html).

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
