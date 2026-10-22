# Compare

<video src="../media/viewer-diff.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `rvtui --diff v1.exr v2.exr`, or mark two images in the browser and press
> `d`. You get A, B and |A − B|: whatever changed lights up. Click the difference tile
> and `2` lists the header rows that differ.

## Starting a comparison

```bash
rvtui --diff v1.exr v2.exr
rvtui --diff a1.exr b1.exr --diff a2.exr b2.exr   # several pairs: n / p
```

Or in the [browser](../browser.md#marks): mark two images and press `d`. More marked pair
up in order (1st with 2nd, 3rd with 4th, …); with one marked, `d` compares it with the
selected image.

## The three tiles

A comparison opens as a tiled view: **A** and **B** on top, **|A − B|** below. The
difference is per channel, so everything that changed lights up and the rest stays
black. Under it, `max Δ 0.0213` gives the largest difference, or `identical`.

The three tiles zoom and pan together, as in any tiled view. `n` / `p` go to the next and
previous pair.

| Key | Does |
|-----|------|
| `d` (browser) | compare the marked images in pairs |
| `n` / `p` | next / previous pair |
| click a tile | select it; `Enter` opens it on its own |
| `2` | the metadata pane: on the difference tile, the rows that differ |

## The header rows that differ

Click the difference tile to select it, then open the
[metadata pane](../panes/metadata.md) (`2`): it compares A's header with B's and lists
only the rows that differ. `d` there shows both headers whole, and back. With A or B
selected, the pane shows that image's header.

## Labels on each side

Name each side so the tiles say which is which:

```bash
rvtui --diff v1.exr:label=bl:old v2.exr:label=bl:new
```

See [Labels from the command line](../panes/annotations.md#labels-from-the-command-line).
