// Creature — a native Box App using only the public platform SDK. MIT.
//
// Holding the big knob is a wah pedal: the Creature Voice rocks a Vox-style wah from heel to toe and the creature's
// mouth opens with it ("oo" to "aa"); release rocks it back, so taps in rhythm play wah-wah strokes. Turning the
// big knob sets the growl (drive after the wah), the top knob the wah voicing.
//
// The Web, Mobile and MIDI controllers (controller/, CCs on channel 1, see README.md) reach every setting the knobs
// reach, plus the voice's MIX and LEVEL, and three hold pads: WAH (the pedal, like holding the big knob), ROAR
// (the pedal with full growl) and DRY (the guitar alone).
#include <resona/platform.h>
#include "demo_capture.h"
#include <resona/startup_helpers.h>
#include "splash_data.h"
#include <resona/ui.h>
#include "rig.h"
#include "own_audio.h"
#include "trip_guard.h"
#include "controller_link.h"
namespace {
const resona_platform *P;
const char *const names[]={"MOSS","VELVET","CINDER","REED"};
const char *const packId="org.resona.demo.creature.voice"; // dsp/resona.project.json
own_audio::Graph g_audio;
enum { GROWL, CHARACTER, WAH, MIX, LEVEL };
struct Saved {uint32_t magic,version;int32_t growl,character,compare,mix,level;};   // version 1 had no compare, 2 no mix/level
// The controllers' MIDI map: CC on channel 1 (controller/shared/profile.js says the same).
enum { CC_GROWL=20,CC_VOICING,CC_DRY,CC_MIX,CC_LEVEL,CC_WAH=30,CC_ROAR,CC_HUSH };
struct State {
 int growl,character,compare,mix,level,wah,keyDown[2],ready,error;
 int padWah,padRoar,padDry;   // pads held on a controller
 ctl::Link link;
 uint32_t now,nextSetup,saveAt,wahUntil,drawAt,drawEvery;float activity,stroke;rig::Rig rig;rig::Shared *sh;
 uint32_t stDraws,stDrawSum,stDrawMax,stSlow,stFrames,stFrameMax,lastFrameAt,stResetAt; // bench stats (SysEx command 3)
 uint32_t hudKey;   // what the corner labels showed at the last full present
 trip_guard::Guard guard;   // back-off after the OS bypasses the voice for CPU
};
int clamp(int x,int a,int b){return x<a?a:(x>b?b:x);}
ctl::Bind kBind[8];
void bind(State *s){
 const ctl::Bind b[8]={{CC_GROWL,ctl::RANGE,0,100,&s->growl},{CC_VOICING,ctl::STEPS,0,3,&s->character},{CC_DRY,ctl::STEPS,0,1,&s->compare},
  {CC_MIX,ctl::RANGE,0,100,&s->mix},{CC_LEVEL,ctl::RANGE,0,100,&s->level},
  {CC_WAH,ctl::TRIGGER,0,1,nullptr},{CC_ROAR,ctl::TRIGGER,0,1,nullptr},{CC_HUSH,ctl::TRIGGER,0,1,nullptr}};
 for(int i=0;i<8;++i)kBind[i]=b[i];
}
// The pads override the settings while held: ROAR rocks the pedal with full growl, DRY plays the guitar alone.
int growlNow(const State *s){return s->padRoar?100:s->growl;}
int wahNow(const State *s){return s->wah||s->padWah||s->padRoar;}
int dryNow(const State *s){return s->compare||s->padDry;}
void checked(State *s,int result){if(result<0)s->error=result;}   // an audio failure, shown until audio is fine again
void parameters(State *s){
 if(!s->ready)return;
 checked(s,P->effect_param_set(g_audio.h[0],GROWL,static_cast<float>(growlNow(s))));
 checked(s,P->effect_param_set(g_audio.h[0],CHARACTER,static_cast<float>(s->character)));
 checked(s,P->effect_param_set(g_audio.h[0],WAH,wahNow(s)?1.0f:0.0f));
 checked(s,P->effect_param_set(g_audio.h[0],MIX,dryNow(s)?0.0f:static_cast<float>(s->mix)));
 checked(s,P->effect_param_set(g_audio.h[0],LEVEL,static_cast<float>(s->level)));
}
void save(State *s){
 Saved saved={0x43525431u,3,s->growl,s->character,s->compare,s->mix,s->level};
 if(P->app_store_write(0,&saved,sizeof saved)<0)P->log("Creature: settings not saved");   // not an audio error
 s->saveAt=0;
}
// The OS bypassed the voice for CPU (API 1 reports it as a processor fault): show CPU LIMIT, drop the graph and
// rebuild it after a wait that grows with each trip (trip_guard.h).
void trip(State *s){s->ready=0;s->error=RESONA_ERR_BUDGET;s->nextSetup=s->now+trip_guard::tripped(s->guard,s->now);own_audio::stop(P,g_audio);}
// The App owns audio: it builds Guitar -> Creature Voice -> Output itself (own_audio.h).
// Until the voice is installed, retry once per second.
void setup(State *s){
 if(s->now<s->nextSetup)return;
 s->nextSetup=s->now+1000;
 if(s->ready){
  if(trip_guard::faulted(P,g_audio.h[0])){trip(s);return;}   // polled once a second while audio runs
  if(s->error){s->error=0;parameters(s);}return;             // a stale failure: resend, keep only a fresh one
 }
 int rc=own_audio::start(P,g_audio,packId,"0.2.4");
 if(rc<0){s->error=rc;return;}
 resona_effect_info fi={};
 rc=P->effect_info(g_audio.h[0],&fi);
 if(rc!=RESONA_OK||fi.param_count!=5){s->error=rc<0?rc:-20;return;}
 rc=P->effect_enable(g_audio.h[0],1);
 if(rc<0){s->error=rc;return;}
 s->ready=1;s->error=0;parameters(s);
}
void input(State *s,uint32_t kind,uint32_t which,int value){
 s->drawAt=s->now;
 if(which==RESONA_INPUT_SELECTOR){
  if(kind==RESONA_EVENT_TURN){s->character=(s->character+value%4+4)%4;s->saveAt=s->now+1800;}
  else if(kind==RESONA_EVENT_CLICK){s->compare=!s->compare;s->saveAt=s->now+1800;}
  else if(kind==RESONA_EVENT_LONG)save(s);
 }else{
  if(kind==RESONA_EVENT_TURN){s->growl=clamp(s->growl+value*2,0,100);s->saveAt=s->now+1800;}
  else if(kind==RESONA_EVENT_LONG){s->wah=1;if(!s->keyDown[1])s->wahUntil=s->now+1000;}   // synthetic long (MIDI): a 1 s stroke
 }
 parameters(s);
}
void key(State *s,int which,int down){
 s->drawAt=s->now;
 s->keyDown[which]=down;
 if(which==1){s->wah=down;s->wahUntil=0;if(s->ready)checked(s,P->effect_param_set(g_audio.h[0],WAH,wahNow(s)?1.0f:0.0f));}
}
const uint16_t kGround=RESONA_RGB(37,10,33);
void draw(State *s){
 uint16_t *px=nullptr;uint32_t stride=0;resona_ui *ui=P->screen_begin(&px,&stride);if(!ui||!px)return;
 const uint16_t ink=RESONA_RGB(255,248,219),green=RESONA_RGB(166,189,83);
 if(s->sh)rig::render(s->rig,*s->sh,px,stride,rig::litSegments(s->rig,growlNow(s)),P->millis);
 else ui->clear(ui->ctx,kGround);
 // the corners, all in the wordmark's style
 rig::alphaBlit(px,stride,rig_art::wordmark,136,25,4,1,ink);
 rig::type(px,stride,rig_art::label,dryNow(s)?"DRY":names[s->character],311,9,green,true);
 char number[4];P->format(number,sizeof number,"%d",clamp(growlNow(s),0,100));
 rig::type(px,stride,rig_art::num,number,8,114,ink,false);
 rig::type(px,stride,rig_art::label,dryNow(s)?"DRY":"GROWL",8,148,ink,false);
 rig::type(px,stride,rig_art::small,rig::caption(s->rig,s->activity),311,149,ink,true);
 // The host model has no DSP library; visual previews show the intended screen.
#if !defined(RESONA_LAB_HOST)
 if(s->error){
  ui->rect(ui->ctx,0,164,320,8,kGround);char line[48];
  if(s->error==RESONA_ERR_PACK_ID)P->format(line,sizeof line,"INSTALL CREATURE VOICE IN LAB");
  else if(s->error==RESONA_ERR_BUDGET)P->format(line,sizeof line,"CPU LIMIT");else P->format(line,sizeof line,"VOICE STATUS %d",s->error);
  ui->text(ui->ctx,6,165,1,RESONA_RGB(255,195,128),line);
 }
#endif
 if(!s->sh)ui->text(ui->ctx,6,90,1,RESONA_RGB(255,195,128),"NOT ENOUGH APP MEMORY");
 demo_capture::remember(px,stride);
 // Only the creature's region changes from frame to frame; present the whole screen when a corner
 // label changed (or there is an error line), so the platform refreshes as little as possible.
 const char *cap=rig::caption(s->rig,s->activity);
 const uint32_t key=static_cast<uint32_t>(growlNow(s))|(static_cast<uint32_t>(s->character)<<8)|(static_cast<uint32_t>(dryNow(s))<<12)|(static_cast<uint32_t>(s->error?1+(-s->error)%7:0)<<13)|(static_cast<uint32_t>(cap[0]+cap[1]*3+cap[4])<<16);
 if(key!=s->hudKey||!s->sh){s->hudKey=key;P->screen_present(0,0,0,0);}
 else P->screen_present(rig::kSX0,rig::kSY0,rig::kSX1-rig::kSX0,rig::kSY1-rig::kSY0);
}
int init(void *state,const resona_platform *platform){
 State *s=static_cast<State*>(state);P=platform;s->growl=64;s->mix=100;s->level=82;
 Saved saved={};int rc=P->app_store_read(0,&saved,sizeof saved);
 if(rc==RESONA_OK&&saved.magic==0x43525431u&&saved.version>=1&&saved.version<=3&&saved.growl>=0&&saved.growl<=100&&saved.character>=0&&saved.character<4){
  s->growl=saved.growl;s->character=saved.character;if(saved.version>=2&&(saved.compare==0||saved.compare==1))s->compare=saved.compare;
  if(saved.version==3){s->mix=clamp(saved.mix,0,100);s->level=clamp(saved.level,0,100);}}
 bind(s);
 P->input_config(RESONA_INPUT_SELECTOR,0);P->input_config(RESONA_INPUT_CUSTOMIZE,0);
 // the boot splash (tools/make_splash.py): written only when it differs; it plays from the next power-up
 { const int rc = resona_startup_splash_apply(P, kSplash, sizeof kSplash);
   if (rc == RESONA_STARTUP_REBOOT_REQUIRED) P->log("Creature: boot splash updated; it plays from the next power-up");
   else if (rc < 0 && rc != RESONA_ERR_UNSUPPORTED) P->log("Creature: boot splash not applied"); }
 // the background plate and the warp basis live on the app heap
 s->sh=static_cast<rig::Shared*>(P->alloc(sizeof(rig::Shared),4));
 if(s->sh)rig::init(s->rig,*s->sh,s->growl/100.f);else P->log("Creature: app heap allocation failed");
 P->log("Creature: native UI, owns audio: Guitar -> Creature Voice");return 0;
}
#ifdef RESONA_DEMO_BENCH
// Bench stats on the capture channel: F0 7D 52 42 58 53 03 token F7 reads, 04 resets.
// Reply: F0 7D 52 42 58 53 40 cmd token, then 11 values as 4 x 7-bit groups each, F7.
bool stats(State *s,const resona_app_event *e){
 if(e->kind!=RESONA_APP_EV_MIDI||e->u.midi.len<9)return false;
 const uint8_t *m=e->u.midi.data;
 if(m[0]!=0xf0||m[1]!=0x7d||m[2]!=0x52||m[3]!=0x42||m[4]!=0x58||m[5]!=0x53||(m[6]!=3&&m[6]!=4))return false;
 if(m[6]==4){s->stDraws=s->stDrawSum=s->stDrawMax=s->stSlow=s->stFrames=s->stFrameMax=0;s->stResetAt=P->millis();if(s->sh)s->sh->msBg=s->sh->msLoop=0;}
 const uint32_t v[11]={s->stDraws,s->stDrawSum,s->stDrawMax,s->stSlow,s->stFrames,s->stFrameMax,s->drawEvery,s->now,s->sh?s->sh->msBg:0,s->sh?s->sh->msLoop:0,P->millis()-s->stResetAt};
 uint8_t out[60]={0xf0,0x7d,0x52,0x42,0x58,0x53,0x40,m[6],m[7]};int n=9;
 for(int i=0;i<11;++i)for(int k=0;k<4;++k)out[n++]=static_cast<uint8_t>((v[i]>>(7*k))&127);
 out[n++]=0xf7;(void)P->midi_send(RESONA_MIDI_USB,out,static_cast<uint32_t>(n));return true;
}
#endif
void event(void *state,const resona_app_event *e){
 State *s=static_cast<State*>(state);
#ifdef RESONA_DEMO_BENCH
 if(stats(s,e))return;
#endif
 if(demo_capture::event(P,e))return;
 ctl::Msg m;
 if(ctl::receive(P,s->link,kBind,8,e,m)){
  if(!m.changed)return;
  if(m.cc==CC_WAH)s->padWah=m.down;else if(m.cc==CC_ROAR)s->padRoar=m.down;else if(m.cc==CC_HUSH)s->padDry=m.down;
  else s->saveAt=s->now+1800;
  s->drawAt=s->now;parameters(s);return;
 }
 if(e->kind==RESONA_APP_EV_INPUT)input(s,e->u.input.kind,e->u.input.input,e->u.input.value);
 else if(e->kind==RESONA_APP_EV_KEY&&e->u.key.key<2)key(s,static_cast<int>(e->u.key.key),e->u.key.down!=0);
#ifdef RESONA_DEMO_BENCH
 // bench only: channel-16 CC20..27 play the encoders and keys
 else if(e->kind==RESONA_APP_EV_MIDI&&e->u.midi.len>=3){
  const uint8_t *m=e->u.midi.data;if(m[0]!=0xbf)return;
  if(m[1]==20||m[1]==21)input(s,RESONA_EVENT_TURN,m[1]-20,static_cast<int>(m[2])-64);
  else if(m[1]==22||m[1]==23)key(s,m[1]-22,m[2]>=64);
  else if(m[1]>=24&&m[1]<=27&&m[2])input(s,m[1]>=26?RESONA_EVENT_LONG:RESONA_EVENT_CLICK,(m[1]-24)%2,1);
 }
#endif
 // audio restarted or the voice was bypassed for CPU (API 1: tripped_role is -1, the fault shows in effect_info)
 else if(e->kind==RESONA_APP_EV_AUDIO){if(e->u.audio.tripped_role>=0||(s->ready&&trip_guard::faulted(P,g_audio.h[0])))trip(s);}
 else if(e->kind==RESONA_APP_EV_CHANGED&&e->u.changed.what==RESONA_CHANGED_MODELS&&!s->ready&&s->error!=RESONA_ERR_BUDGET){s->nextSetup=s->now;}
}
void frame(void *state,uint32_t ms){ if(demo_capture::paused(P))return;
 State *s=static_cast<State*>(state);s->now+=ms;setup(s);ctl::sync(P,s->link,kBind,8,s->now);
 {const uint32_t t=P->millis();if(s->lastFrameAt){const uint32_t gap=t-s->lastFrameAt;if(gap>s->stFrameMax)s->stFrameMax=gap;}s->lastFrameAt=t;++s->stFrames;}
 uint8_t in=0,out=0;P->meters(&in,&out,nullptr);
 const float target=static_cast<float>(in)/9;
 s->activity+=(target-s->activity)*(static_cast<float>(ms)/(static_cast<float>(ms)+(target>s->activity?35.f:160.f)));
 if(s->wahUntil&&s->now>=s->wahUntil){s->wah=0;s->wahUntil=0;parameters(s);}
 // mirror the voice's stroke (0.32 s up, 0.40 s down, eased) so the mouth is the pedal you hear
 s->stroke+=(wahNow(s)?1.f/320.f:-1.f/400.f)*static_cast<float>(ms);s->stroke=s->stroke<0?0:(s->stroke>1?1:s->stroke);
 const float pedal=s->stroke*s->stroke*(3-2*s->stroke);
 if(s->saveAt&&s->now>=s->saveAt)save(s);
 if(s->sh)rig::update(s->rig,*s->sh,ms,pedal,s->activity,growlNow(s));
 if(s->now>=s->drawAt){
  // 30 frames a second; if a draw ever takes more than half of that, back off so the app thread
  // stays at least half idle and the callback always returns well inside its deadline
  const uint32_t t0=P->millis();draw(s);const uint32_t took=P->millis()-t0;
  ++s->stDraws;s->stDrawSum+=took;if(took>s->stDrawMax)s->stDrawMax=took;if(took>=20)++s->stSlow;
  s->drawEvery=took*2>33?(took*2>100?100:took*2):33;s->drawAt=s->now+s->drawEvery;
 }
}
const resona_app app={RESONA_PLATFORM_VERSION,sizeof(resona_app),RESONA_APP_ID,RESONA_APP_NAME,RESONA_APP_VERSION,sizeof(State),sizeof(rig::Shared)+4096,init,event,frame};
}
extern "C" const resona_app *resona_app_get(){return &app;}
