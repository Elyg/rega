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

```sh
rvtui                      # browse the current directory
rvtui shots/               # browse a directory
rvtui 'shot.####.exr' ...  # view sequences
rvtui 'shot.#.exr'         # a lone # finds the padding itself
rvtui --dump foo.exr       # print layers, channels and metadata
rvtui --doctor             # check the terminal / tmux setup
rvtui --tutorial           # a guided tour on sample images (rendered into ~/.cache/rvtui/tutorial)
```

## Supported terminals

| Terminal | Images |
| --- | --- |
| [Ghostty](https://ghostty.org), [kitty](https://sw.kovidgoyal.net/kitty/) | Full resolution (kitty graphics protocol with Unicode placeholders) |
| Anything else (iTerm2, WezTerm, Alacritty, Terminal.app, …) | Half-block characters: lower resolution, works everywhere with true colour |

rvtui picks the mode by itself; `--graphics kitty|halfblock` overrides it. WezTerm and
Konsole implement kitty graphics but not the Unicode placeholders rvtui draws with, so
they get half-blocks.

- **tmux:** works when the outer terminal is Ghostty or kitty and passthrough is on:
  `set -g allow-passthrough on`. Without it rvtui falls back to half-blocks. Also
  recommended: `set -as terminal-features ',xterm*:RGB'` (true colour) and
  `set -g set-clipboard on` (so `y` copies reach the system clipboard).
- **ssh:** works; images are sent inline over the connection (`--transfer direct`)
  instead of through shared memory or temp files.
- **`rvtui --doctor`** checks the terminal, tmux and clipboard setup and says what to
  change.

## Colour

With `$OCIO` set, rvtui shows images through that OpenColorIO config: pick the
display, view and look, and override an image's input colour space (otherwise the
config's file rules decide), in the colour pane (`6`). Without `$OCIO` it shows
plain sRGB; the pane can switch to OCIO's built-in ACES configs, and remembers the
choice. `s` toggles the view transform off (raw values).

## Install

### Home Manager (flake)

```nix
# flake.nix
inputs = {
  rvtui.url = "github:Elyg/rvtui/v0.0.2";   # or "github:Elyg/rvtui" for latest main
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

```sh
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

```sh
v=0.0.2; arch=$(uname -m)   # x86_64 or aarch64
curl -L "https://github.com/Elyg/rvtui/releases/download/v$v/rvtui-$v-linux-$arch.tar.gz" \
  | tar -xz -C ~/.local/bin rvtui
```

On NixOS, or anywhere with Nix, use the flake instead (above): it builds against nixpkgs
(glibc) rather than using the static binary.

### macOS

Apple Silicon only: `rvtui-<version>-macos-arm64.tar.gz` from the same release page. It
links only against system libraries.

```sh
v=0.0.2
curl -L "https://github.com/Elyg/rvtui/releases/download/v$v/rvtui-$v-macos-arm64.tar.gz" \
  | tar -xz -C ~/.local/bin rvtui
```

## Build from source

Prerequisites:

- [Nix](https://nixos.org/download) with flakes enabled. Its dev shell provides `just`,
  cmake, ninja, conan and clang-format/clang-tidy, so there's nothing else to install.
- [direnv](https://direnv.net) (optional): enters the dev shell automatically on `cd`
  (`direnv allow` once). Without it, run `nix develop`.
- macOS: Xcode Command Line Tools (`xcode-select --install`); the build uses Apple clang.

Then:

```sh
just build        # conan deps + configure + build
just run demos/   # build and run
just test         # unit tests
just              # list every recipe
```

## Development

Developed with assistance from [Claude Code](https://claude.com/claude-code). All changes
are reviewed, tested, and maintained by me.
