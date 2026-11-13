// MIT. CREATURE as a 2D rig of the approved painting: every pixel is sampled from rig_art.h.
//
// Acting is a few continuous controls driven by springs (they accelerate in, settle out and overshoot a
// little), so any retarget mid-move stays smooth:
//   open    mouth opening (1 = as painted)   tilt/lift  head back as the wah opens
//   inflate body volume (inhale)             breath     slow squash and stretch
//   lids    blink / squint per eye           look       pupil offset
//   jaw     pick bounce                      shake      (unused)
// The mouth is a backward-mapped Gaussian RBF warp (handles on the lips, head and eyes, anchors at the
// feet), evaluated on a coarse grid from a precomputed basis. Lids stretch the painted lid rim down over
// each eyeball along its own tilted axis. One bilinear sample per screen pixel.
#pragma once
#include <stdint.h>
#include <resona/platform.h>
#include <resona/math.h>
#include "rig_art.h"

namespace rig {
using namespace rig_art;

constexpr int kHandles = 13, kGrid = 6;
constexpr int kGW = kBodyW / kGrid + 3, kGH = kBodyH / kGrid + 3;

struct Spring { float x, v; };
struct Rig {
 Spring open, tilt, lift, inflate, squint;
 float jaw, shake, breathT, pick, look[2], lookT[2], lids[2];
 float pedal; uint32_t t, blinkT, nextBlink, nextLook; int blinking; uint32_t seed;
 float gx[kGW * kGH], gy[kGW * kGH];
};
// Heap: the background plate and the RBF basis on the warp grid.
// The screen region the creature can reach, and its source map on an 8 px grid.
constexpr int kSX0 = kBodyX - 10 > 0 ? kBodyX - 10 : 0, kSX1 = kBodyX + kBodyW + 10 < 320 ? kBodyX + kBodyW + 10 : 320;
constexpr int kSY0 = kBodyY - 14 > 0 ? kBodyY - 14 : 0, kSY1 = kBodyY + kBodyH + 2 < 172 ? kBodyY + kBodyH + 2 : 172;
constexpr int kMW = (kSX1 - kSX0 + 7) / 8 + 1, kMH = (kSY1 - kSY0 + 7) / 8 + 1;
// Packed texels: premultiplied RGB565 in the low half, alpha 0..32 above it.
struct Shared {
 uint16_t bg[320 * 172]; float phi[kGW * kGH * kHandles]; float hx[kHandles], hy[kHandles], inv[kHandles * kHandles];
 uint32_t texP[kBodyW * kBodyH];   // premultiplied RGB565 of (x, y) low, (x, y + 1) high
 uint8_t alpha5[kBodyW * kBodyH];   // 0..32
 uint32_t pupL[kPupilLW * kPupilLH], pupR[kPupilRW * kPupilRH];
 int32_t mapX[kMW * kMH], mapY[kMW * kMH];
 uint8_t occ[((kBodyW + 7) / 8) * ((kBodyH + 7) / 8)];   // 8x8 blocks, dilated by one block: 0 empty, 1 ink, 2 all opaque
 uint32_t msBg, msLoop;   // bench: time of the last background copy and creature loop
};
inline uint32_t pack(uint16_t c, uint8_t a8) {
 const uint32_t a = (a8 * 32u + 127u) / 255u;
 const uint32_t r = ((c >> 11) * a + 16) >> 5, g = (((c >> 5) & 63) * a + 16) >> 5, b = ((c & 31) * a + 16) >> 5;
 return (a << 16) | (r << 11) | (g << 5) | b;
}

inline float sq(float x) { return x * x; }
inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
inline uint32_t next(uint32_t &s) { s = s * 1664525u + 1013904223u; return s >> 8; }
inline float rnd(uint32_t &s) { return static_cast<float>(next(s) & 0xffff) / 65536.f; }

// handles in reference pixels (the painting is 1710 x 920)
static const float kHandleRef[kHandles][2] = {
 {857, 425}, {857, 760}, {600, 600}, {1115, 600}, {857, 120}, {595, 330}, {1100, 345},
 {520, 885}, {1200, 885}, {857, 905}, {340, 520}, {1410, 520}, {857, 860}};
constexpr float kSigmaRef = 150.f;

inline void init(Rig &r, Shared &sh, float growl01) {
 r = Rig{};
 r.open.x = .5f + .15f * growl01; r.seed = 0x5eed1234u; r.nextBlink = 1800; r.nextLook = 900;
 // background plate, ordered-dithered to RGB565
 static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
 for (int y = 0; y < 172; ++y) for (int x = 0; x < 320; ++x) {
  const float u = (x + .5f) / 320.f - .5f, v = (y + .5f) / 172.f - .5f;
  const float t[8] = {1, u, v, u * u, u * v, v * v, u * u * v, u * v * v};
  float c[3];
  for (int k = 0; k < 3; ++k) { c[k] = 0; for (int i = 0; i < 8; ++i) c[k] += kBgCoef[k][i] * t[i]; }
  const float th = (bayer[(y & 3) * 4 + (x & 3)] + .5f) / 16.f;
  const int R = clampf(c[0] / 8.f + th, 0, 31), G = clampf(c[1] / 4.f + th, 0, 63), B = clampf(c[2] / 8.f + th, 0, 31);
  sh.bg[y * 320 + x] = static_cast<uint16_t>((R << 11) | (G << 5) | B);
 }
 // the RBF basis: handle positions, the inverse of their Gaussian matrix, and the basis on the grid
 const float sig = kSigmaRef * kRefScale;
 for (int k = 0; k < kHandles; ++k) { sh.hx[k] = kHandleRef[k][0] * kRefScale; sh.hy[k] = kHandleRef[k][1] * kRefScale; }
 float M[kHandles][2 * kHandles];
 for (int i = 0; i < kHandles; ++i) for (int j = 0; j < kHandles; ++j) {
  M[i][j] = resona_expf(-(sq(sh.hx[i] - sh.hx[j]) + sq(sh.hy[i] - sh.hy[j])) / (2 * sig * sig));
  M[i][kHandles + j] = i == j ? 1.f : 0.f;
 }
 for (int c = 0; c < kHandles; ++c) {
  int p = c; for (int i = c + 1; i < kHandles; ++i) if (resona_fabsf(M[i][c]) > resona_fabsf(M[p][c])) p = i;
  for (int j = 0; j < 2 * kHandles; ++j) { float tmp = M[c][j]; M[c][j] = M[p][j]; M[p][j] = tmp; }
  const float d = M[c][c]; for (int j = 0; j < 2 * kHandles; ++j) M[c][j] /= d;
  for (int i = 0; i < kHandles; ++i) if (i != c) { const float f = M[i][c]; for (int j = 0; j < 2 * kHandles; ++j) M[i][j] -= f * M[c][j]; }
 }
 for (int i = 0; i < kHandles; ++i) for (int j = 0; j < kHandles; ++j) sh.inv[i * kHandles + j] = M[i][kHandles + j];
 for (int y = 0; y < kBodyH; ++y) for (int x = 0; x < kBodyW; ++x) {
  const int i = y * kBodyW + x; const uint32_t w = pack(body_rgb[i], body_a[i]);
  const uint32_t below = y + 1 < kBodyH ? pack(body_rgb[i + kBodyW], body_a[i + kBodyW]) & 0xFFFFu : 0u;
  sh.texP[i] = (w & 0xFFFFu) | (below << 16); sh.alpha5[i] = static_cast<uint8_t>(w >> 16);
 }
 for (int i = 0; i < kPupilLW * kPupilLH; ++i) sh.pupL[i] = pack(pupilL_rgb[i], pupilL_a[i]);
 {
  constexpr int OW = (kBodyW + 7) / 8, OH = (kBodyH + 7) / 8; uint8_t ink[OW * OH] = {}, solid[OW * OH];
  for (int i = 0; i < OW * OH; ++i) solid[i] = 1;
  for (int y = 0; y < OH * 8; ++y) for (int x = 0; x < OW * 8; ++x) {
   const int a = x < kBodyW && y < kBodyH ? sh.alpha5[y * kBodyW + x] : 0;
   if (a) ink[(y >> 3) * OW + (x >> 3)] = 1;
   if (a < 32) solid[(y >> 3) * OW + (x >> 3)] = 0;
  }
  for (int j = 0; j < OH; ++j) for (int i = 0; i < OW; ++i) {
   uint8_t anyInk = 0, allSolid = 1;
   for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
    const int a = i + di, b = j + dj;
    if (a >= 0 && b >= 0 && a < OW && b < OH) { anyInk |= ink[b * OW + a]; allSolid &= solid[b * OW + a]; } else allSolid = 0;
   }
   sh.occ[j * OW + i] = allSolid ? 2 : anyInk ? 1 : 0;
  }
 }
 for (int i = 0; i < kPupilRW * kPupilRH; ++i) sh.pupR[i] = pack(pupilR_rgb[i], pupilR_a[i]);
 for (int j = 0; j < kGH; ++j) for (int i = 0; i < kGW; ++i) {
  const float X = kBodyX + (i - 1) * kGrid, Y = kBodyY + (j - 1) * kGrid;
  for (int k = 0; k < kHandles; ++k) sh.phi[(j * kGW + i) * kHandles + k] = resona_expf(-(sq(X - sh.hx[k]) + sq(Y - sh.hy[k])) / (2 * sig * sig));
 }
}

// springs, integrated in 4 sub-steps for stability
inline void spring(Spring &s, float to, float hz, float zeta, float dt) {
 const float w = 6.2831853f * hz, h = dt / 4.f;
 for (int i = 0; i < 4; ++i) { const float a = w * w * (to - s.x) - 2 * zeta * w * s.v; s.v += a * h; s.x += s.v * h; }
}

// pedal 0..1 is the wah position (the app mirrors the voice's stroke): the mouth is the pedal, "oo" at the heel
// to a wide "aa" at the toe, with the head lifting and the eyes squeezing as it opens.
inline void update(Rig &r, Shared &sh, uint32_t ms, float pedal, float activity, int growl) {
 const float dt = ms / 1000.f; r.t += ms; r.pedal = pedal;
 const float g = growl / 100.f, rest = .5f + .15f * g;          // "oo" at rest
 spring(r.open, rest + (1.12f - rest) * pedal, 3.2f, .78f, dt);   // quick enough to follow wah-wah taps
 spring(r.tilt, -.075f * pedal, 2.f, .8f, dt); spring(r.lift, 7 * pedal, 2.f, .8f, dt);
 spring(r.inflate, .03f * pedal - .01f * (1 - pedal), 1.4f, .7f, dt);
 spring(r.squint, .55f * pedal * pedal, 2.6f, .9f, dt);
 // the guitar bounces the jaw
 const float decay = resona_expf(-static_cast<float>(ms) / 110.f);
 r.pick = r.pick * decay > activity ? r.pick * decay : activity;
 r.jaw = r.pick * .9f; r.shake = 0;
 r.breathT += dt * (1 - pedal);
 // blinks and glances
 if (r.nextBlink > ms) r.nextBlink -= ms; else if (!r.blinking) { r.blinking = 1; r.blinkT = 0; r.nextBlink = 2400 + static_cast<uint32_t>(rnd(r.seed) * 3200); }
 float bl = 0;
 if (r.blinking) { r.blinkT += ms; if (r.blinkT > 190) r.blinking = 0; else bl = resona_sinf(3.14159265f * r.blinkT / 190.f); }
 if (r.nextLook > ms) r.nextLook -= ms;
 else {
  r.nextLook = 700 + static_cast<uint32_t>(rnd(r.seed) * 2200);
  if (r.pedal < .05f) { r.lookT[0] = (rnd(r.seed) - .5f) * 4.5f; r.lookT[1] = (rnd(r.seed) - .5f) * 2.5f; } else { r.lookT[0] = 0; r.lookT[1] = -1.5f; }
 }
 const float le = 1.f - resona_expf(-static_cast<float>(ms) / 60.f);
 r.look[0] += (r.lookT[0] - r.look[0]) * le; r.look[1] += (r.lookT[1] - r.look[1]) * le;
 const float sqz = r.squint.x < 0 ? 0 : r.squint.x;
 r.lids[0] = bl > sqz * .9f ? bl : sqz * .9f; r.lids[1] = bl > sqz ? bl : sqz;
 // the mouth warp: forward displacements at the handles, weights through the precomputed inverse
 const float c = 1.f - r.open.x, s = kRefScale;
 float dx[kHandles] = {}, dy[kHandles] = {};
 dy[0] = c * 92 * s; dy[1] = -c * 58 * s + r.jaw * 22 * s; dx[2] = c * 22 * s; dx[3] = -c * 22 * s; dy[2] = dy[3] = c * 10 * s;
 dy[4] = c * 64 * s; dy[5] = dy[6] = c * 62 * s; dy[12] = -c * 14 * s;
 float wx[kHandles], wy[kHandles];
 for (int i = 0; i < kHandles; ++i) { float ax = 0, ay = 0; for (int j = 0; j < kHandles; ++j) { ax += sh.inv[i * kHandles + j] * dx[j]; ay += sh.inv[i * kHandles + j] * dy[j]; } wx[i] = ax; wy[i] = ay; }
 for (int n = 0; n < kGW * kGH; ++n) { float ax = 0, ay = 0; const float *p = sh.phi + n * kHandles; for (int k = 0; k < kHandles; ++k) { ax += wx[k] * p[k]; ay += wy[k] * p[k]; } r.gx[n] = ax; r.gy[n] = ay; }
}

/* ---- sampling --------------------------------------------------------------------------------------- */
struct Eye { float ex, ey, rx, ry, ca, sa; int bx, by, bw, bh, px, py, pw, ph; const uint8_t *ball, *pa; const uint16_t *prgb; float lookScale; };
inline Eye eye(int k) {
 const float *E = k ? kEyeR : kEyeL;
 Eye e;
 const float ang = k ? E[4] - .08f : (resona_fabsf(E[2] - E[3]) / E[2] < .12f ? 0.f : E[4]);
 e.ex = E[0] + (k ? 1.f : 0.f); e.ey = E[1]; e.rx = E[2] * (k ? 1.1f : 1.f) * 1.12f; e.ry = E[3] * 1.1f; e.ca = resona_cosf(ang); e.sa = resona_sinf(ang);
 if (k) { e.bx = kBallRX; e.by = kBallRY; e.bw = kBallRW; e.bh = kBallRH; e.ball = ballR; e.px = kPupilRX; e.py = kPupilRY; e.pw = kPupilRW; e.ph = kPupilRH; e.prgb = pupilR_rgb; e.pa = pupilR_a; e.lookScale = .8f; }
 else { e.bx = kBallLX; e.by = kBallLY; e.bw = kBallLW; e.bh = kBallLH; e.ball = ballL; e.px = kPupilLX; e.py = kPupilLY; e.pw = kPupilLW; e.ph = kPupilLH; e.prgb = pupilL_rgb; e.pa = pupilL_a; e.lookScale = 1.f; }
 return e;
}

// lids: the painted lid rim just above each eyeball stretches down while the eyeball squashes below it
inline void lidWarp(const Rig &r, const Eye *eyes, float &lx, float &ly) {
 for (int k = 0; k < 2; ++k) {
  const Eye &e = eyes[k]; const float c = r.lids[k]; if (c <= .002f) continue;
  const float dx = lx - e.ex, dy = ly - e.ey, u = dx * e.ca + dy * e.sa, v = -dx * e.sa + dy * e.ca, un = u / e.rx;
  if (un <= -1.08f || un >= 1.08f) continue;
  const float prof = resona_sqrtf(clampf(1 - un * un, 0, 1)), au = resona_fabsf(un);
  const float wx = au <= .92f ? 1.f : clampf(1 - (au - .92f) / .16f, 0, 1);
  const float top = -e.ry * prof - .3f, bot = e.ry * prof + .3f, band = e.ry * .3f;
  if (v < top - band || v > bot || bot - top < .5f) continue;
  const float cl = c * wx < .985f ? c * wx : .985f, top2 = top + (bot - top) * cl;
  const float v2 = v >= top2 ? bot - (bot - v) * (bot - top) / (bot - top2 > .25f ? bot - top2 : .25f)
                             : (top - band) + (v - (top - band)) * band / (top2 - (top - band) > .25f ? top2 - (top - band) : .25f);
  lx = e.ex + u * e.ca - v2 * e.sa; ly = e.ey + u * e.sa + v2 * e.ca;
  return;
 }
}

/* ---- fixed-point drawing -------------------------------------------------------------------------- */
// Apps build with -Os; the per-pixel path is compiled for speed and its helpers are always inlined.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O2")
#endif
#define RIG_HOT inline __attribute__((always_inline))
#ifdef RIG_STATS
static uint32_t g_runsEmpty, g_runsFast, g_runsSlow;
#define RIG_COUNT(x) (++(x))
#else
#define RIG_COUNT(x) ((void)0)
#endif
// RGB565 spread so that one multiply blends all three channels with a 5-bit weight
constexpr uint32_t kSpread = 0x07E0F81Fu;
RIG_HOT uint32_t spread(uint32_t c) { return ((c & 0xFFFFu) | (c << 16)) & kSpread; }
RIG_HOT uint16_t gather(uint32_t e) { return static_cast<uint16_t>((e & 0xF81Fu) | ((e >> 16) & 0x07E0u)); }
RIG_HOT uint32_t lerp5(uint32_t a, uint32_t b, uint32_t w) { return ((a * (32 - w) + b * w) >> 5) & kSpread; }
// bilinear read of a packed texture at 16.16 coordinates (caller keeps 0 <= ix < w-1, 0 <= iy < h-1);
// returns the spread premultiplied colour and sets a (0..32)
RIG_HOT uint32_t sample(const uint32_t *t, int w, int32_t sx, int32_t sy, uint32_t &a) {
 const uint32_t fx = (sx >> 11) & 31, fy = (sy >> 11) & 31;
 const uint32_t *p = t + (sy >> 16) * w + (sx >> 16);
 const uint32_t w00 = p[0], w10 = p[1], w01 = p[w], w11 = p[w + 1];
 a = (((w00 >> 16) * (32 - fx) + (w10 >> 16) * fx) * (32 - fy) + ((w01 >> 16) * (32 - fx) + (w11 >> 16) * fx) * fy) >> 10;
 if (!a) return 0;
 return lerp5(lerp5(spread(w00), spread(w10), fx), lerp5(spread(w01), spread(w11), fx), fy);
}
RIG_HOT uint32_t spreadLo(uint32_t w) { return ((w & 0xFFFFu) | (w << 16)) & kSpread; }
RIG_HOT uint32_t spreadHi(uint32_t w) { return ((w >> 16) | (w & 0xFFFF0000u)) & kSpread; }
RIG_HOT uint32_t sampleBody(const Shared &sh, int32_t sx, int32_t sy, uint32_t &a) {
 const uint32_t fx = (sx >> 11) & 31, fy = (sy >> 11) & 31; const int i = (sy >> 16) * kBodyW + (sx >> 16);
 const uint8_t *al = sh.alpha5 + i;
 a = ((al[0] * (32 - fx) + al[1] * fx) * (32 - fy) + (al[kBodyW] * (32 - fx) + al[kBodyW + 1] * fx) * fy) >> 10;
 if (!a) return 0;
 const uint32_t L = sh.texP[i], R = sh.texP[i + 1];
 return lerp5(lerp5(spreadLo(L), spreadLo(R), fx), lerp5(spreadHi(L), spreadHi(R), fx), fy);
}
inline uint32_t mask5(const uint8_t *m, int w, int h, float x, float y) {      // bilinear 0..32
 x -= .5f; y -= .5f; if (x < 0 || y < 0 || x >= w - 1 || y >= h - 1) return 0;
 const int ix = static_cast<int>(x), iy = static_cast<int>(y); const uint32_t fx = static_cast<uint32_t>((x - ix) * 32), fy = static_cast<uint32_t>((y - iy) * 32);
 const uint8_t *p = m + iy * w + ix;
 return (((p[0] * (32 - fx) + p[1] * fx) * (32 - fy) + (p[w] * (32 - fx) + p[w + 1] * fx) * fy) >> 10) >> 3;
}

// screen point -> body coordinates: undo the body transform, then the mouth warp
struct Pose { float fx0, fy0, sx, sy, ca, sa, lift, shx; };
inline void toBody(const Rig &r, const Pose &q, float x, float y, float &lx, float &ly) {
 float X = x - q.shx, Y = y + q.lift;
 const float ux = X - q.fx0, uy = Y - q.fy0, rx = q.ca * ux + q.sa * uy, ry = -q.sa * ux + q.ca * uy;
 X = q.fx0 + rx / q.sx; Y = q.fy0 + ry / q.sy;
 const float gu = (X - kBodyX) / kGrid + 1, gv = (Y - kBodyY) / kGrid + 1;
 if (gu >= 0 && gv >= 0 && gu < kGW - 1 && gv < kGH - 1) {
  const int i = static_cast<int>(gu), j = static_cast<int>(gv); const float fu = gu - i, fv = gv - j; const int a = j * kGW + i;
  X -= (r.gx[a] * (1 - fu) + r.gx[a + 1] * fu) * (1 - fv) + (r.gx[a + kGW] * (1 - fu) + r.gx[a + kGW + 1] * fu) * fv;
  Y -= (r.gy[a] * (1 - fu) + r.gy[a + 1] * fu) * (1 - fv) + (r.gy[a + kGW] * (1 - fu) + r.gy[a + kGW + 1] * fu) * fv;
 }
 lx = X - kBodyX; ly = Y - kBodyY;
}

inline void render(const Rig &r, Shared &sh, uint16_t *px, uint32_t stride, int litSegments, uint32_t (*clock)()) {
 const uint32_t t0 = clock ? clock() : 0;
 // background: word copies
 for (int y = 0; y < 172; ++y) {
  const uint16_t *src = sh.bg + y * 320; uint16_t *dst = px + y * stride;
  if (((reinterpret_cast<uintptr_t>(dst) | reinterpret_cast<uintptr_t>(src)) & 3) == 0) {
   const uint32_t *s4 = reinterpret_cast<const uint32_t *>(src); uint32_t *d4 = reinterpret_cast<uint32_t *>(dst);
   for (int x = 0; x < 160; ++x) d4[x] = s4[x];
  } else for (int x = 0; x < 320; ++x) dst[x] = src[x];
 }
 const uint32_t t1 = clock ? clock() : 0; sh.msBg += t1 - t0;
 Pose q;
 const float breath = resona_sinf(r.breathT * 6.2831853f * .32f);
 q.fx0 = 857.f * kRefScale; q.fy0 = 900.f * kRefScale;
 q.sy = 1 + .010f * breath + r.inflate.x * .6f; q.sx = 1 - .005f * breath + r.inflate.x;
 q.ca = resona_cosf(r.tilt.x); q.sa = resona_sinf(r.tilt.x); q.lift = r.lift.x * kRefScale; q.shx = r.shake;
 // the source map at the 8 px grid nodes (pixel centres), in 16.16
 for (int j = 0; j < kMH; ++j) for (int i = 0; i < kMW; ++i) {
  float lx, ly; toBody(r, q, kSX0 + i * 8 + .5f, kSY0 + j * 8 + .5f, lx, ly);
  sh.mapX[j * kMW + i] = static_cast<int32_t>((lx - .5f) * 65536.f); sh.mapY[j * kMW + i] = static_cast<int32_t>((ly - .5f) * 65536.f);
 }
 const Eye eyes[2] = {eye(0), eye(1)};
 // each eye's box (eyeball plus the lid band above it) takes the slow path, in body pixels
 int ebx[2], eby[2]; uint32_t ebw[2], ebh[2];
 for (int k = 0; k < 2; ++k) {
  const Eye &e = eyes[k]; const int band = r.lids[k] > .002f ? static_cast<int>(e.ry * .35f) + 3 : 1;
  ebx[k] = e.bx - 2; eby[k] = e.by - band; ebw[k] = static_cast<uint32_t>(e.bw + 4); ebh[k] = static_cast<uint32_t>(e.bh + band + 2);
 }
 constexpr int OW = (kBodyW + 7) / 8;
 for (int y = kSY0; y < kSY1; ++y) {
  const int j = (y - kSY0) >> 3, fyr = (y - kSY0) & 7;
  uint16_t *row = px + y * stride;
  for (int i = 0; i + 1 < kMW; ++i) {
   const int xa = kSX0 + i * 8; if (xa >= kSX1) break;
   const int n = j * kMW + i;
   const int32_t lx0 = sh.mapX[n] + ((sh.mapX[n + kMW] - sh.mapX[n]) * fyr >> 3), ly0 = sh.mapY[n] + ((sh.mapY[n + kMW] - sh.mapY[n]) * fyr >> 3);
   const int32_t lx1 = sh.mapX[n + 1] + ((sh.mapX[n + 1 + kMW] - sh.mapX[n + 1]) * fyr >> 3), ly1 = sh.mapY[n + 1] + ((sh.mapY[n + 1 + kMW] - sh.mapY[n + 1]) * fyr >> 3);
   const int32_t dx = (lx1 - lx0) >> 3, dy = (ly1 - ly0) >> 3;
   int32_t sx = lx0, sy = ly0;
   const int xe = xa + 8 < kSX1 ? xa + 8 : kSX1;
   // classify the run once: empty, plain body (fast loop) or near an eye or the meter (careful loop)
   {
    const int32_t ex = lx0 + dx * (xe - xa - 1), ey = ly0 + dy * (xe - xa - 1);
    const int bx0 = ((lx0 < ex ? lx0 : ex) >> 16) - 1, bx1 = ((lx0 > ex ? lx0 : ex) >> 16) + 2;
    const int by0 = ((ly0 < ey ? ly0 : ey) >> 16) - 1, by1 = ((ly0 > ey ? ly0 : ey) >> 16) + 2;
    if (bx1 < 0 || by1 < 0 || bx0 >= kBodyW || by0 >= kBodyH) continue;
    bool ink = false, solid = true;
    for (int oy = (by0 < 0 ? 0 : by0) >> 3; oy <= (by1 >= kBodyH ? kBodyH - 1 : by1) >> 3; ++oy)
     for (int ox = (bx0 < 0 ? 0 : bx0) >> 3; ox <= (bx1 >= kBodyW ? kBodyW - 1 : bx1) >> 3; ++ox) { const uint8_t o = sh.occ[oy * OW + ox]; ink |= o != 0; solid &= o == 2; }
    if (!ink) { RIG_COUNT(g_runsEmpty); continue; }
    const bool inside = bx0 >= 0 && by0 >= 0 && bx1 < kBodyW - 1 && by1 < kBodyH - 1;
    const bool nearEye = (bx1 >= ebx[0] && bx0 < ebx[0] + static_cast<int>(ebw[0]) && by1 >= eby[0] && by0 < eby[0] + static_cast<int>(ebh[0])) ||
                         (bx1 >= ebx[1] && bx0 < ebx[1] + static_cast<int>(ebw[1]) && by1 >= eby[1] && by0 < eby[1] + static_cast<int>(ebh[1]));
    const bool nearSeg = bx1 >= kSegX && bx0 < kSegX + kSegW && by1 >= kSegY && by0 < kSegY + kSegH;
    if (inside && !nearEye && !nearSeg && solid) {
     // opaque interior: one paired load per new texel column, reused along the row
     RIG_COUNT(g_runsFast);
     int pix = -9999, piy = -9999; uint32_t tl = 0, bl = 0, tr = 0, br = 0;
     for (int x = xa; x < xe; ++x, sx += dx, sy += dy) {
      const int ix = sx >> 16, iy = sy >> 16;
      if (iy != piy || ix != pix) {
       const uint32_t *t = sh.texP + iy * kBodyW + ix;
       if (iy == piy && ix == pix + 1) { tl = tr; bl = br; } else { const uint32_t L = t[0]; tl = spreadLo(L); bl = spreadHi(L); }
       const uint32_t R = t[1]; tr = spreadLo(R); br = spreadHi(R); pix = ix; piy = iy;
      }
      const uint32_t fx = (sx >> 11) & 31, fy = (sy >> 11) & 31;
      row[x] = gather(lerp5(lerp5(tl, tr, fx), lerp5(bl, br, fx), fy));
     }
     continue;
    }
    if (inside && !nearEye && !nearSeg) {
     RIG_COUNT(g_runsFast);
     for (int x = xa; x < xe; ++x, sx += dx, sy += dy) {
      uint32_t a; const uint32_t c = sampleBody(sh, sx, sy, a); if (!a) continue;
      row[x] = a >= 32 ? gather(c) : gather(c + (((spread(row[x]) * (32 - a)) >> 5) & kSpread));
     }
     continue;
    }
   }
   RIG_COUNT(g_runsSlow);
   for (int x = xa; x < xe; ++x, sx += dx, sy += dy) {
    const int ix = sx >> 16, iy = sy >> 16;
    if (static_cast<uint32_t>(ix) >= static_cast<uint32_t>(kBodyW - 1) || static_cast<uint32_t>(iy) >= static_cast<uint32_t>(kBodyH - 1)) continue;
    if (!sh.occ[(iy >> 3) * OW + (ix >> 3)]) continue;
    uint32_t a, c;
    const bool inL = static_cast<uint32_t>(ix - ebx[0]) < ebw[0] && static_cast<uint32_t>(iy - eby[0]) < ebh[0];
    const bool inR = !inL && static_cast<uint32_t>(ix - ebx[1]) < ebw[1] && static_cast<uint32_t>(iy - eby[1]) < ebh[1];
    if (inL || inR) {
     // eyes: lids (when active), then the pupils inside the eyeballs
     float lx = sx / 65536.f + .5f, ly = sy / 65536.f + .5f;
     if (r.lids[inL ? 0 : 1] > .002f) lidWarp(r, eyes, lx, ly);
     const int32_t tx = static_cast<int32_t>((lx - .5f) * 65536.f), ty = static_cast<int32_t>((ly - .5f) * 65536.f);
     if (static_cast<uint32_t>(tx >> 16) >= static_cast<uint32_t>(kBodyW - 1) || static_cast<uint32_t>(ty >> 16) >= static_cast<uint32_t>(kBodyH - 1)) continue;
     c = sampleBody(sh, tx, ty, a); if (!a) continue;
     for (int k = 0; k < 2; ++k) {
      const Eye &e = eyes[k];
      if (lx < e.bx - 1 || ly < e.by - 1 || lx > e.bx + e.bw + 1 || ly > e.by + e.bh + 1) continue;
      const uint32_t ball = mask5(e.ball, e.bw, e.bh, lx - e.bx, ly - e.by); if (!ball) continue;
      const float pxf = lx - e.px - r.look[0] * e.lookScale - .5f, pyf = ly - e.py - r.look[1] - .5f;
      if (pxf < 0 || pyf < 0 || pxf >= e.pw - 1 || pyf >= e.ph - 1) continue;
      uint32_t pa; const uint32_t pc = sample(k ? sh.pupR : sh.pupL, e.pw, static_cast<int32_t>(pxf * 65536.f), static_cast<int32_t>(pyf * 65536.f), pa);
      const uint32_t pe = (pa * ball) >> 5; if (!pe) continue;
      c = (((c * (32 - pe)) >> 5) & kSpread) + (((pc * ball) >> 5) & kSpread);
     }
    } else {
     c = sampleBody(sh, sx, sy, a); if (!a) continue;
     // meter segments above the level are dimmed
     if (static_cast<uint32_t>(ix - kSegX) < static_cast<uint32_t>(kSegW) && static_cast<uint32_t>(iy - kSegY) < static_cast<uint32_t>(kSegH)) {
      const int sg = seg[(iy - kSegY) * kSegW + (ix - kSegX)]; if (sg && sg > litSegments) c = ((c * 13) >> 5) & kSpread;
     }
    }
    if (a >= 32) row[x] = gather(c);
    else row[x] = gather(c + (((spread(row[x]) * (32 - a)) >> 5) & kSpread));
   }
  }
 }
 if (clock) sh.msLoop += clock() - t1;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

inline const char *caption(const Rig &r, float activity) {
 if (r.pedal > .7f) return "WAAAH"; if (r.pedal > .35f) return "WAAH"; if (r.pedal > .02f) return "WAH";
 return activity > .2f ? "SINGING" : "HOLD: WAH";
}
inline int litSegments(const Rig &r, int growl) {
 const float g = growl / 100.f, p = r.pick * 2 > 1 ? 1 : r.pick * 2;
 return static_cast<int>(g * 6 * (.75f + .25f * p) + .5f) + static_cast<int>(r.pedal * 6 + .5f);
}

/* ---- the corner type ------------------------------------------------------------------------------------ */
inline uint16_t blend565(uint16_t dst, uint16_t src, int a) {
 const uint32_t w = static_cast<uint32_t>(a + 4) >> 3;            // 0..32
 return w >= 32 ? src : gather(lerp5(spread(dst), spread(src), w));
}
inline void alphaBlit(uint16_t *px, uint32_t stride, const uint8_t *a, int w, int h, int x, int y, uint16_t color) {
 for (int v = 0; v < h; ++v) for (int u = 0; u < w; ++u) {
  const int X = x + u, Y = y + v; if (X < 0 || Y < 0 || X >= 320 || Y >= 172) continue;
  const int al = a[v * w + u]; if (al) px[Y * stride + X] = blend565(px[Y * stride + X], color, al);
 }
}
inline int glyphIndex(const Font &f, char ch) { for (int i = 0; f.chars[i]; ++i) if (f.chars[i] == ch) return i; return -1; }
// right: x is the right edge
inline void type(uint16_t *px, uint32_t stride, const Font &f, const char *text, int x, int y, uint16_t color, bool right) {
 int width10 = 0, last = -1;
 for (int n = 0; text[n]; ++n) { const int g = glyphIndex(f, text[n]); if (g < 0) continue; width10 += f.adv10[g]; last = g; }
 if (last >= 0) width10 -= f.adv10[last] - f.w[last] * 10;
 int cx10 = (right ? x * 10 - width10 : x * 10);
 for (int n = 0; text[n]; ++n) {
  const int g = glyphIndex(f, text[n]); if (g < 0) continue;
  if (f.w[g]) alphaBlit(px, stride, f.alpha + f.off[g], f.w[g], f.h, (cx10 + 5) / 10, y, color);
  cx10 += f.adv10[g];
 }
}
}
