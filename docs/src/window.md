# The window

> [!TIP]
> **TL;DR** `rega --gui` opens rega in a window of its own: the same keys and panes,
> no kitty or Ghostty needed. With nothing to open it starts on a screen to open files,
> a folder, pasted paths or a recent one. Drop files on it, Ctrl+V paths, drag to pan.

```bash
rega --gui              # the start screen
rega --gui shots/       # the browser, in that folder
rega --gui render.exr   # the viewer
```

Everything in this guide works the same in the window, keys and panes alike. Pictures
are always full resolution, playback too (`P` still caps it when a sequence can't keep
up).

> [!NOTE]
> The window comes with the Nix package and with source builds. The static Linux binary
> from the install script is terminal-only. On Windows, `rega.exe` is always the window.

## Start screen

```text
  Open files…
  Open a folder…
  Browse ~/renders
  Tutorial

  Paste paths…
    comp.exr            ~/renders/v002                 ×
    comp.exr            ~/renders/v003                 ×
                        [ Open (2) ]

  Recent
  shot.####.exr         ~/renders/sq010            2 h ago
  plates/               ~                         3 days ago
```

- **Open files…** and **Open a folder…** show the system's dialogs.
- **Browse** opens the [browser](browser.md) where it was last left. `q` in the
  browser comes back here.
- **Tutorial** renders the [tutorial](tutorial.md)'s images (the first time) and starts
  its tour.
- **Paste paths…**: Ctrl+V adds each pasted line to the list; `a` types one (`Enter`
  adds it). Paths from a file manager (`file://…`), quoted ones and `~` are fine;
  patterns like `shot.####.exr` open as sequences. Missing ones show red. **Open (n)**
  opens them together, as marks do in the browser: `n` / `p` step, `t` tiles.
- **Recent**: a click puts an item in that list (again: out), Shift+click a range,
  a double click opens it on its own. `x` forgets one.

## Keys

{{#include reference/keys.md:start}}

{{#include reference/keys.md:window}}

## Mouse

- **Drop** files or folders anywhere on the window to open them.
- **Drag** to pan: the right or middle button anywhere, the left one on a still or on
  tiles. The picture follows the pointer; tiles still rendering show as boxes.
- **Wheel** over the image or tiles zooms by as much as you turn: smoothly on a
  touchpad.
- **Shift+click** / **Ctrl+click** in the browser mark a range / one more.
- **Drag** a file in the [files pane](panes/files.md) to sort the list.
- **Click** a selected row in the [metadata pane](panes/metadata.md) again, or below
  the rows, to unselect.

Ctrl+C copies: the path in the browser and the viewer, the selected rows in the
metadata and inspector panes. Ctrl+V pastes into what you type, else opens the paths. On
macOS ⌘ works as Ctrl.

## What it keeps

| Path | What |
|------|------|
| `~/.local/state/rega/window` | size, text size, the browser's last folder |
| `~/.local/state/rega/recent` | the start screen's recent items |

On Windows they're in `%LOCALAPPDATA%\rega`.

## First launch

The app isn't notarized (macOS) or signed (Windows) yet, so the system asks once:

- **macOS**: open it, then *System Settings → Privacy & Security → Open Anyway*.
- **Windows**: SmartScreen's *More info → Run anyway*.
