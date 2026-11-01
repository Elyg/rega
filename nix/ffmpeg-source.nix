# FFmpeg's release tarball, unpatched: what the Nix build compiles, the same file as
# conanfile.py's ffmpeg/9.0.1, and what each GitHub release attaches for the LGPL.
# Changing it means changing conanfile.py and licenses/FFMPEG-BUILD.txt too.
{ fetchurl }:

fetchurl rec {
  url = "https://ffmpeg.org/releases/ffmpeg-${passthru.version}.tar.xz";
  hash = "sha256-zzjg4ox+VgWULEp3dVNJsBRYBKOXrzfrH7THfLI39jU=";
  passthru.version = "9.0.1";
}
