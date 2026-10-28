{
  description = "rega — terminal image viewer (C++20 + FTXUI + OpenEXR)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    let
      # Nix builds have no tags; identify them by commit instead. A release passes its tag
      # in: REGA_VERSION=0.1.0 nix build --impure .#static (getEnv is "" when pure).
      version =
        let
          tag = builtins.getEnv "REGA_VERSION";
        in
        if tag != "" then tag else "git-${self.shortRev or self.dirtyShortRev or "dev"}";

      # pkgsStatic libraries aren't in the binary cache, so `.#static` compiles them from
      # source; skip their test suites (rega's own still run). Non-static packages are
      # left untouched so they keep coming from the cache.
      skipStaticDepTests =
        final: prev:
        let
          noTests =
            name: extra:
            if prev.stdenv.hostPlatform.isStatic then
              prev.${name}.overrideAttrs (old: { doCheck = false; } // extra old)
            else
              prev.${name};
          flags = fs: old: { cmakeFlags = (old.cmakeFlags or [ ]) ++ fs; };
          none = _: { };
          # catch2 is only there for the tests. The static stdenv moves buildInputs into
          # propagatedBuildInputs, so drop it from both.
          noCatch2 =
            old:
            let
              keep = builtins.filter (p: (p.pname or "") != "catch2");
            in
            {
              buildInputs = keep (old.buildInputs or [ ]);
              propagatedBuildInputs = keep (old.propagatedBuildInputs or [ ]);
            };
        in
        {
          fmt = noTests "fmt" (flags [ "-DFMT_TEST=OFF" ]);
          spdlog = noTests "spdlog" (old: flags [ "-DSPDLOG_BUILD_TESTS=OFF" ] old // noCatch2 old);
          cli11 = noTests "cli11" (old: flags [ "-DCLI11_BUILD_TESTS=OFF" ] old // noCatch2 old);
          libdeflate = noTests "libdeflate" (flags [ "-DLIBDEFLATE_BUILD_TESTS=OFF" ]);
          imath = noTests "imath" (flags [ "-DBUILD_TESTING=OFF" ]);
          openexr = noTests "openexr" (flags [ "-DBUILD_TESTING=OFF" ]);
          # These derive their test build flags from doCheck.
          ftxui = noTests "ftxui" none;
          yaml-cpp = noTests "yaml-cpp" none;
          minizip-ng = noTests "minizip-ng" none;
          pystring = noTests "pystring" none;
        };
    in
    {
      overlays.default = final: prev: {
        rega = final.callPackage ./nix/package.nix { inherit version; };
      };
    }
    // flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ skipStaticDepTests ];
        };
        hostLlvm = pkgs.llvmPackages_20;

        tools = with pkgs; [
          cmake
          ninja
          just
          conan
          git
          hostLlvm.clang-tools # clang-format / clang-tidy
          doxygen # just docs
        ];

        # The user guide (docs/): just guide, and the Pages workflow.
        guideTools = with pkgs; [
          mdbook
          mdbook-mermaid
        ];

        # Day to day: also the guide, and ffmpeg for just guide-record.
        devTools = tools ++ guideTools ++ [ pkgs.ffmpeg ];

        commonSetup = ''
          export SHELL_TYPE="host"
          export CONAN_HOME="''${CONAN_HOME:-$HOME/.conan2}"
        '';
        rega = pkgs.callPackage ./nix/package.nix { inherit version; };
      in
      {
        packages =
          {
            default = rega;
            rega = rega;
          }
          // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            # Fully static musl binary: copy anywhere, no runtime deps.
            static = pkgs.pkgsStatic.callPackage ./nix/package.nix { inherit version; };
          };

        apps.default = flake-utils.lib.mkApp { drv = rega; };

        # Deps from nixpkgs instead of conan: what CI and `nix build` use. clang-tidy parses
        # this tree cleanly (on macOS the Apple-clang compile DB trips nix clang-tidy).
        devShells.ci = pkgs.mkShell {
          inputsFrom = [ rega ];
          packages = tools ++ [ pkgs.gtest ];
        };

        devShells.guide = pkgs.mkShellNoCC { packages = guideTools; };

        devShells.default =
          if pkgs.stdenv.hostPlatform.isDarwin then
            # On macOS build with Apple's clang (Xcode CLT) so binaries link against the system SDK.
            pkgs.mkShellNoCC {
              packages = devTools;
              shellHook = commonSetup + ''
                export CC=/usr/bin/clang
                export CXX=/usr/bin/clang++
                unset SDKROOT DEVELOPER_DIR
              '';
            }
          else
            (pkgs.mkShell.override { stdenv = hostLlvm.stdenv; }) {
              packages = devTools;
              shellHook = commonSetup + ''
                export CC=clang
                export CXX=clang++
              '';
            };
      }
    );
}
