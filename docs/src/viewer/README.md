# Viewer

> [!TIP]
> **TL;DR** `Enter` on an image in the browser, or `rvtui render.exr`, opens the viewer.
> Its keys come in groups: [Image](image.md) (zoom, layers, several images),
> [Compare](compare.md), [Sequence](sequence.md) (playback) and [Display](display.md)
> (channels, exposure). `2`-`6` open the [panes](../panes/README.md). `?` lists every
> key, `q` goes back to the browser, `Q` quits.

```bash
rvtui render.exr                 # one image
rvtui 'shot.####.exr'            # a sequence (quote the #s)
rvtui a.exr b.exr c.exr          # several: n / p step between them
rvtui --diff v1.exr v2.exr       # A, B and |A − B|
```

## The screen

```text
┌ [1] shot.1042.exr  rgba (1/6)  RGBA  960×540  🔍 fit ─────────────┐
│ layers ┐                                         ┌ inspector      │
│ colour │                the image                │ metadata       │
│ files  ┘                                         └                │
├ 1001 ───────────────▲──────────────────── 1100    24.00  24.0 fps ┤
└ space play  [ ] layer  2 meta  3 files  ? help  q back ───────────┘
```

- **Top line**: which open image (`[1]`, its number in the files pane) and its name, the
  layer shown, its channels, the resolution and the zoom (`🔍 fit`, `🔍 200%`).
- **The image**, fitted to the terminal at first.
- **Panes**, when open: layers, colour and files on the left; inspector and metadata on
  the right. See [Panes](../panes/README.md).
- **Playbar**, for a sequence: frame numbers, the playhead, in and out points and the
  playback rate. See [Sequence](sequence.md).
- **Bottom line**: the keys that matter most right now.

## Sequences on the command line

To open a sequence, give a pattern instead of one frame. Put it in single quotes so the
shell passes it to rvtui unchanged:

| Pattern | Matches |
|---------|---------|
| `'shot.####.exr'` | `shot.1001.exr` … (each `#` one digit) |
| `'shot.#.exr'` | any padding: a lone `#` finds it |
| `shot.*.exr` | `*` works too, unquoted: the shell lists the frames for rvtui |

A plain frame path (`shot.1001.exr`) opens just that frame.

## Leaving

{{#include ../reference/keys.md:viewer-leave}}
