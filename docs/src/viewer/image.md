# Image

<video src="../media/viewer-zoom-pan.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `+` / `-` zoom, `hjkl` pans, `f` fits, `z` shows 1:1. `[` / `]` step through
> the layers; `t` tiles them. With several images open, `n` / `p` step between them and
> `t` tiles the images instead. `Ctrl+click` picks a pixel for the inspector.

## Zoom and pan

`+` / `-` or the mouse wheel zoom. `f` fits the image to the area the open panes leave;
`z` shows it 1:1, one image pixel to one screen pixel. `hjkl` or the arrows pan, `HJKL`
pan faster; right-drag (or middle-drag) pans with the mouse.

## Keys

{{#include ../reference/keys.md:viewer-image}}

## Layers

<video src="../media/viewer-layers.mp4" autoplay loop muted playsinline></video>

An EXR's channels group into layers (AOVs) by their prefix, up to the last dot:
`diffuse.R`, `diffuse.G`, `diffuse.B` make the layer `diffuse`. Every channel without a
prefix (`R G B A`, a `Z`, a lone `Y`) goes into the first layer, `rgba`; the other layers
follow alphabetically. `[` / `]` step through them, and the top line shows which one is
up (`rgba (1/6)`). `T` writes the layer's name on the image.

`t` tiles every layer side by side: the quickest way to see what a render wrote. The top
line names the selected tile; `T` writes each tile's name on it. In the tiled view, `+` /
`-` and the wheel zoom all the tiles together, `hjkl` pans, `f` fits and `z` goes 1:1.
Click a tile to select it; `Enter` opens it on its own. `t` again, or `q`, goes back to
one image.

The [layers pane](../panes/layers.md) (`5`) lists them all.

## Several images

Open several images (marked in the browser, or named on the command line) and `n` / `p`
step between them. `t` tiles the images instead of the layers.

<video src="../media/viewer-tiles.mp4" autoplay loop muted playsinline></video>

The [files pane](../panes/files.md) (`3`) lists what's open: show, reorder or drop
images there. To compare two versions, see [Compare](compare.md).

## Picking a pixel

`Ctrl+click` a pixel to pick it: the [inspector](../panes/inspector.md) (`4`) lists every
channel of the layer shown there. To read another layer, step to it (`[` / `]`) and pick
again, or tile the layers (`t`) and `Ctrl+click` in a tile. Coordinates are `[x, y]` as
in Nuke, counted from the bottom-left.

## Labels

`path:label=SLOT:TEXT` on the command line puts a line of text on that image. See
[Labels from the command line](../panes/annotations.md#labels-from-the-command-line).

## Incomplete images

A render that's still writing, or a file cut short, shows what's there: the strips that
were written, the rest black, and **incomplete image** in the top-right corner. Once the
file is complete on disk, the viewer reloads it.
