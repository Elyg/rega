# Display

<video src="../media/display-channels.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `c` shows colour; `r` `g` `b` `a` show one channel as grey; `u` shows luma.
> `e` / `E` change exposure by half a stop, `y` / `Y` change gamma, `0` resets the
> display. `s` turns the view transform off and on. `w` outlines the display and data
> windows. `!` paints NaN and inf pixels.

The display settings change how the image looks, never the file. They stay as you step
between images and frames.

## Keys

{{#include ../reference/keys.md:viewer-display}}

## Channels

| Key | Shows |
|-----|-------|
| `c` | colour (all channels) |
| `r` `g` `b` | one channel, as grey |
| `a` | alpha, as grey |
| `u` | luma |

A layer with a single channel (`luma-ramp.exr`'s `Y`, a depth layer) shows as grey.

## Exposure and gamma

<video src="../media/display-exposure.mp4" autoplay loop muted playsinline></video>

`E` stops down half a stop, `e` brightens half a stop; `y` / `Y` change the gamma by 0.1.
Values above 1 (an HDR sky, a hot specular) clip to white until you stop down.

`0` resets the display: exposure, gamma and channel, and it turns the view transform
back on, and outlines and NaN painting off.

## View transform

The image goes through a view transform to the screen: plain sRGB, or your OCIO display
and view when a config is in use (see the [colour pane](../panes/colour.md)). `s` turns
it off to show the raw values, and back on.

## Outlines

<video src="../media/display-outlines.mp4" autoplay loop muted playsinline></video>

An EXR has two rectangles:

- the **display window**: the area the image is meant to show (Nuke's format);
- the **data window**: where the pixels actually are (Nuke's bounding box).

They often match. When the data window reaches past the display window, that part is
**overscan**; when it's smaller, the rest of the display window simply has no pixels.

The clip shows `beachball.exr` from the tutorial, after the OpenEXR docs' example: the
data window is a box around a beach ball, inside the display window, and the rest of the
display window has no pixels (black). `w` cycles the outlines:

1. display window, plus the data window (dashed)
2. display window only
3. none (where it starts)

In a tiled view the selected tile is outlined from the start; there `w` cycles the
selection, the windows plus the selection, the windows, none.

## NaN and inf

<video src="../media/display-nan.mp4" autoplay loop muted playsinline></video>

`!` paints NaN pixels magenta and infinite ones cyan, so a broken render shows where it
broke. The [inspector](../panes/inspector.md) counts them per layer; step through the
layers (`[` / `]`) to find the one that broke.

## Annotations

`A` shows or hides the [annotations](../panes/annotations.md), the burn-in text over the
image. Hiding keeps them.
