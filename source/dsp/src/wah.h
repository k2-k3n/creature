// Wah — a pedal-position resonator for one mono guitar, MIT.
//
// The structure is the measured Cry Baby model by Julius O. Smith (CCRMA, Faust `crybaby`): a two-pole resonator
// with a zero at DC, H(z) = g (1 - z^-1) / (1 + a1 z^-1 + a2 z^-2), whose resonance frequency, Q and gain all
// move with the pedal. Smith's fit is fr = 450 * 2^(2.3 w) Hz and Q = 2^(3 - 2 w) for pedal w in 0..1 (heel to
// toe). The other voicings shift that range and Q: a Vox-style sweep sits a little lower with a softer peak.
// Gain is set per voicing and position so the sweep stays at an even loudness (the raw model is much louder at
// the heel). Coefficients change once per 16 samples; the per-sample cost is 6 multiply-adds.
#pragma once
#include <stdint.h>
#include <resona/math.h>

namespace wah {
struct Voicing { float heelHz, toeHz, heelQ, toeQ; };
// MOSS: Vox V847-style (lower top, softer peak); VELVET: smooth and dark; CINDER: Smith's measured Cry Baby;
// REED: narrow and vocal.
constexpr Voicing kVoicings[4] = {{430, 1800, 6, 2.2f}, {350, 1500, 4, 1.8f}, {450, 2217, 8, 2}, {500, 2400, 11, 4}};

struct Filter { float w, v, f2, fq, g; };

inline void reset(Filter &f) { f.w = f.v = 0; f.f2 = f.fq = f.g = 0; }

// pedal 0 (heel) .. 1 (toe). `level` is the even-loudness gain, measured per voicing (see the bench notes).
inline void set(Filter &f, const Voicing &vc, float pedal, float level) {
 const float p = pedal < 0 ? 0 : (pedal > 1 ? 1 : pedal);
 const float fr = vc.heelHz * resona_exp2f(p * resona_log2f(vc.toeHz / vc.heelHz));
 const float Q = vc.heelQ * resona_exp2f(p * resona_log2f(vc.toeQ / vc.heelQ));
 const float frn = fr / 48000.f, d = 3.141592654f * frn / Q, sn = resona_sinf(3.141592654f * frn);
 f.f2 = d * d + 4 * (1 - d) * sn * sn;
 f.fq = d * (2 - d);
 f.g = level;
}

inline float run(Filter &f, float x) {
 const float vn = f.v + (f.g * x - f.f2 * f.w - f.fq * f.v);
 f.w += vn; f.v = vn;
 if (f.w < 1e-20f && f.w > -1e-20f) f.w = 0;
 if (f.v < 1e-20f && f.v > -1e-20f) f.v = 0;
 return vn;
}
}  // namespace wah
