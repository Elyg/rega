# FFmpeg's release tarball, unpatched: what the Nix build compiles, the same file as
# conanfile.py's ffmpeg/9.0.2, and what each GitHub release attaches for the LGPL.
# Changing it means changing conanfile.py and licenses/FFMPEG-BUILD.txt too.
{ fetchurl }:

fetchurl rec {
  url = "https://ffmpeg.org/releases/ffmpeg-${passthru.version}.tar.xz";
  hash = "sha256-jDhQKD6yX6AmSCB4oEBR4L4XNHsJ74GghJvsFaluAC4=";
  passthru.version = "9.0.2";
}
