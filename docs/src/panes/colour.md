# Colour (6)

<video src="../media/pane-colour.mp4" autoplay loop muted playsinline></video>

> [!TIP]
> **TL;DR** `6` opens the colour pane: `j` / `k` pick a row, `h` / `l` change it, `Enter`
> picks from a list. With `$OCIO` set, rega shows images through that config. Without
> it, images are plain sRGB, or you pick a built-in ACES config in the first row. For
> ACEScg renders under a built-in config, set the input row to `ACEScg` ([why](#aces)).

## Keys

{{#include ../reference/keys.md:pane-colour}}

## The rows

| Row | What |
|-----|------|
| config | `$OCIO` (when set), OCIO's built-in configs (ACES studio and CG), the config in use, or none (plain sRGB) |
| display | the config's displays (sRGB, Rec.1886, P3, …) |
| view | the display's views (ACES 1.0 / 2.0 SDR, Un-tone-mapped, Raw, …) |
| look | the config's looks, or none |
| input | the input colour space of the source shown |

`j` / `k` move between rows, `h` / `l` step a row's value, `Enter` opens a fuzzy list of
its values (configs can have dozens of colour spaces): type to filter, `Enter` picks. In
the config row's list you can also type the path to a config file and press `Enter`.

"Plain sRGB" means linear values go through the sRGB curve, like Nuke's default sRGB
viewer.

## Input colour space

By default the config's file rules decide each image's input colour space, from its path
(a studio config often maps render folders or extensions to a space). rega doesn't read
a colour space from the EXR header. The input row overrides the rules for the source
shown (a whole sequence), for this session.

Integer images (PNG, JPEG, and 8- or 16-bit TIFF) are already display-encoded. rega
decodes them from their sRGB codes once, so the input colour space applies to those
codes and the curve isn't applied twice.

## File values and shown values

The [inspector](inspector.md) always reads the file's own values (scene-linear, in the
input colour space) plus the colour shown on screen. `s` in the viewer turns the view
transform off to show the raw values on the image too, and back on; see
[Display](../viewer/display.md#view-transform).

## ACES

**With your studio's config.** Point `$OCIO` at it, as for Nuke:

```bash
export OCIO=/studio/config/config.ocio
rega shots/
```

rega then shows its displays, views and looks, and its file rules pick each image's
input colour space, the same as in the other apps reading that config.

**Without one.** OCIO comes with two ACES configs built in, nothing to download:

| Config row | What it is |
|------------|------------|
| OCIO studio config (ACES, built in) | `ocio://studio-config-latest`: the full set of ACES colour spaces, displays and views |
| OCIO CG config (ACES, built in) | `ocio://cg-config-latest`: a smaller set for CG work |

Pick one in the config row (`6`, then `h` / `l` on the first row); rega remembers it for
next time. The display defaults to the config's default display (sRGB for the built-in
ones), and the view to its default for that display, the ACES SDR output transform.

> [!WARNING]
> The built-in configs' file rules read every image as **ACES2065-1** (AP0). Renders are
> usually **ACEScg** (AP1): Arnold, Karma, RenderMan and V-Ray in an ACES setup all write
> it. Shown as ACES2065-1, an ACEScg render looks slightly off in hue and saturation. Set
> the input row to `ACEScg` (`Enter`, type `acescg`, `Enter`). That holds for this
> session; for good, use a config whose file rules say ACEScg for your renders.

## What's remembered

rega remembers the config you picked last, and per config its display, view and look,
in `~/.local/state/rega/colour`. `$OCIO`, when set, wins at start-up.

## Speed

The transform is baked into a 3D LUT, so an ACES 2.0 view costs the same per pixel as
plain sRGB, and sequences play at full speed through it.
