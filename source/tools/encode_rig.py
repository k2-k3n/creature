#!/usr/bin/env python3
"""Cut the approved CREATURE painting into rig layers and bake the corner type: writes src/rig_art.h.

Everything on screen comes from assets/creature-reference.png. The tool fits a creature-free background
(8 polynomial terms), separates the body with the pupils painted out of the eyeballs, cuts two pupil
sprites, eyeball masks with fitted ellipses for the lids, and the six meter segments, all on the box's
1x pixel grid. The corner type is Montserrat Bold (SIL OFL, assets/Montserrat-OFL.txt), thickened and
widened to match the wordmark, which is kept from src/hud_art.h.

Optional development tool: needs Python 3, numpy, scipy and Pillow. Builds use the generated header.
  python3 tools/encode_rig.py            (from the project folder)
"""
import argparse, re
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

ap = argparse.ArgumentParser()
ap.add_argument('--ref', default='assets/creature-reference.png')
ap.add_argument('--font', default='assets/Montserrat-Bold.ttf')
ap.add_argument('--hud', default='src/hud_art.h', help='source of the wordmark glyph')
ap.add_argument('--out', default='src/rig_art.h')
args = ap.parse_args()

A = np.asarray(Image.open(args.ref).convert('RGB')).astype(np.float32)
H0, W0 = A.shape[:2]
SW, SH = 320, 172
K = SW / W0                                            # reference px -> screen px
yy, xx = np.mgrid[0:H0, 0:W0]
lum = A.mean(-1); sat = A.max(-1) - A.min(-1)

# ---- background: a quadratic-ish surface fitted to the empty purple ------------------------------
def design(x, y):
    x = x / W0 - .5; y = y / H0 - .5
    return np.stack([np.ones_like(x), x, y, x * x, x * y, y * y, x * x * y, x * y * y], -1)
pick = np.zeros((H0, W0), bool); pick[::6, ::6] = True
pick &= (A.sum(-1) < 170) & ~((xx > 300) & (xx < 1450) & (yy > 20))
coef = np.linalg.lstsq(design(xx[pick].astype(np.float32), yy[pick].astype(np.float32)), A[pick], rcond=None)[0]
BG = design(xx.astype(np.float32), yy.astype(np.float32)) @ coef

# ---- the creature: far from the ground colour, connected to the body; soft, un-mixed edge ---------
dist = np.sqrt(((A - BG) ** 2).sum(-1))
cand = (dist > 34) & ~((xx < 380) & (yy > 560)) & ~((xx > 1420) & (yy > 760)) & ~(yy < 20)
lab, _ = ndimage.label(cand)
body = ndimage.binary_opening(ndimage.binary_fill_holes(lab == lab[600, 857]), iterations=2)
band = ndimage.binary_dilation(body, iterations=3) & ~ndimage.binary_erosion(body, iterations=3)
alpha = body.astype(np.float32); alpha[band] = np.clip((dist[band] - 14) / 40, 0, 1)
a3 = np.clip(alpha, .05, 1)[..., None]
CRE = np.where(alpha[..., None] >= .999, A, np.clip((A - BG * (1 - a3)) / a3, 0, 255))

# ---- eyes: eyeball masks, pupils painted out (harmonic fill + rim shade), pupil sprites, ellipses ----
def ellipse(cx, cy, rx, ry): return ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 <= 1
EYES = {'L': dict(box=ellipse(594, 330, 140, 140), pc=(642, 334), pr=68, seed=(540, 330)),
        'R': dict(box=ellipse(1098, 342, 135, 112), pc=(1034, 332), pr=58, seed=(1130, 350))}
eyes = {}
for k, e in EYES.items():
    white = e['box'] & (lum > 110) & (sat < 115) & (A[..., 0] >= A[..., 1] - 5)
    white = ndimage.binary_closing(white, iterations=3)
    wl, _ = ndimage.label(white); white = wl == wl[e['seed'][1], e['seed'][0]]
    pupil = ellipse(*e['pc'], e['pr'], e['pr']) & e['box'] & ~((lum > 175) & (sat < 60) & ~ellipse(*e['pc'], e['pr'] * .72, e['pr'] * .72))
    pupil = ndimage.binary_closing(pupil, iterations=2)
    ball = ndimage.binary_fill_holes(white | pupil) & e['box']
    fill = ndimage.binary_dilation(pupil, iterations=11) & ball
    ys, xs = np.nonzero(ball); y0, y1, x0, x1 = ys.min() - 2, ys.max() + 3, xs.min() - 2, xs.max() + 3
    sub = CRE[y0:y1, x0:x1].copy(); F = fill[y0:y1, x0:x1]; Bm = ball[y0:y1, x0:x1]
    S2, F2, B2 = sub[::2, ::2].copy(), F[::2, ::2], Bm[::2, ::2]
    S2[F2] = S2[B2 & ~F2].mean(0)
    for _ in range(2500):
        acc = np.zeros_like(S2); cnt = np.zeros(S2.shape[:2], np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(np.roll(S2, dy, 0), dx, 1) * np.roll(np.roll(B2, dy, 0), dx, 1)[..., None]
            cnt += np.roll(np.roll(B2, dy, 0), dx, 1)
        upd = F2 & (cnt > 0); S2[upd] = acc[upd] / cnt[upd][:, None]
    up = np.asarray(Image.fromarray(np.clip(S2, 0, 255).astype(np.uint8)).resize((sub.shape[1], sub.shape[0]), Image.BILINEAR)).astype(np.float32)
    w = np.clip(ndimage.gaussian_filter(F.astype(np.float32), 3.0) * 1.5, 0, 1)[..., None] * Bm[..., None]
    shade = 1 - .55 * np.exp(-ndimage.distance_transform_edt(Bm)[..., None] / 16.0)
    CRE[y0:y1, x0:x1] = sub * (1 - w) + up * shade * w
    pcx, pcy, pr = *e['pc'], e['pr']
    sx0, sy0, sx1, sy1 = pcx - pr - 4, pcy - pr - 4, pcx + pr + 5, pcy + pr + 5
    d = np.hypot(xx[sy0:sy1, sx0:sx1] - pcx, yy[sy0:sy1, sx0:sx1] - pcy)
    sal = ndimage.gaussian_filter(np.clip((pr - d) / 3 + .5, 0, 1) * pupil[sy0:sy1, sx0:sx1], .8)
    by_, bx_ = np.nonzero(ball); mx, my = bx_.mean(), by_.mean()
    ev, evec = np.linalg.eigh(np.cov(np.stack([bx_ - mx, by_ - my])))
    ang = float(np.arctan2(evec[1, 1], evec[0, 1]))
    ang = ang - np.pi if ang > np.pi / 2 else ang + np.pi if ang < -np.pi / 2 else ang
    eyes[k] = dict(ball=ball, spr=A[sy0:sy1, sx0:sx1].copy(), sal=sal, sxy=(sx0, sy0), ell=(mx, my, 2 * np.sqrt(ev[1]), 2 * np.sqrt(ev[0]), ang))

# ---- meter segments (they light from the bottom) ----------------------------------------------------
meter = (xx > 1160) & (xx < 1330) & (yy > 340) & (yy < 700) & (A[..., 0] >= A[..., 1] - 6) & (lum > 120)
meter = ndimage.binary_opening(meter, iterations=2)
mlab, mn = ndimage.label(meter)
segs = sorted([(ndimage.center_of_mass(meter, mlab, i), i) for i in range(1, mn + 1) if (mlab == i).sum() > 800], key=lambda c: -c[0][0])
SEG = np.zeros((H0, W0), np.uint8)
for order, (_, i) in enumerate(segs[:6]): SEG[ndimage.binary_dilation(mlab == i, iterations=2)] = order + 1

# ---- down to the screen grid ----------------------------------------------------------------------------
def to_screen(arr):
    if arr.ndim == 2:
        return np.asarray(Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), 'L').resize((SW, SH), Image.LANCZOS)).astype(np.float32) / 255
    return np.asarray(Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), 'RGB').resize((SW, SH), Image.LANCZOS)).astype(np.float32)
bodyA = to_screen(alpha)
bodyRGB = to_screen(CRE * alpha[..., None]) / np.maximum(bodyA[..., None], 1 / 255)
ys, xs = np.nonzero(bodyA > .004)
BX, BY, BX1, BY1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
BW, BH = BX1 - BX, BY1 - BY
def crop(a): return a[BY:BY1, BX:BX1]
def rgb565(rgb):
    q = np.clip(rgb + .5, 0, 255).astype(np.uint16)
    return (q[..., 0] >> 3 << 11) | (q[..., 1] >> 2 << 5) | (q[..., 2] >> 3)

# pupils on their own small grid, aligned to the screen grid
pupils = {}
for k in 'LR':
    e = eyes[k]; sx0, sy0 = e['sxy']
    x0s, y0s = int(np.floor(sx0 * K)), int(np.floor(sy0 * K))
    w = int(np.ceil((sx0 + e['sal'].shape[1]) * K)) - x0s + 1; h = int(np.ceil((sy0 + e['sal'].shape[0]) * K)) - y0s + 1
    full_a = np.zeros((H0, W0), np.float32); full_a[sy0:sy0 + e['sal'].shape[0], sx0:sx0 + e['sal'].shape[1]] = e['sal']
    full_c = np.zeros((H0, W0, 3), np.float32); full_c[sy0:sy0 + e['sal'].shape[0], sx0:sx0 + e['sal'].shape[1]] = e['spr'] * e['sal'][..., None]
    pa = to_screen(full_a); pc = to_screen(full_c) / np.maximum(pa[..., None], 1 / 255)
    pupils[k] = dict(x=x0s - BX, y=y0s - BY, w=w, h=h, a=pa[y0s:y0s + h, x0s:x0s + w], rgb=pc[y0s:y0s + h, x0s:x0s + w])
balls = {}
for k in 'LR':
    m = to_screen(eyes[k]['ball'].astype(np.float32))
    ys2, xs2 = np.nonzero(m > .004)
    x0b, y0b, x1b, y1b = xs2.min(), ys2.min(), xs2.max() + 1, ys2.max() + 1
    mx, my, rx, ry, ang = eyes[k]['ell']
    balls[k] = dict(x=x0b - BX, y=y0b - BY, w=x1b - x0b, h=y1b - y0b, m=m[y0b:y1b, x0b:x1b],
                    ell=(mx * K - BX, my * K - BY, rx * K, ry * K, ang))
seg = np.asarray(Image.fromarray(SEG).resize((SW, SH), Image.NEAREST))
ys3, xs3 = np.nonzero(seg)
SX, SY, SX1, SY1 = xs3.min(), ys3.min(), xs3.max() + 1, ys3.max() + 1

# ---- type: Montserrat Bold baked to match the wordmark -----------------------------------------------
def bake(cap, chars, track, ss=8, stretch=1.10):
    size = cap * ss * 1.4
    for _ in range(8):
        f = ImageFont.truetype(args.font, int(size)); b = f.getbbox('H', stroke_width=ss // 2); size *= cap * ss / (b[3] - b[1])
    f = ImageFont.truetype(args.font, int(size)); hb = f.getbbox('H', stroke_width=ss // 2)
    out = []
    for ch in chars:
        if ch == ' ': out.append((0, round(cap * .32), b'')); continue
        im = Image.new('L', (int(size * 2), int(size * 2)))
        ImageDraw.Draw(im).text((ss * 2, 0), ch, font=f, fill=255, stroke_width=ss // 2, stroke_fill=255)
        bb = im.getbbox(); w = max(1, round((bb[2] - bb[0]) / ss * stretch))
        g = im.crop((bb[0], hb[1], bb[2], hb[3])).resize((w, cap), Image.LANCZOS)
        out.append((w, w + track, np.asarray(g, np.uint8).tobytes()))
    return out
FONTS = {'num': (28, '0123456789', 1.0), 'label': (9, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ: ', .7), 'small': (8, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ: ', .6)}
hud = open(args.hud).read()
wordmark = [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', re.search(r'wordmark_alpha\[\]\s*=\s*\{(.*?)\};', hud, re.S).group(1))]

# ---- write the header --------------------------------------------------------------------------------
def arr(ctype, name, values, per=24):
    vals = [str(int(v)) for v in np.asarray(values).ravel()]
    lines = [','.join(vals[i:i + per]) for i in range(0, len(vals), per)]
    return 'alignas(4) static const %s %s[%d]={\n%s};\n' % (ctype, name, len(vals), ',\n'.join(lines))
o = ['// Generated by tools/encode_rig.py from assets/creature-reference.png and assets/Montserrat-Bold.ttf\n'
     '// (SIL OFL 1.1, see assets/Montserrat-OFL.txt). Do not edit by hand. MIT for the code around it.\n'
     '#pragma once\n#include <stdint.h>\nnamespace rig_art {\n']
o.append('constexpr int kBodyX=%d,kBodyY=%d,kBodyW=%d,kBodyH=%d;\n' % (BX, BY, BW, BH))
o.append('constexpr float kRefScale=%.8ff;   // reference px -> screen px\n' % K)
o.append('static const float kBgCoef[3][8]={%s};\n' % ','.join('{' + ','.join('%.5ff' % v for v in coef[:, c]) + '}' for c in range(3)))
o.append(arr('uint16_t', 'body_rgb', crop(rgb565(bodyRGB))))
o.append(arr('uint8_t', 'body_a', np.clip(crop(bodyA) * 255 + .5, 0, 255)))
for k in 'LR':
    p, b = pupils[k], balls[k]
    o.append('constexpr int kPupil%sX=%d,kPupil%sY=%d,kPupil%sW=%d,kPupil%sH=%d;\n' % (k, p['x'], k, p['y'], k, p['w'], k, p['h']))
    o.append(arr('uint16_t', 'pupil%s_rgb' % k, rgb565(p['rgb'])))
    o.append(arr('uint8_t', 'pupil%s_a' % k, np.clip(p['a'] * 255 + .5, 0, 255)))
    o.append('constexpr int kBall%sX=%d,kBall%sY=%d,kBall%sW=%d,kBall%sH=%d;\n' % (k, b['x'], k, b['y'], k, b['w'], k, b['h']))
    o.append(arr('uint8_t', 'ball%s' % k, np.clip(b['m'] * 255 + .5, 0, 255)))
    o.append('static const float kEye%s[5]={%s};   // centre x, y (body px), semi-axes, tilt (rad)\n' % (k, ','.join('%.4ff' % v for v in b['ell'])))
o.append('constexpr int kSegX=%d,kSegY=%d,kSegW=%d,kSegH=%d;\n' % (SX - BX, SY - BY, SX1 - SX, SY1 - SY))
o.append(arr('uint8_t', 'seg', seg[SY:SY1, SX:SX1]))
o.append(arr('uint8_t', 'wordmark', wordmark))
o.append('struct Font{int h;const char *chars;const uint8_t *w;const uint16_t *adv10;const uint16_t *off;const uint8_t *alpha;};\n')
for name, (cap, chars, track) in FONTS.items():
    gl = bake(cap, chars, track); offs, blob = [], b''
    for w, adv, a in gl: offs.append(len(blob)); blob += a
    o.append(arr('uint8_t', name + '_w', [g[0] for g in gl]))
    o.append(arr('uint16_t', name + '_adv10', [round(g[1] * 10) for g in gl]))
    o.append(arr('uint16_t', name + '_off', offs))
    o.append(arr('uint8_t', name + '_alpha', list(blob)))
    o.append('static const Font %s={%d,"%s",%s_w,%s_adv10,%s_off,%s_alpha};\n' % (name, cap, chars, name, name, name, name))
o.append('}\n')
open(args.out, 'w').write(''.join(o))
print('wrote', args.out, 'body', (BW, BH), 'at', (BX, BY), 'meter segments', len(segs))
