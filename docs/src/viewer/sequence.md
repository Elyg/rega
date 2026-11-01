# Sequence

<video src="../media/sequence-play.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `space` plays and pauses. `,` / `.` step one frame, `<` / `>` jump to the
> first and last, `:` goes to a frame number. `I` / `O` set the in and out points
> (again: clear). `G` switches missing frames between skip and hold (show the nearest).

A sequence gets a playbar under the image. It shows:

- the frame numbers, with the playhead over the current frame;
- the bar, green where frames are decoded and ready;
- the playback rate.

## Keys

{{#include ../reference/keys.md:viewer-sequence}}

## Playing

`space` plays and pauses. Playback runs at the file's rate (`framesPerSecond` in the EXR
header, or a movie's own), else 24. `F` cycles the rate through 12, 23.976, 24, 25, 30, 48 and 60 fps. The
playbar shows the target rate and, while playing, the rate actually reached.

Frames are never dropped: when the next one isn't decoded yet, playback waits for it and
the measured rate drops. While playing, rega reads ahead as many frames as fit in 60% of
the cache (`--cache`, 4G by default), shared between the open sources, 2 to 96 frames
each.

`P` switches how playback renders:

- **default**: below full resolution, so playback keeps up. Pausing sharpens the frame.
- **full**: every frame at full resolution, as fast as rega can.

## Moving through frames

| Key / mouse | Does |
|-------------|------|
| `,` / `.` | one frame back / forward |
| `<` / `>` | first / last frame |
| `:` | type a frame number; `Enter` goes there (the nearest when it's missing) |
| left-drag | scrub, on the image or the playbar |
| click / wheel on the playbar | go to a frame / step |

Frame numbers are the files' own (`1001`), not counted from zero. A movie's start at 1.

## In and out

<video src="../media/sequence-inout.mp4" autoplay loop muted playsinline></video>

`I` sets the in point at the current frame, `O` the out point; press it again on the same
frame to clear it. Playback then loops from in to out, and `,` `.` `<` `>` stay inside.
The bar is thinner outside the range.

## Missing frames and renders in progress

<video src="../media/sequence-missing.mp4" autoplay loop muted playsinline></video>

A sequence with gaps (frames not rendered yet, or failed) plays one of two ways; `G`
switches between them:

- **skip** (default): missing frames are left out, so playback goes from one existing
  frame to the next.
- **hold**: a frame for every number from first to last; a missing one shows the
  nearest that exists, so the timing stays right.

rega watches the folders it shows. New frames join an open sequence as they're written,
and a frame that changes on disk is decoded again. A frame still being written shows as
an [incomplete image](image.md#incomplete-images) until it's done. Point rega at a
render's output folder and leave it open.

> [!NOTE]
> Over ssh, playback is paced to the link, measured as it plays, so it doesn't back up. See
> [Over ssh](../setup.md#over-ssh).

## Movies

`.mov` and `.mp4` files open like a sequence: the same playbar, keys, in and out points,
cache and `--diff`, with frames numbered from 1. In the browser a movie is one file, even
when its name ends in a number, so `take_v001.mov` and `take_v002.mov` stay two takes.

Going anywhere is frame-accurate, including backwards. In a long-GOP codec (H.264, HEVC)
a jump decodes forward from the keyframe before it, so it takes a moment on a heavy file;
intra codecs such as ProRes jump straight there.

rega reads H.264, HEVC, ProRes, DNxHD and DNxHR, Motion JPEG, MPEG-2, MPEG-4 Part 2, VP9,
PNG, QuickTime Animation and uncompressed video. An alpha channel shows as `A`, and a
clip recorded on its side is turned upright. Metadata lists the codec, pixel format,
colour tags and the file's own tags.

What it doesn't do:

- **No sound.** Audio tracks are ignored.
- **No AV1.** A file in a codec rega doesn't read says so in place of the picture.
- **No HDR.** Colour goes by the matrix and range tags, and the code values are then
  treated as sRGB like a PNG's, whatever the transfer tag says: a PQ or HLG clip looks
  washed out rather than tone-mapped.
- **Non-square pixels are shown as stored.** Metadata lists the `pixelAspect`.
- **A variable frame rate plays at its average.** Every frame is shown, evenly spaced.
- **`e` doesn't expand a movie** in the files pane: its frames have no files.
