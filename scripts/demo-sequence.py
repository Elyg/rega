#!/usr/bin/env python3
"""Render a small multi-layer EXR sequence for demos and the README.

Three spheres orbit over a checkered floor under a moving light, ray traced
with numpy. Every frame carries the AOVs a renderer would write: beauty
(RGBA), diffuse, specular, albedo, N (world normals), Z (depth) and mask
(one channel per sphere).

    python3 scripts/demo-sequence.py [out_dir] [--frames N] [--size WxH]

Run through `just demo-sequence`, which provides numpy and OpenEXR.
"""

import argparse
import math
from pathlib import Path

import numpy as np
import OpenEXR

FIRST_FRAME = 1001
FPS = 24.0

# (orbit radius, orbit phase, height, sphere radius, colour)
SPHERES = [
    (1.5, 0.0, 0.70, 0.70, (0.85, 0.20, 0.15)),
    (1.5, 2.1, 0.55, 0.55, (0.15, 0.65, 0.70)),
    (1.5, 4.2, 0.60, 0.60, (0.90, 0.70, 0.20)),
]
SKY_TOP = np.array([0.20, 0.35, 0.75])
SKY_HORIZON = np.array([0.80, 0.85, 0.95])
FAR = 1e9


def normalize(v):
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def hit_sphere(orig, dirs, centre, radius):
    """Distance along each ray to the sphere, or FAR."""
    oc = orig - centre
    b = np.einsum("...i,...i", dirs, oc)
    c = np.einsum("...i,...i", oc, oc) - radius * radius
    disc = b * b - c
    t = -b - np.sqrt(np.maximum(disc, 0.0))
    return np.where((disc > 0) & (t > 1e-4), t, FAR)


def hit_floor(orig, dirs):
    with np.errstate(divide="ignore", invalid="ignore"):
        t = -orig[..., 1] / dirs[..., 1]
    return np.where((t > 1e-4) & np.isfinite(t), t, FAR)


def render(frame, count, width, height):
    phase = 2 * math.pi * frame / count
    spheres = [
        (
            np.array([r * math.cos(a + phase), y, r * math.sin(a + phase)]),
            rad,
            np.array(col),
        )
        for r, a, y, rad, col in SPHERES
    ]
    light = np.array([4 * math.cos(-phase * 0.5), 5.0, 4 * math.sin(-phase * 0.5)])

    # Camera rays.
    eye = np.array([0.0, 2.2, -6.0])
    fwd = normalize(np.array([0.0, 0.5, 0.0]) - eye)
    right = normalize(np.cross(np.array([0.0, 1.0, 0.0]), fwd))
    up = np.cross(fwd, right)
    half = math.tan(math.radians(20))
    xs = (np.arange(width) + 0.5) / width * 2 - 1
    ys = 1 - (np.arange(height) + 0.5) / height * 2
    px, py = np.meshgrid(xs * half * width / height, ys * half)
    dirs = normalize(fwd + px[..., None] * right + py[..., None] * up)
    orig = np.broadcast_to(eye, dirs.shape)

    # Nearest hit: 0 = floor, 1.. = spheres, -1 = sky.
    ts = [hit_floor(orig, dirs)] + [hit_sphere(orig, dirs, c, r) for c, r, _ in spheres]
    ts = np.stack(ts)
    obj = np.argmin(ts, axis=0)
    t = np.min(ts, axis=0)
    sky = t >= FAR
    obj[sky] = -1
    t = np.where(sky, 0.0, t)
    pos = orig + dirs * t[..., None]

    normal = np.zeros_like(dirs)
    albedo = np.zeros_like(dirs)
    normal[obj == 0] = (0.0, 1.0, 0.0)
    checker = (np.floor(pos[..., 0]) + np.floor(pos[..., 2])) % 2
    floor_col = np.where(checker[..., None] > 0, 0.55, 0.35) * np.ones(3)
    albedo[obj == 0] = floor_col[obj == 0]
    for i, (c, r, col) in enumerate(spheres, start=1):
        m = obj == i
        normal[m] = (pos[m] - c) / r
        albedo[m] = col

    # Hard shadows from the spheres.
    to_light = normalize(light - pos)
    shadow_orig = pos + normal * 1e-3
    lit = np.ones(t.shape, dtype=bool)
    for c, r, _ in spheres:
        lit &= hit_sphere(shadow_orig, to_light, c, r) >= FAR

    geo = ~sky
    ndl = np.clip(np.einsum("...i,...i", normal, to_light), 0, 1) * lit
    diffuse = albedo * (0.12 + 0.95 * ndl)[..., None]
    half_vec = normalize(to_light - dirs)
    ndh = np.clip(np.einsum("...i,...i", normal, half_vec), 0, 1)
    shine = np.where(obj > 0, 60.0, 8.0)
    spec_amt = np.where(obj > 0, 0.6, 0.05)
    specular = (spec_amt * ndh**shine * lit)[..., None] * np.ones(3)
    diffuse[~geo] = 0
    specular[~geo] = 0

    sky_col = SKY_HORIZON + (SKY_TOP - SKY_HORIZON) * np.clip(dirs[..., 1:2] * 3, 0, 1)
    beauty = np.where(geo[..., None], diffuse + specular, sky_col)
    # Fade the floor into the sky with distance.
    fog = np.clip((t - 8) / 14, 0, 1)[..., None] * (obj == 0)[..., None]
    beauty = beauty * (1 - fog) + sky_col * fog

    h = np.float16
    ch = {
        "R": beauty[..., 0].astype(h),
        "G": beauty[..., 1].astype(h),
        "B": beauty[..., 2].astype(h),
        "A": np.ones(t.shape, h),
    }
    for name, img in (("diffuse", diffuse), ("specular", specular), ("albedo", albedo)):
        for k, axis in zip("RGB", range(3)):
            ch[f"{name}.{k}"] = img[..., axis].astype(h)
    for k, axis in zip("XYZ", range(3)):
        ch[f"N.{k}"] = normal[..., axis].astype(h)
    ch["Z"] = np.where(geo, t, 0.0).astype(np.float32)
    for k, i in zip("RGB", range(1, 4)):
        ch[f"mask.{k}"] = (obj == i).astype(h)
    return ch


def main():
    ap = argparse.ArgumentParser(description="Render the rega demo EXR sequence.")
    ap.add_argument("out", nargs="?", default="demos/rega-sequence")
    ap.add_argument("--frames", type=int, default=48)
    ap.add_argument("--size", default="960x540")
    args = ap.parse_args()
    width, height = (int(v) for v in args.size.split("x"))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for i in range(args.frames):
        header = {
            "compression": OpenEXR.ZIP_COMPRESSION,
            "type": OpenEXR.scanlineimage,
            "framesPerSecond": (int(FPS), 1),
            "software": "rega scripts/demo-sequence.py",
        }
        path = out / f"shot.{FIRST_FRAME + i:04d}.exr"
        with OpenEXR.File(header, render(i, args.frames, width, height)) as f:
            f.write(str(path))
        print(path)


if __name__ == "__main__":
    main()
