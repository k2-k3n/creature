// Creature Voice — a Vox-style wah into the creature's growl, mono, MIT.
//
// WAH is the pedal: while it is held the wah rocks from heel to toe in about 0.3 s and stays at the toe; on
// release it rocks back in 0.4 s (tap it in rhythm for wah-wah strokes). The position is eased like a foot on a
// pedal. The sweep fades in from a fixed filter voice and returns to it on release. The wah
// comes first, as a pedal before an amp, then GROWL drives it (0 is clean). CHARACTER picks the wah voicing:
// MOSS (Vox-style), VELVET (smooth, dark), CINDER (the measured Cry Baby), REED (narrow, vocal); see wah.h.
// A CHARACTER change morphs the voicing over ~0.1 s (no coefficient jump while the wah is in). GROWL has a
// make-up gain measured on the DI, so loudness stays put across its range. At MIX 0 the guitar passes at unity
// (LEVEL scales only the wet; 82, the default, is unity). No transcendental runs per sample.
#include <resona/module.h>
#include <resona/parameters.h>
#include <resona/math.h>
#include "wah.h"
namespace {
enum { GROWL, CHARACTER, WAH, MIX, LEVEL, COUNT };
const resona_param params[] = {
 {"GROWL", 0, 100, 40, "%"}, {"CHARACTER", 0, 3, 0, ""},
 {"WAH", 0, 1, 0, ""}, {"MIX", 0, 100, 100, "%"}, {"LEVEL", 0, 100, 82, "%"},
};
const char *const characters[] = {"MOSS", "VELVET", "CINDER", "REED"};
const resona_param_ext ext[] = {
 {}, {RESONA_PARAM_CHOICE | RESONA_PARAM_STEPPED, 1, 4, characters},
 {RESONA_PARAM_TOGGLE, 1, 0, nullptr}, {}, {}
};
// Even-loudness gain at pedal 0, .25, .5, .75, 1 for each voicing: the reciprocal of the DI's RMS gain through
// the unit-gain filter. The raw resonator is ~30 dB louder at the heel than at the toe.
constexpr float kLevels[4][5] = {
 {0.0263f, 0.0594f, 0.1467f, 0.3357f, 0.6767f},
 {0.0281f, 0.0474f, 0.1049f, 0.2506f, 0.5440f},
 {0.0241f, 0.0555f, 0.1684f, 0.4500f, 1.0099f},
 {0.0186f, 0.0545f, 0.1821f, 0.3974f, 0.8587f}};
// GROWL make-up at 0, 20, .. 100 %: the reciprocal of the DI's RMS gain through the growl (wah at rest), so the
// wet sound sits at the dry guitar's loudness. Linear between the points.
constexpr float kMakeup[6] = {.8969f, .5619f, .3105f, .2110f, .1711f, .1545f};
// Resting tones, in CHARACTER order. The broad and narrow peaks sit in different bands.
struct RestVoice { wah::Voicing filter; float gain, mix, trim; };
constexpr RestVoice kRest[] = {
 {{580, 580, 2.0f, 2.0f}, .22f, .56f, .62f}, // MOSS: rounded low mids
 {{310, 310, 1.3f, 1.3f}, .17f, .67f, .377f}, // VELVET: dark and broad
 {{1780, 1780, 2.2f, 2.2f}, .78f, .56f, 1.26f}, // CINDER: bright bite
 {{1010, 1010, 5.8f, 5.8f}, .23f, .67f, .86f}, // REED: narrow vowel
};
constexpr float kRise = 1.f / (.32f * 48000), kFall = 1.f / (.40f * 48000);
float absf(float x) { return x < 0 ? -x : x; }
float clip(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
float flush(float x) { return absf(x) < 1e-20f ? 0 : x; }
float soft(float x) { return x / (1 + absf(x)); }
// output limiter: linear to 0.8, then a soft knee toward 0.99 (no corner)
float limit(float x) { const float a = absf(x); if (a <= .8f) return x; const float t = (a - .8f) * 5, y = .8f + .19f * t / (1 + t); return x < 0 ? -y : y; }
struct State {
 resona_parameter_inbox inbox; float desired[COUNT], p[COUNT], smooth[COUNT];
 wah::Filter wah, rest;
 wah::Filter restCoefficients[4];
 float stroke, pedal, wet, lp, dcX, dcY, makeup, restMix, restTrim;
 uint32_t coefficientPhase;
};
float level(uint32_t c, float pedal) {   // log-linear between the measured points
 const float x = clip(pedal, 0, 1) * 4; const uint32_t i = x >= 4 ? 3 : static_cast<uint32_t>(x); const float f = x - static_cast<float>(i);
 return kLevels[c][i] * resona_exp2f(f * resona_log2f(kLevels[c][i + 1] / kLevels[c][i]));
}
float lerp(float a, float b, float t) { return a + t * (b - a); }

void coefficients(State &s) {
 // the voicing follows the smoothed CHARACTER: between two voicings, interpolate the sweep and the level
 const float cs = clip(s.smooth[CHARACTER], 0, 3); const uint32_t c = cs >= 3 ? 2 : static_cast<uint32_t>(cs);
 const float f = cs - static_cast<float>(c);
 s.pedal = s.stroke * s.stroke * (3 - 2 * s.stroke);        // a foot eases in and out of the travel
 if (f < .001f || f > .999f) { const uint32_t k = f > .5f ? c + 1 : c; wah::set(s.wah, wah::kVoicings[k], s.pedal, level(k, s.pedal)); }
 else {
  const wah::Voicing &a = wah::kVoicings[c], &b = wah::kVoicings[c + 1];
  const wah::Voicing v = {a.heelHz + f * (b.heelHz - a.heelHz), a.toeHz + f * (b.toeHz - a.toeHz), a.heelQ + f * (b.heelQ - a.heelQ), a.toeQ + f * (b.toeQ - a.toeQ)};
  const float la = level(c, s.pedal); wah::set(s.wah, v, s.pedal, la * resona_exp2f(f * resona_log2f(level(c + 1, s.pedal) / la)));
 }
 const wah::Filter &restA = s.restCoefficients[c], &restB = s.restCoefficients[c + 1];
 s.rest.f2 = lerp(restA.f2, restB.f2, f);
 s.rest.fq = lerp(restA.fq, restB.fq, f);
 s.rest.g = lerp(restA.g, restB.g, f);
 s.restMix = lerp(kRest[c].mix, kRest[c + 1].mix, f);
 s.restTrim = lerp(kRest[c].trim, kRest[c + 1].trim, f);
 const float g = clip(s.smooth[GROWL], 0, 100) * .05f; const uint32_t i = g >= 5 ? 4 : static_cast<uint32_t>(g);
 s.makeup = kMakeup[i] + (g - static_cast<float>(i)) * (kMakeup[i + 1] - kMakeup[i]);
}
void reset(void *p) {
 State &s = *static_cast<State *>(p); s.lp = s.dcX = s.dcY = 0; s.stroke = s.pedal = s.wet = 0; wah::reset(s.wah);
 wah::reset(s.rest);
 for (uint32_t i = 0; i < 4; ++i)
  wah::set(s.restCoefficients[i], kRest[i].filter, 0, kRest[i].gain);
 s.coefficientPhase = 0;
 for (uint32_t n = 0; n < COUNT; ++n) s.smooth[n] = s.p[n];
 coefficients(s);
}
void init(void *p, const resona_host *) {
 State &s = *static_cast<State *>(p); s.inbox = {};
 for (uint32_t n = 0; n < COUNT; ++n) s.p[n] = s.desired[n] = params[n].def;
 reset(p);
}
void set(void *p, uint32_t n, float value) {
 if (n >= COUNT) return; State &s = *static_cast<State *>(p);
 value = clip(value, params[n].min, params[n].max);
 if (n == CHARACTER || n == WAH) value = static_cast<float>(static_cast<uint32_t>(value + .5f));
 s.desired[n] = value; resona_parameter_publish(&s.inbox, n, value);
}
float get(const void *p, uint32_t n) { return n < COUNT ? static_cast<const State *>(p)->desired[n] : 0; }
void process(void *p, const float *const *in, float *const *out, uint32_t frames) {
 State &s = *static_cast<State *>(p); float pending[12];
 const uint32_t bits = resona_parameter_consume(&s.inbox, pending);
 for (uint32_t n = 0; n < COUNT; ++n) if (bits & (1u << n)) s.p[n] = pending[n];
 for (uint32_t i = 0; i < frames; ++i) {
  for (uint32_t n = 0; n < COUNT; ++n) s.smooth[n] += (n == CHARACTER ? .0006f : .002f) * (s.p[n] - s.smooth[n]);
  if (s.coefficientPhase == 0) coefficients(s);
  s.coefficientPhase = (s.coefficientPhase + 1) & 15u;
  const float dry = in[0][i];
  // the stroke: linear in time, eased into the pedal position once per 16 samples
  s.stroke = clip(s.stroke + (s.p[WAH] > .5f ? kRise : -kFall), 0, 1);
  // Blend back to the resting voice near the heel, before the pedal stops.
  const float transition = clip(s.stroke * 10.f, 0.f, 1.f);
  const float sweep = transition * transition * (3.f - 2.f * transition);
  s.wet += (sweep - s.wet) * .0011f;
  const float resting = lerp(dry, wah::run(s.rest, dry), s.restMix) * s.restTrim;
  const float x = lerp(resting, wah::run(s.wah, dry), s.wet);
  // GROWL: soft clipping (0 is almost clean), a gentle top cut like a speaker
  const float growl = s.smooth[GROWL] * .01f, drive = 1 + growl * growl * 14;
  const float driven = soft(x * drive + .06f * x * x * drive) / (1 + .05f * drive) * (1 + growl * .6f);
  s.lp = flush(s.lp + .34f * (driven - s.lp));
  const float wet = s.lp * 1.35f * s.makeup * s.smooth[LEVEL] * (1.f / 82);
  const float mix = s.smooth[MIX] * .01f;
  // Keep the dry reference out of the wet path's DC blocker and limiter.
  const float dc = wet - s.dcX + .9985f * s.dcY;
  s.dcX = wet;
  s.dcY = flush(dc);
  out[0][i] = dry + (limit(dc) - dry) * mix;
 }
}
void trial(void *p, uint32_t phase) {
 set(p, GROWL, 100); set(p, CHARACTER, phase % 4); set(p, WAH, phase == RESONA_CPU_TRIAL_RELEASE ? 0 : 1);
}
const resona_cpu_profile cpu = {RESONA_CPU_PROFILE_VERSION, sizeof(resona_cpu_profile), 0, trial};
const resona_module module = {RESONA_MODULE_ABI, sizeof(resona_module),
 "org.resona.demo.creature.voice", "Creature Voice", "0.2.4",
 RESONA_LAYOUT_MONO, RESONA_RATE_48000, sizeof(State), COUNT, params, ext,
 init, reset, set, process, nullptr, nullptr, get, &cpu, nullptr, RESONA_MEMORY_FAST, nullptr};
}
extern "C" RESONA_EXPORT const resona_module *resona_module_get() { return &module; }
