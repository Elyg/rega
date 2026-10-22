# Inspector (4)

<video src="../media/pane-inspector.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `4` (or `i`) opens the inspector. It reads the pixel under the mouse as it
> moves. `Ctrl+click` picks a pixel and lists every channel of the layer shown; `x`
> clears the pick. Below that: each channel's min, max and average, with a warning for
> NaN and inf.

## Keys

{{#include ../reference/keys.md:pane-inspector}}

## Under the mouse

The top of the pane reads the pixel under the mouse as it moves: its position and its
values, each number in its channel's colour. Positions are `[x, y]` as in Nuke, counted
from the bottom-left of the display window. A `≈` means the value was read from a
reduced resolution (while playing, or zoomed out on a big image).

## The picked pixel

`Ctrl+click` picks a pixel; a red ring marks it while the inspector is open. The pane
lists the pixel's channels in the layer it was picked in, plus its colour on screen and
its luma. To read another layer, step to it (`[` / `]`) and pick again, or tile the
layers (`t`) and pick in a tile. `j` / `k` move over the rows, `y` copies a line, `Y` a
value, `x` clears the pick.

## Layer statistics

For the layer on screen: each channel's minimum, maximum and average. Under the title,
a count for the whole layer (`12 NaN · 3 inf px`, also on the top line). The title shows
a badge (`NaN`, `inf`) when the pixel under the mouse, or the picked one, holds such a
value. `!` paints them on the image: see [NaN and inf](../viewer/display.md#nan-and-inf).
