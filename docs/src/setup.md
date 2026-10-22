# Terminal setup

Run **`rvtui --doctor`** first: it checks your terminal and tmux, and says what to add.

```console
$ rvtui --doctor
rvtui doctor
  ✓ terminal     TERM=tmux-256color TERM_PROGRAM=tmux tmux client=xterm-ghostty
  ✓ tmux         version 3.8
  ✓ passthrough  allow-passthrough on
  ✓ cell size    13x28 px
  ✓ truecolor    tmux client has RGB
  ✓ clipboard    set-clipboard on
  · graphics     kitty
  · transfer     file: pixels go through temp files, not the pty
```

What rvtui needs depends on your terminal app and on what runs between it and rvtui.
tmux keeps terminal sessions alive and splits them into panes; if you don't use it, skip
the tmux sections.

## Ghostty or kitty: just works

```mermaid
flowchart LR
  T[Ghostty / kitty] --> R[rvtui]
  T2[Ghostty / kitty] --> S[ssh] --> R2[rvtui]
```

## Other terminals: half-blocks

iTerm2, WezTerm, Warp, Konsole, Alacritty, foot, GNOME Terminal, xterm, Windows Terminal,
Terminal.app, VS Code and zellij show half-blocks: each character cell shows two pixels,
one above the other. Everything else works the same.

![The same frame at full resolution (left, Ghostty) and in half-blocks (right)](media/graphics-compare.png)

*Full resolution on the left (Ghostty), half-blocks on the right.*

## tmux: one line in tmux.conf

```mermaid
flowchart LR
  T[terminal] --> X["tmux<br/>allow-passthrough on"] --> R[rvtui]
  T2[terminal] --> S[ssh] --> X2["remote tmux<br/>allow-passthrough on"] --> R2[rvtui]
```

In every tmux between the terminal and rvtui, add to `~/.tmux.conf`, then
`tmux source-file ~/.tmux.conf`:

| tmux.conf | What for |
|-----------|----------|
| `set -g allow-passthrough on` | images (needed) |
| `set -as terminal-features ',xterm*:RGB'` | true colour |
| `set -g set-clipboard on` | `y` copies reach the clipboard |

Use tmux 3.8 or later: 3.7 drops images in split panes.

## tmux, then ssh: add --graphics kitty

When tmux runs on your machine and you ssh from inside it, rvtui on the remote can't ask
your terminal what it supports. Tell it:

```mermaid
flowchart LR
  T[terminal] --> X["local tmux<br/>allow-passthrough on"] --> S[ssh] --> R["rvtui --graphics kitty"]
  T2[terminal] --> X2["local tmux<br/>allow-passthrough on"] --> S2[ssh] --> X3["remote tmux<br/>allow-passthrough on"] --> R2["rvtui --graphics kitty"]
```

`--graphics kitty` skips the check, so a missing setting leaves images blank instead of
falling back to half-blocks. (`--graphics halfblock` forces half-blocks.)

## Over ssh

Images go through the ssh connection. While a sequence plays, rvtui paces them to
`--link-rate` (4M a second); on a fast network, raise it: `--link-rate 20M`.
`--transfer` picks how pixels reach the terminal; `auto` gets it right (see
[Command line](reference/cli.md#options)).
