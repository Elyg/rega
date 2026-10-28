# Install

> [!TIP]
> **TL;DR** `curl -fsSL https://raw.githubusercontent.com/Elyg/rega/main/scripts/install.sh | bash`
> puts `rega` in `~/.local/bin`, on your machine or on a remote one.

rega runs where the renders are. For renders on your own machine, install it there. For
renders on a remote machine, ssh in and run the install line there too.

## Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rega/main/scripts/install.sh | bash
```

The script downloads the latest [release](https://github.com/Elyg/rega/releases):

- **Linux** (x86_64, aarch64): a fully static binary. It runs on any distribution and
  needs no libraries.
- **macOS**: the Apple Silicon build.

To pin a release or pick another folder, set these for `bash`:

```bash
curl -fsSL https://raw.githubusercontent.com/Elyg/rega/main/scripts/install.sh \
  | REGA_VERSION=0.0.16 REGA_BIN_DIR=~/bin bash
```

Run the same line again to update.

## Nix

```bash
nix run github:Elyg/rega -- shots/       # try it
nix profile install github:Elyg/rega    # install it
```

### Home Manager (flake)

```nix
# flake.nix
inputs = {
  rega.url = "github:Elyg/rega";   # latest main, or "github:Elyg/rega/v0.0.16" to pin
  rega.inputs.nixpkgs.follows = "nixpkgs";
};

# home.nix (pass `inputs` through extraSpecialArgs)
home.packages = [ inputs.rega.packages.${pkgs.stdenv.hostPlatform.system}.default ];

# or with the overlay:
nixpkgs.overlays = [ inputs.rega.overlays.default ];
home.packages = [ pkgs.rega ];
```

> [!NOTE]
> There is no binary cache yet, so the first Nix install builds rega from source. Its
> dependencies come from the nixpkgs cache.

## Check it

```bash
rega --version
rega --doctor     # can this terminal show images? See Terminal setup
```

If `rega --version` says "command not found", `~/.local/bin` isn't on your `PATH`. Add
`export PATH="$HOME/.local/bin:$PATH"` to `~/.bashrc` (or `~/.zshrc`) and open a new
terminal.

## Build from source

See [Build from source](https://github.com/Elyg/rega#build-from-source) in the README:
a Nix dev shell provides everything, then `just build`.
