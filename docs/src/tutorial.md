# First run: the tutorial

<video src="media/tutorial.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `rega --tutorial` renders sample images into `~/.cache/rega/tutorial` and
> opens them with a guide card in the corner. `F1` hides the card, `F2` skips a step,
> `F3` goes back one. `Q` quits.

```bash
rega --tutorial
```

The first run renders the images (a few seconds); later runs open them straight away.
rega opens in the [browser](browser.md), on the tutorial folder. A card in the
bottom-right corner gives one step at a time. Each step names its keys; they turn green
as you press them, and the step ticks off when all of them are done.

The steps go: move in the browser, open and play the sequence, step and scrub, set in
and out points, step through the layers, tile them, switch channels, change exposure,
read a pixel, go back to the browser, then the HDR sky, the broken render and several
images at once.

| Key | Does |
|-----|------|
| `F1` | hide / show the card |
| `F2` | skip a step |
| `F3` | back one step |
| `Q` | quit rega |

On a Mac keyboard, the F keys may need `Fn`. The card starts from the first step each
time.

## The images

Every image explains itself in its `comments` attribute: open the metadata pane (`2`) to
read it. The same list is in `README.txt` next to them.

| File | What it shows | Try |
|------|---------------|-----|
| `shot.####.exr` | frames 1001-1100 at 24 fps; layers `rgba` (R G B A, plus depth Z), N, albedo, diffuse, mask, specular | `space` to play, `[` `]`, `t` |
| `wip.####.exr` | a render still being written: frames 1006-1008 and 1015 missing, the last one cut short | open it, `space` to play, `G` |
| `chart.exr` | a colour chart over a linear grey wedge | `Ctrl+click` a patch |
| `chart-v2.exr` | the chart with two patches changed | in the browser, `space` on chart.exr and on this one, then `d` |
| `hdr-sky.exr` | values far above 1 | `E` to stop down |
| `luma-ramp.exr` | a single channel (Y) | `y` / `Y` gamma, `s` view transform off |
| `beachball.exr` | a data window (a box around a beach ball) inside the display window | `w` |
| `nan-inf.exr` | NaN and inf pixels | `!` |

Every clip in this guide uses these images, so you can follow along.
