# Panes

<video src="../media/panes.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `2`-`6` open the metadata, files, inspector, layers and colour panes and
> focus them; the same key again closes a focused pane. `1` focuses the image, `Tab`
> hides every pane and brings them back.

## Keys

{{#include ../reference/keys.md:viewer-panes}}

## The panes

The viewer's panes, numbered as in RV:

| Key | Also | Pane | Shows | Where |
|-----|------|------|-------|-------|
| `2` | `m` | [Metadata](metadata.md) | the header: attributes and layers | right |
| `3` | `o` / `x` | [Files](files.md) | the open sources and their [annotations](annotations.md) | bottom left |
| `4` | `i` | [Inspector](inspector.md) | pixel values and layer statistics | right, above metadata |
| `5` | `/` | [Layers](layers.md) | every layer; the cursor picks the one shown | left |
| `6` | | [Colour](colour.md) | OCIO config, display, view, look, input colour space | left |

The keys in the Also column open their pane only when the focused pane doesn't use that
key itself: in the inspector `x` clears the pick, and in the metadata pane `/` filters.

## Focus

A pane opens focused: its keys (`j` / `k`, `y` to copy, …) go to it. Keys it doesn't
use fall through to the viewer, so exposure, playback and the rest still work while a
pane is focused.

- The pane's number again closes it.
- `1` focuses the image, leaving the panes open, so `j` / `k` and the other keys act on
  the image again.
- `Tab` hides every pane at once, and brings them back.
- A click on a pane focuses it.

## Panes cover the image

Panes are drawn over the image, so opening or closing one never moves or refits the
image. `f` fits the image into the space the open panes leave; `Tab` hides them all for
a clear view. (A tiled view is laid out beside the panes instead.)

## Resizing panes

Drag a pane's edge to change its width. The files pane sits at the bottom: drag its title
up to show more rows.

## Copying

The metadata and inspector panes share `y` (copy the line) and `Y` (copy just the
value); in the metadata pane, `v` starts selecting lines. The copy goes to the system
clipboard: in tmux through tmux's buffer (`set -g set-clipboard on` passes it to the
terminal), otherwise straight through the terminal, which Ghostty and kitty allow.
