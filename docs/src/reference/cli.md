# Command line

```text
rvtui [OPTIONS] [paths...]
```

## Paths

| Argument | Opens |
|----------|-------|
| *(none)* | the browser, in the current folder |
| `shots/` | the browser, in that folder |
| `render.exr` | the viewer, on that image |
| `'shot.####.exr'` | the viewer, on the sequence (each `#` one digit) |
| `'shot.#.exr'` | the sequence, whatever its padding |
| `a.exr b.exr …` | the viewer, on all of them (`n` / `p` step) |
| `a.exr:label=top:main` | the image, with a [label](../panes/annotations.md#labels-from-the-command-line) |

Quote patterns with `#` so the shell leaves them alone. A plain frame path
(`shot.1001.exr`) opens just that frame.

## Options

| Option | Does |
|--------|------|
| `--diff A B` | compare A with B: A, B and \|A − B\| tiled. Repeat for more pairs; paths can carry labels. See [Compare](../viewer/compare.md) |
| `--dump FILE` | print the layers, channels and metadata, then exit |
| `--doctor` | check the terminal and tmux setup for images and copy, then exit (1 if a check fails). See [Terminal setup](../setup.md) |
| `--tutorial` | render the sample images into `~/.cache/rvtui/tutorial` (first run) and open them with a guide. See [tutorial](../tutorial.md) |
| `--graphics MODE` | `auto` (default), `kitty` or `halfblock`. See [setup](../setup.md) |
| `--transfer MODE` | how kitty images reach the terminal: `auto` (default) picks `shm` (shared memory) locally outside tmux, `file` (temp files) in tmux, `direct` (inline) over ssh. Change it only if images stay blank |
| `--link-rate SIZE` | bytes a second the ssh link is assumed to carry until measured, e.g. `20M`; playback sending images inline is paced to the link (default `4M`, `0`: unpaced, unmeasured) |
| `--terminal-cache SIZE` | terminal image memory frames played over ssh may keep there, e.g. `512M`: a loop's second pass sends no pixels (default `256M`, under kitty's and Ghostty's 320 MB; `0`: off) |
| `--cache SIZE` | memory for decoded images, e.g. `8G` or `512M` (default `4G`, at least `64M`) |
| `--chooser-file FILE` | `o` in the browser writes the chosen paths to FILE and exits. See [picking files](../browser.md#picking-files-for-other-tools) |
| `--cwd-file FILE` | on exit, write the last folder to FILE |
| `--version` | print the version |
| `-h`, `--help` | print the options |

## Files rvtui keeps

| Path | What |
|------|------|
| `~/.cache/rvtui/tutorial/` | the tutorial's images |
| `~/.local/state/rvtui/annotations` | [annotations](../panes/annotations.md), between sessions |
| `~/.local/state/rvtui/colour` | the [colour](../panes/colour.md) choices, per config |

`$XDG_CACHE_HOME` and `$XDG_STATE_HOME` move them.

## Environment

| Variable | Effect |
|----------|--------|
| `OCIO` | the OpenColorIO config to show images through |
| `LC_TERMINAL` | names the terminal behind a tmux on the other side of ssh (`ghostty`, `kitty`): turns kitty graphics on there |
