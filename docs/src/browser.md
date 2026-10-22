# Browser

<video src="media/browser-move.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `j` / `k` move with a live preview, `l` or `Enter` opens, `h` goes up. `/`
> filters. `space` marks images in any folder; `Enter` then views them, `t` tiles them,
> `d` compares them. `cc` copies the path. `Esc` clears the filter, then the marks.

`rvtui` with no arguments, or with a folder, starts here:

```bash
rvtui            # the current folder
rvtui shots/     # another one
```

Three columns, like the column view in the macOS Finder: the parent folder on the left,
the current folder in the middle, and a preview of the selected entry (the row under the
cursor) on the right. Images preview at full resolution; text files show their first
lines.

```text
┌ parent ──┬ current ─────────────────────┬ preview ─────────┐
│ renders  │ ≋ shot.####.exr [1001-1100]  │                  │
│ ▸ shots  │ ▪ chart.exr                  │   (the image)    │
│          │ ▪ hdr-sky.exr                │                  │
└──────────┴──────────────────────────────┴──────────────────┘
```

## Keys

{{#include reference/keys.md:browser}}

`q` quits from the browser. `Q` quits rvtui from anywhere.

## Moving around

`j` / `k` (or the arrows) move, `gg` / `G` jump to the top and bottom. `l`, `→` or
`Enter` goes into a folder or opens an image in the [viewer](viewer/README.md); `h` or
`←` goes back up. On a text file, `l` moves into the preview to scroll it.

With the mouse: a click selects, a double-click opens, a click in the parent column goes
there. Drag the column dividers to resize them.

`.` shows hidden files.

## Filter

`/` starts a fuzzy filter over the current folder: type, and only the matches stay.
`Enter` keeps the filter and goes back to moving; `Esc` clears it.

<video src="media/browser-filter.mp4" autoplay loop muted playsinline></video>

## Sequences

Numbered frames collapse into one entry: `shot.1001.exr` … `shot.1100.exr` becomes
`≋ shot.####.exr [1001-1100]`, with the total size. Gaps show in the range
(`[1001-1005,1009-1014,1016-1024]`). Two frames make a sequence; a lone numbered file
stays a file.

`e` expands the selected sequence into its frames, and folds them back.

<video src="media/browser-sequences.mp4" autoplay loop muted playsinline></video>

> [!NOTE]
> The listing follows the disk: new files and frames appear as they're written. A
> half-written EXR previews as an [incomplete image](viewer/image.md#incomplete-images).

## Marks

Marking picks images to open together, like a selection that stays as you move between
folders. `space` marks the selected image (again: unmarks it); `a` marks every image in
the folder (again: unmarks them). Marked rows show in yellow, in the order you marked
them. `Esc` clears every mark (after the filter, if one is set).

With marks, `Enter` or `l` on any image opens the marked images, not the one under the
cursor:

- `Enter` views them one at a time: `n` / `p` step between them.
- `t` opens them in a tiled view.
- `d` [compares](viewer/compare.md) them in pairs: the 1st with the 2nd, the 3rd with the
  4th, … With one marked, `d` compares it with the selected image.

<video src="media/browser-marks.mp4" autoplay loop muted playsinline></video>

## Copying paths

`c`, then a second key, copies to the clipboard: the marked entries, one per line, or
the selected one when nothing is marked.

| Keys | Copies |
|------|--------|
| `cc` | the full path |
| `cd` | the folder it's in |
| `cf` | the file name |
| `cn` | the name without its extension |

A sequence copies as its frame pattern, `/renders/shot.####.exr`, which Nuke and RV read
as is. The bottom line shows what was copied. The copy works over ssh and in tmux the
same way as `y` in the panes (see [Copying](panes/README.md#copying)).

## Picking files for other tools

rvtui can act as a file picker. With `--chooser-file FILE`, `o` writes paths to FILE,
one per line, and quits: the marked images, or the selected entry when nothing is marked
(a sequence as its first frame). `Enter` still opens the viewer.

```bash
rvtui --chooser-file /tmp/picked shots/ && cat /tmp/picked
```

`--cwd-file FILE` writes the folder you were in when you quit, so a shell function can
follow you there:

```bash
rv() {
  local f; f=$(mktemp)
  rvtui --cwd-file "$f" "$@"
  cd "$(cat "$f")" && rm -f "$f"
}
```
