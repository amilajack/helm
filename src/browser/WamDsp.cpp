// SPDX-License-Identifier: GPL-3.0-or-later
// Headless WAM instrument: the same production engine as AU/VST3 and standalone.
#include "HelmInstrument.h"
#include <emscripten.h>
using helm::Instrument;
extern "C" {
EMSCRIPTEN_KEEPALIVE Instrument* helm_wam_create(int rate) {
  if(rate<8000||rate>192000||!helm_browser_audio_pool_reserve(size_t(32)<<20))return nullptr;
  return new Instrument(rate);
}
EMSCRIPTEN_KEEPALIVE void helm_wam_destroy(Instrument* synth) {delete synth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_callback_begin() {++helm_audio_callback_depth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_callback_end() {--helm_audio_callback_depth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_prepare_state(Instrument* synth) {synth->prepareState();}
EMSCRIPTEN_KEEPALIVE const char* helm_wam_metadata(Instrument* synth) {return synth->metadataJson().c_str();}
EMSCRIPTEN_KEEPALIVE int helm_wam_set(Instrument* synth,int index,double value) {return synth&&synth->set(index,value);}
EMSCRIPTEN_KEEPALIVE double helm_wam_get(Instrument* synth,int index) {return synth?synth->get(index):0;}
EMSCRIPTEN_KEEPALIVE void helm_wam_clear_routes(Instrument* synth) {synth->clearRoutes();}
EMSCRIPTEN_KEEPALIVE int helm_wam_route(Instrument* synth,const char* source,const char* destination,double amount) {
  return synth->route(source,destination,amount);
}
EMSCRIPTEN_KEEPALIVE void helm_wam_midi(Instrument* synth,int status,int first,int second) {synth->midi(status,first,second);}
EMSCRIPTEN_KEEPALIVE void helm_wam_transport(Instrument* synth,double ppq,double bpm) {synth->transport(ppq,bpm);}
EMSCRIPTEN_KEEPALIVE float* helm_wam_render(Instrument* synth,int frames) {
  return synth?const_cast<float*>(synth->render(frames)):nullptr;
}
}
