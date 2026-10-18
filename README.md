# rvtui

Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), PNG and JPEG.
Draws images with the kitty graphics protocol, or half-blocks on any terminal.

## Demo

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

### Home Manager (flake)

```nix
# flake.nix
inputs = {
  rvtui.url = "github:Elyg/rvtui";   # latest main, or "github:Elyg/rvtui/v0.0.15" to pin a release
  rvtui.inputs.nixpkgs.follows = "nixpkgs";
};

# home.nix (pass `inputs` through extraSpecialArgs)
home.packages = [ inputs.rvtui.packages.${pkgs.stdenv.hostPlatform.system}.default ];

# or with the overlay:
nixpkgs.overlays = [ inputs.rvtui.overlays.default ];
home.packages = [ pkgs.rvtui ];
```

There is no binary cache yet, so the first install builds rvtui from source (dependencies
come from the nixpkgs cache).

### Nix, without Home Manager

```bash
nix run github:Elyg/rvtui -- shots/
nix profile install github:Elyg/rvtui
```

### Linux

Each [GitHub Release](https://github.com/Elyg/rvtui/releases) has a fully static (musl)
binary that runs on any distro with no dependencies. Two architectures:

| Arch            | Tarball                                |
| --------------- | -------------------------------------- |
| x86_64          | `rvtui-<version>-linux-x86_64.tar.gz`  |
| aarch64 (ARM64) | `rvtui-<version>-linux-aarch64.tar.gz` |

```bash
v=$(curl -sLo /dev/null -w '%{url_effective}' https://github.com/Elyg/rvtui/releases/latest)
v=${v##*/v}
mkdir -p ~/.local/bin
arch=$(uname -m)
curl -L "https://github.com/Elyg/rvtui/releases/download/v$v/rvtui-$v-linux-$arch.tar.gz" \
  | tar -xz -C ~/.local/bin rvtui
```

This installs the latest release for your architecture into `~/.local/bin`, which needs
to be on your `PATH`.

On NixOS, or anywhere with Nix, use the flake instead (above): it builds against nixpkgs
(glibc) rather than using the static binary.

### macOS

Apple Silicon only: `rvtui-<version>-macos-arm64.tar.gz` from the same release page. It
links only against system libraries.

```bash
v=$(curl -sLo /dev/null -w '%{url_effective}' https://github.com/Elyg/rvtui/releases/latest)
v=${v##*/v}
mkdir -p ~/.local/bin
curl -L "https://github.com/Elyg/rvtui/releases/download/v$v/rvtui-$v-macos-arm64.tar.gz" \
  | tar -xz -C ~/.local/bin rvtui
```

This installs the latest release into `~/.local/bin`, which needs to be on your `PATH`.

## Setup

What rvtui needs, by what sits between the terminal and rvtui:

**✅ Just works**

terminal: Ghostty / kitty

```
┌──────────┐
│ terminal │──────────────▶ rvtui
└──────────┘

┌──────────┐    ┌─────┐
│ terminal │───▶│ ssh │───▶ rvtui
└──────────┘    └─────┘
```

**⚙️ One line in tmux.conf**

```
┌──────────┐    ┌──────┐
│ terminal │───▶│ tmux │───▶ rvtui
└──────────┘    └──┬───┘
                   └─ tmux.conf:
                        set -g allow-passthrough on

┌──────────┐    ┌─────┐    ┌──────┐
│ terminal │───▶│ ssh │───▶│ tmux │───▶ rvtui
└──────────┘    └─────┘    └──┬───┘
                              └─ remote tmux.conf:
                                   set -g allow-passthrough on
```

**⚙️ tmux, then ssh:** rvtui on the remote can't see the terminal through tmux, so tell
it with `--graphics kitty`.

```
┌──────────┐    ┌──────┐    ┌─────┐
│ terminal │───▶│ tmux │───▶│ ssh │───▶ rvtui --graphics kitty
└──────────┘    └──┬───┘    └─────┘
                   └─ local tmux.conf:
                        set -g allow-passthrough on

┌──────────┐    ┌──────┐    ┌─────┐    ┌──────┐
│ terminal │───▶│ tmux │───▶│ ssh │───▶│ tmux │───▶ rvtui --graphics kitty
└──────────┘    └──┬───┘    └─────┘    └──┬───┘
                   │                      └─ remote tmux.conf:
                   │                           set -g allow-passthrough on
                   └─ local tmux.conf:
                        set -g allow-passthrough on
```

**▦ Other**

```
┌──────────┐    ┌────────┐
│ terminal │───▶│ zellij │───▶ rvtui     zellij can't show rvtui's images
└──────────┘    └────────┘

┌───────────────────────────┐
│ iTerm2, WezTerm, Warp,    │
│ Konsole, Alacritty, foot, │
│ GNOME Terminal, xterm,    │───▶ rvtui     these terminals can't show
│ Windows Terminal,         │               rvtui's full-resolution images
│ Terminal.app, VS Code, …  │               (half-blocks only)
└───────────────────────────┘
```

Whatever is missing, rvtui falls back to half-blocks, except with `--graphics kitty`,
which trusts you: if a line above is missing, images stay blank. **`rvtui --doctor`**
says which case you are in and what to add.

### tmux.conf

In every tmux between the terminal and rvtui:

- `set -g allow-passthrough on`: needed for images
- `set -as terminal-features ',xterm*:RGB'`: true colour (recommended)
- `set -g set-clipboard on`: `y` copies reach the clipboard (recommended)

## Colour

With `$OCIO` set, rvtui shows images through that OpenColorIO config: pick the
display, view and look, and override an image's input colour space (otherwise the
config's file rules decide), in the colour pane (`6`). Without `$OCIO` it shows
plain sRGB; the pane can switch to OCIO's built-in ACES configs, and remembers the
choice. `s` toggles the view transform off (raw values).

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
