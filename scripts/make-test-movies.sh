#!/usr/bin/env bash
# Regenerate the small movies under tests/data (committed; needs a full ffmpeg, which
# the dev shell has). Each ramp frame is one grey level, 16 + 4 * frame in the 8-bit
# video range, so a test can tell from its pixels exactly which frame it was given.
set -euo pipefail

out="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tests/data"
mkdir -p "$out"
ff() { ffmpeg -v error -y -bitexact "$@"; }

ramp=(-f lavfi -i "color=black:s=64x32:r=24:d=2,format=yuv420p,geq=lum='16+N*4':cb=128:cr=128")
x264=(-c:v libx264 -qp 1 -pix_fmt yuv420p -map_metadata -1)
closed="keyint=12:min-keyint=12:scenecut=0:bframes=2:open-gop=0"

# Long GOP with B-frames: a seek must decode forward from a keyframe.
ff "${ramp[@]}" "${x264[@]}" -x264-params "$closed" -movflags +faststart "$out/gop.mp4"
# Open GOP: frames just before a keyframe in time come after it in the file.
ff "${ramp[@]}" "${x264[@]}" -x264-params "keyint=12:min-keyint=12:scenecut=0:bframes=3:open-gop=1" \
    "$out/open_gop.mp4"
# Fragmented: no sample table in the moov, the index is spread through the file.
ff "${ramp[@]}" "${x264[@]}" -x264-params "$closed" \
    -movflags frag_keyframe+empty_moov+default_base_moof "$out/fragmented.mp4"
# Cut with an edit list: frames 12-14 are in the file but not in the movie.
ff -seek_timestamp 1 -ss 0.625 -i "$out/gop.mp4" -c copy "$out/trimmed.mov"
# Variable frame rate, with B-frames.
ff "${ramp[@]}" -vf "settb=1/90000,setpts='(N/24+if(gt(N,24),0.5,0)+if(gt(N,36),0.25,0))/TB'" \
    -fps_mode passthrough "${x264[@]}" -x264-params "$closed" "$out/vfr.mp4"
# Portrait: stored 32x16 with the left half bright, flagged to show turned 90 degrees.
ff -f lavfi -i "color=black:s=32x16:r=24:d=0.125,format=yuv420p,geq=lum='if(lt(X,16),200,50)':cb=128:cr=128" \
    "${x264[@]}" "$out/stored.mp4"
ff -display_rotation -90 -i "$out/stored.mp4" -c copy "$out/rotated.mp4"
rm "$out/stored.mp4"
# ProRes 4444: 10+ bit, intra only, with alpha rising over the 4 frames.
ff -f lavfi -i "color=0x808080:s=16x8:r=24:d=0.1667,format=yuva444p10le,geq=lum='p(X,Y)':cb='p(X,Y)':cr='p(X,Y)':a='256+N*128'" \
    -c:v prores_ks -profile:v 4444 -map_metadata -1 "$out/alpha.mov"
# One colour, sRGB #3060C0, as limited BT.709 and as full-range BT.601.
ff -f lavfi -i "color=0x3060C0:s=64x64:r=24:d=0.0417,scale=out_color_matrix=bt709:out_range=tv,format=yuv444p" \
    -c:v libx264 -qp 1 -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv \
    -map_metadata -1 "$out/bt709_limited.mp4"
ff -f lavfi -i "color=0x3060C0:s=64x64:r=24:d=0.0417,scale=out_color_matrix=bt601:out_range=pc,format=yuvj444p" \
    -c:v libx264 -qp 1 -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range pc \
    -map_metadata -1 "$out/bt601_full.mp4"
# A codec the build leaves out.
ff -f lavfi -i "color=black:s=16x16:r=24:d=0.0417" -c:v libaom-av1 -cpu-used 8 -crf 40 -pix_fmt yuv420p \
    -map_metadata -1 "$out/av1.mp4"

du -ch "$out"/* | tail -1
