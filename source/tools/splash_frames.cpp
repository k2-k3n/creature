// MIT. Renders the boot-splash source frames with the app's own rig and HUD type (host build, see make_splash.py).
// Writes raw RGB565 320x172 frames: rest ("oo"), blink, wah open ("aa", meter full) without the HUD, then the HUD
// alone on black for the rest and the open caption (make_splash.py draws it back crisp after posterising).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "rig.h"
static uint16_t fb[320 * 172];
static void hud(const char *caption) {
 const uint16_t ink = RESONA_RGB(255, 248, 219), green = RESONA_RGB(166, 189, 83);
 rig::alphaBlit(fb, 320, rig_art::wordmark, 136, 25, 4, 1, ink);
 rig::type(fb, 320, rig_art::label, "MOSS", 311, 9, green, true);
 rig::type(fb, 320, rig_art::num, "64", 8, 114, ink, false);
 rig::type(fb, 320, rig_art::label, "GROWL", 8, 148, ink, false);
 rig::type(fb, 320, rig_art::small, caption, 311, 149, ink, true);
}
int main(int argc, char **argv) {
 static rig::Rig r; rig::Shared *sh = (rig::Shared *)calloc(1, sizeof(rig::Shared)); rig::init(r, *sh, .64f);
 FILE *f = fopen(argc > 1 ? argv[1] : "frames.raw", "wb");
 auto settle = [&](float pedal, int ms) { for (int t = 0; t < ms; t += 33) rig::update(r, *sh, 33, pedal, 0, 64); };
 auto emit = [&](float lids) {
  if (lids >= 0) { r.lids[0] = r.lids[1] = lids; }
  rig::render(r, *sh, fb, 320, rig::litSegments(r, 64), nullptr); fwrite(fb, 2, 320 * 172, f);
 };
 auto emitHud = [&](const char *cap) { memset(fb, 0, sizeof fb); hud(cap); fwrite(fb, 2, 320 * 172, f); };
 r.nextBlink = 100000; settle(0, 1500); r.look[0] = r.look[1] = 0; r.lookT[0] = r.lookT[1] = 0; r.lids[0] = r.lids[1] = 0;
 emit(0);                              // 0 rest
 emit(1);                              // 1 blink
 settle(1, 900); emit(-1);             // 2 wah open
 emitHud("HOLD: WAH");                 // 3 HUD at rest
 emitHud("WAAAH");                     // 4 HUD with the wah open
 fclose(f);
}
