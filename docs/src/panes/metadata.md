# Metadata (2)

<video src="../media/pane-metadata.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `2` (or `m`) shows the current frame's header: its parts, attributes and
> layers. `/` filters it, `y` / `Y` copy a line or just its value. On a comparison it
> lists only what differs.

## Keys

{{#include ../reference/keys.md:pane-metadata}}

## What it shows

The header of the frame on screen:

- its path;
- each part, with every attribute and its value (`compression`, `dataWindow`,
  `framesPerSecond`, `comments`, the renderer's own);
- each layer and its channels.

As you step or play, it shows each frame's header. (`rega --dump` prints the same, with
types: see below.)

## Moving and filtering

`j` / `k` move, `gg` / `G` jump to the ends, `Ctrl+d` / `Ctrl+u` move half a page; the
wheel or `{` / `}` scroll. The row under the cursor expands to show its whole value.

`/` filters. A row stays when its name fuzzy-matches what you type, or its value contains
it; matching rows stay under their part. `Enter` keeps the filter, `Esc` clears it.

## Copying

`y` copies the line (`name: value`), `Y` only the value. `v` selects lines from the
cursor; a mouse drag or `Ctrl+click` selects rows too. Then `y` / `Y` copy them all.

## On a comparison

Select the difference tile of a [comparison](../viewer/compare.md#the-header-rows-that-differ):
the pane compares A's header with B's and lists only the rows that differ. `d` toggles
between those and both headers whole.

## From the command line

```console
$ rega --dump shot.1001.exr
shot.1001.exr  [exr]
fps: 24

part 0
  channels (chlist): A(half), B(half), G(half), N.X(half), …
  compression (compression): zip
  dataWindow (box2i): [0, 0]-[959, 539]  960x540
  …

layers:
  rgba: R(half) G(half) B(half) A(half) Z(half)
  N: X(half) Y(half) Z(half)
  …
```
