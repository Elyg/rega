# rvtui

<video src="media/intro.mp4" autoplay loop muted playsinline></video>

rvtui is an image browser and viewer that runs in the terminal. It is made for renders:
multi-layer OpenEXR files and sequences, plus TIFF, PNG and JPEG.

Use it on your own machine, or on one you reach through a terminal, such as a farm node
or a remote workstation: log in there with ssh from Ghostty or kitty, run rvtui on that
machine, and the images show in your terminal.

## Where to start

1. **[Install](install.md)** rvtui on your machine, and on any remote machine whose
   renders you want to look at.
2. **[Terminal setup](setup.md)**: check your terminal (and tmux, if you use it) with
   `rvtui --doctor`.
3. **[The tutorial](tutorial.md)**: `rvtui --tutorial` walks you through sample images.

After that, `rvtui` browses the current folder and `rvtui render.exr` opens one image.
`?` lists every key.

Then the guide follows the screen, in the same order as the `?` list: the
[browser](browser.md), the [viewer](viewer/README.md) and its [panes](panes/README.md).
Each page starts with a clip of the feature, a TL;DR and its keys. The
[reference](reference/cli.md) lists every command-line option and every key.

The clips use the tutorial's images, so you can follow along: run `rvtui --tutorial` and
press the keys shown in the corner of each clip.

## What it does

- **Browse** folders with a live preview. Numbered frames collapse into one sequence
  (`shot.####.exr`). Filter by typing, and mark images in any folder to open together.
- **View** one image, a sequence with a playbar, every layer (AOV) side by
  side, or several images tiled.
- **Compare** two versions: A, B and |A − B| side by side, plus the header rows that
  differ.
- **Inspect** pixel values (Nuke coordinates), per-channel statistics, NaN and inf
  pixels, and the data window against the display window.
- **Colour** through your OpenColorIO config (`$OCIO`) or OCIO's built-in ACES
  configs: display, view, look and each image's input colour space.
- **Annotate** with burn-in text, filled in from each frame's header (`[#owner]`,
  `[#@frame]`).
- **Follow a render**: new frames appear as they're written, and a half-written EXR
  shows what's there.

## Reading the keys

- `h` `j` `k` `l` move left, down, up and right, as in the vim editor. The arrow keys
  work too.
- A capital letter means Shift: `E` is Shift+e, and `e` and `E` do different things.
- `gg` means press `g` twice.
- `Ctrl+d` and `Ctrl+click` mean hold Ctrl.
- `?` lists every key for the screen you're on. Type to search it (a pane's name finds
  that pane's keys); `?` or `Esc` closes the list.
- `q` goes back one screen. `Q` quits rvtui from anywhere.

## How images are drawn

In [Ghostty](https://ghostty.org) and [kitty](https://sw.kovidgoyal.net/kitty/), images
are full resolution, also through tmux and ssh. Every other terminal shows half-blocks,
a coarser picture ([side by side](setup.md#other-terminals-half-blocks)).
[Terminal setup](setup.md) has the details.
