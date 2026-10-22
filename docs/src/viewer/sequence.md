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
header), else 24. `F` cycles the rate through 12, 23.976, 24, 25, 30, 48 and 60 fps. The
playbar shows the target rate and, while playing, the rate actually reached.

Frames are never dropped: when the next one isn't decoded yet, playback waits for it and
the measured rate drops. While playing, rvtui reads ahead as many frames as fit in 60% of
the cache (`--cache`, 4G by default), shared between the open sources, 2 to 96 frames
each.

`P` switches how playback renders:

- **default**: below full resolution, so playback keeps up. Pausing sharpens the frame.
- **full**: every frame at full resolution, as fast as rvtui can.

## Moving through frames

| Key / mouse | Does |
|-------------|------|
| `,` / `.` | one frame back / forward |
| `<` / `>` | first / last frame |
| `:` | type a frame number; `Enter` goes there (the nearest when it's missing) |
| left-drag | scrub, on the image or the playbar |
| click / wheel on the playbar | go to a frame / step |

Frame numbers are the files' own (`1001`), not counted from zero.

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

rvtui watches the folders it shows. New frames join an open sequence as they're written,
and a frame that changes on disk is decoded again. A frame still being written shows as
an [incomplete image](image.md#incomplete-images) until it's done. Point rvtui at a
render's output folder and leave it open.

> [!NOTE]
> Over ssh, playback is paced to `--link-rate` so the link doesn't back up. See
> [Over ssh](../setup.md#over-ssh).
