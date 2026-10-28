# Annotations

<video src="../media/pane-annotations.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** Burn-in text over the image. Press `3`, move to **global** (every
> image) or a source, press `l`. On a slot, `a` adds a line: type it, `Enter` keeps it.
> `[#@frame]` and `[#owner]` fill in per frame. `A` in the viewer hides them all.

## Keys

Inside a row of the [files pane](files.md) (`3`, then `l`):

{{#include ../reference/keys.md:pane-annotations}}

## Slots and sets

Each image has six **slots**, the places a line can sit: `tl tc tr` along the top
(left, centre, right), `bl bc br` along the bottom. A slot holds any number of lines;
top slots stack downwards, bottom slots upwards.

Lines live in one of two **sets**:

- **global**: over every image.
- **per source**: one set for each row of the files pane. A sequence is one source, so
  its lines go over all its frames.

In each slot the global lines come first, then the source's. In a tiled view, the global
lines go once over the whole tiled view (filled in from the selected tile) and each tile
shows its own source's lines.

Text sits on the image, inside the display window, on a darkened patch of the image's
own colour, so it reads over any image. It moves with zoom and pan like a real burn-in.

> [!NOTE]
> Panes are drawn over the image's sides, so while the files pane is open it covers the
> left slots. A centre slot (`tc`, `bc`) stays in view while you edit.

## Adding a line

1. Open the files pane: `3`.
2. Move to **global** (the row above the files) or to a source, and press `l` to go into
   its set: all six slots, with their lines.
3. Move to a slot with `j` / `k` and press `a`. Type the line in the bottom bar; the
   image shows it as you type. `Enter` keeps it, `Esc` drops it.

On a line, `a` adds a line below it and `A` above it; on a slot's own row, `a` (or
`Enter`) adds one at the end of that slot. `Enter` on a line edits it, `d` deletes it,
`J` / `K` move it, and `g` / `s` move it to the global set or the source's. `h` goes back
to the file list.

While typing: `Tab` jumps to the next slot, `↑` / `↓` recall lines typed before, and
`Ctrl+n` completes a `[#…` placeholder from the frame's header.

`D` `D` in the file list clears every annotation; inside a set, just that set.

## Placeholders

| Placeholder | Becomes |
|-------------|---------|
| `[#key]` | header attribute `key` of the frame, e.g. `[#owner]`, `[#comments]` |
| `[#@file]` | the frame's file name, `shot.1042.exr` |
| `[#@frame]` | the frame number (empty for a single image) |
| `[#@layer]` | the layer shown |
| `[#@res]` | the resolution, `1920x1080` |
| `[#@fps]` | the playback rate |
| `[#@date]` | today, `YYYY-MM-DD` |

They fill in per frame and per tile. A key the header doesn't have stays as typed,
dimmed (`[#ownr]`), so a typo shows.

## Kept between sessions

Annotations are saved when you quit and come back next time, with the recall history,
in `~/.local/state/rega/annotations`.

## Labels from the command line

A `:label=` on an image path puts a line on that image for this run, with the same slots
and placeholders:

```bash
rega 'shot.####.exr:label=tl:[#@frame]'  'v2.exr:label=bottom:client notes'
rega --diff v1.exr:label=bl:old v2.exr:label=bl:new
```

The slot is one of `tl tc tr bl bc br`, or `top` / `bottom` for the centre ones. Labels
aren't part of the sets above and aren't saved, but `A` hides them too.
