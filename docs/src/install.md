# Install

> [!TIP]
> **TL;DR** `curl -fsSL https://raw.githubusercontent.com/Elyg/rvtui/main/scripts/install.sh | bash`
> puts `rvtui` in `~/.local/bin`, on your machine or on a remote one.

rvtui runs where the renders are. For renders on your own machine, install it there. For
renders on a remote machine, ssh in and run the install line there too.

## Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rvtui/main/scripts/install.sh | bash
```

The script downloads the latest [release](https://github.com/Elyg/rvtui/releases):

- **Linux** (x86_64, aarch64): a fully static binary. It runs on any distribution and
  needs no libraries.
- **macOS**: the Apple Silicon build.

To pin a release or pick another folder, set these for `bash`:

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rvtui/main/scripts/install.sh \
  | RVTUI_VERSION=0.0.16 RVTUI_BIN_DIR=~/bin bash
```

Run the same line again to update.

## Nix

```bash
nix run github:Elyg/rvtui -- shots/       # try it
nix profile install github:Elyg/rvtui    # install it
```

### Home Manager (flake)

```nix
# flake.nix
inputs = {
  rvtui.url = "github:Elyg/rvtui";   # latest main, or "github:Elyg/rvtui/v0.0.16" to pin
  rvtui.inputs.nixpkgs.follows = "nixpkgs";
};

# home.nix (pass `inputs` through extraSpecialArgs)
home.packages = [ inputs.rvtui.packages.${pkgs.stdenv.hostPlatform.system}.default ];

# or with the overlay:
nixpkgs.overlays = [ inputs.rvtui.overlays.default ];
home.packages = [ pkgs.rvtui ];
```

> [!NOTE]
> There is no binary cache yet, so the first Nix install builds rvtui from source. Its
> dependencies come from the nixpkgs cache.

## Check it

```bash
rvtui --version
rvtui --doctor     # can this terminal show images? See Terminal setup
```

If `rvtui --version` says "command not found", `~/.local/bin` isn't on your `PATH`. Add
`export PATH="$HOME/.local/bin:$PATH"` to `~/.bashrc` (or `~/.zshrc`) and open a new
terminal.

## Build from source

See [Build from source](https://github.com/Elyg/rvtui#build-from-source) in the README:
a Nix dev shell provides everything, then `just build`.
