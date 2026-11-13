#!/usr/bin/env python3
"""MIT. CREATURE's boot splash: eyes open in the dark, blink, then the creature roars (the wah wide open).

Renders the source frames with the app's own rig (tools/splash_frames.cpp, host clang++), posterises the painting to
a palette picked from it (Lab k-means, 2-D majority filter), draws the HUD back crisp, and writes src/splash_data.h
(RSP1, <= 24 KiB) plus build/splash.gif decoded from those exact bytes.

  /opt/homebrew/bin/python3.11 tools/make_splash.py
"""
import os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from scipy import ndimage
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build'   # previews; created when the script runs

import splash_rsp as S   # noqa: E402
from splash_rsp import to_lab   # noqa: E402

SDK = Path(os.environ['RESONA_SDK'])
GROUND = (37, 10, 33)                       # the app's kGround
INK, GREEN = (255, 248, 219), (166, 189, 83)
EYE_BOXES = [(84, 36, 140, 86), (176, 42, 232, 82)]   # screen boxes around the two eyeballs at rest (x0, y0, x1, y1)


def render():
    tmp = Path(tempfile.mkdtemp()); exe = tmp / 'frames'
    subprocess.run(['clang++', '-std=c++17', '-O2', '-DRESONA_LAB_HOST', f'-I{SDK}', f'-I{ROOT / "src"}', '-include', 'cmath',
                    str(ROOT / 'tools' / 'splash_frames.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(tmp / 'f.raw')], check=True)
    return np.fromfile(tmp / 'f.raw', np.uint16).reshape(-1, 172, 320)


def palette(frames, k=11, seed=3):
    """The ground, plus k-1 colours picked (Lab k-means) from the creature's own pixels: the background vignette
    is flattened to the ground, so it costs one run and no palette entries."""
    g = to_lab(np.array([[GROUND]], np.float32)).reshape(3)
    px = np.concatenate([f.reshape(-1, 3) for f in frames]).astype(np.float32)
    lab = to_lab(px.reshape(-1, 1, 3)).reshape(-1, 3)
    body = np.sqrt(((lab - g) ** 2).sum(-1)) > 14
    lb, pb = lab[body], px[body]
    rng = np.random.default_rng(seed); cent = np.vstack([g, lb[rng.choice(len(lb), k - 1, replace=False)]])
    for _ in range(25):
        d = ((lb[::5, None] - cent[None]) ** 2).sum(-1).argmin(1)
        for j in range(1, k):
            m = d == j
            if m.any(): cent[j] = lb[::5][m].mean(0)
    d = ((lb[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
    rgb = np.array([GROUND] + [pb[d == j].mean(0) if (d == j).any() else GROUND for j in range(1, k)])
    return cent, rgb.astype(np.uint8)


def quantise(rgb, cent, pal, smooth=5):
    lab = to_lab(rgb)
    idx = ((lab[..., None, :] - cent[None, None]) ** 2).sum(-1).argmin(-1)
    for _ in range(2):   # majority filter: speckle goes, outlines stay
        votes = np.stack([ndimage.uniform_filter((idx == j).astype(np.float32), smooth) for j in range(len(pal))])
        idx = votes.argmax(0)
    return idx


def hud_mask(hud_rgb):
    lum = hud_rgb.astype(np.int32).sum(-1)
    out = np.zeros(hud_rgb.shape[:2], np.int8)
    is_green = (hud_rgb[..., 1].astype(int) > hud_rgb[..., 0].astype(int) + 20)
    out[(lum > 330) & ~is_green] = 1; out[(lum > 250) & is_green] = 2
    return out


def compose(idx, pal, hud=None):
    rgb = pal[idx].copy()
    if hud is not None:
        rgb[hud == 1] = INK; rgb[hud == 2] = GREEN
    return S.rgb565(rgb)


def main():
    raw = render()
    rest, blink, open_, hud_rest, hud_open = [S.rgb888(f) for f in raw]
    cent, pal = palette([rest, open_], k=10)
    q_rest, q_blink, q_open = (quantise(f, cent, pal) for f in (rest, blink, open_))
    ground = np.zeros((172, 320), np.int64)
    # eyes in the dark: only the eye whites and pupils inside the two eye boxes, everything else the ground
    light = [j for j in range(len(pal)) if pal[j].astype(int).sum() > 480]
    dark = [j for j in range(len(pal)) if pal[j].astype(int).sum() < 120 and j != 0]

    def eyes(q):
        e = ground.copy()
        for x0, y0, x1, y1 in EYE_BOXES:
            box = q[y0:y1, x0:x1]; keep = np.isin(box, light)
            keep = ndimage.binary_dilation(keep, iterations=3) & np.isin(box, light + dark)
            e[y0:y1, x0:x1][keep] = box[keep]
        return e
    frames = [
        (320, compose(ground, pal)),                         # the dark
        (260, compose(eyes(q_rest), pal)),                   # eyes open
        (90, compose(ground, pal)),                          # blink
        (240, compose(eyes(q_rest), pal)),                   # eyes again
        (380, compose(q_rest, pal)),                         # the creature appears
        (900, compose(q_open, pal, hud_mask(hud_open))),     # the roar: wah open, WAAAH
    ]
    for hold, f in frames: print(f'  frame {hold:4d} ms: {S.frame_bytes(f):6d} bytes')
    data = S.encode(frames, duration_ms=sum(h for h, _ in frames))
    S.write_header(data, ROOT / 'src' / 'splash_data.h')
    OUT.mkdir(exist_ok=True)
    S.preview(S.decode(data), OUT / 'splash.gif', scale=2)
    from PIL import Image
    Image.fromarray(S.rgb888(frames[-1][1])).resize((640, 344), Image.NEAREST).save(OUT / 'splash-final.png')
    print(f'{len(data)} bytes of {S.MAX_BYTES}')


if __name__ == '__main__':
    main()
