// SPDX-License-Identifier: GPL-3.0-or-later
// Headless WAM instrument: the same production engine as AU/VST3 and standalone.
#include "helm_engine.h"
#include "helm_common.h"
#include <emscripten.h>
#include <array>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>
extern "C" thread_local int helm_audio_callback_depth;
extern "C" int helm_browser_audio_pool_reserve(size_t);
namespace {
std::string quote(const std::string& value) {
  std::string result="\"";
  for (auto c:value) {if(c=='"'||c=='\\')result+='\\';if(c=='\n'){result+="\\n";continue;}result+=c;}
  return result+'"';
}
struct Instrument {
  mopo::HelmEngine engine;
  std::vector<mopo::Value*> values;
  std::vector<std::string> names;
  std::vector<std::unique_ptr<mopo::ModulationConnection>> routes;
  std::array<float,512> output{};
  std::string metadata;
  explicit Instrument(int rate) {
    engine.setSampleRate(rate);engine.setBufferSize(128);
    const auto controls=engine.getControls();
    std::ostringstream json;json<<std::setprecision(17)<<"{\"parameters\":[";
    bool first=true;
    for(const auto& entry:controls) {
      const auto& details=mopo::Parameters::getDetails(entry.first);
      values.push_back(entry.second);names.push_back(entry.first);
      entry.second->set(details.default_value);
      // Preserve the native reader's historical factory-preset ranges.
      double minimum=details.min,maximum=details.max;
      if(entry.first=="cutoff")minimum=0;
      if(entry.first=="stutter_frequency")minimum=-7;
      if(entry.first=="distortion_drive")maximum=72;
      if(!first)json<<',';first=false;
      json<<"{\"id\":"<<quote(entry.first)<<",\"label\":"<<quote(details.display_name)
          <<",\"minValue\":"<<minimum<<",\"maxValue\":"<<maximum
          <<",\"defaultValue\":"<<details.default_value<<",\"discreteStep\":0,\"nativeStep\":"<<(details.steps>1?1:0)
          <<",\"display\":{\"skew\":"<<int(details.display_skew)<<",\"offset\":"<<details.post_offset
          <<",\"invert\":"<<(details.display_invert?"true":"false")<<",\"multiplier\":"<<details.display_multiply
          <<",\"units\":"<<quote(details.display_units)<<"}}";
    }
    json<<"],\"modulationSources\":[";first=true;
    for(const auto& source:engine.getModulationSources()) {
      if(!first)json<<',';first=false;
      json<<"{\"id\":"<<quote(source.first)<<",\"destinations\":[";
      bool firstDestination=true;
      for(const auto& name:names)if(engine.getModulationDestination(name,source.second->owner->isPolyphonic())) {
        if(!firstDestination)json<<',';firstDestination=false;json<<quote(name);
      }
      json<<"]}";
    }
    json<<"]}";metadata=json.str();
    engine.process(); // Adopt the initial routing/arp controls before notes arrive.
  }
  ~Instrument() {for(const auto& route:routes)engine.disconnectModulation(route.get());}
};
}
extern "C" {
EMSCRIPTEN_KEEPALIVE Instrument* helm_wam_create(int rate) {
  if(rate<8000||rate>192000||!helm_browser_audio_pool_reserve(size_t(32)<<20))return nullptr;
  return new Instrument(rate);
}
EMSCRIPTEN_KEEPALIVE void helm_wam_destroy(Instrument* synth) {delete synth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_callback_begin() {++helm_audio_callback_depth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_callback_end() {--helm_audio_callback_depth;}
EMSCRIPTEN_KEEPALIVE void helm_wam_prepare_state(Instrument* synth) {
  synth->engine.sustainOff();synth->engine.allNotesOff();
  synth->engine.setBufferSize(128);synth->engine.process();
}
EMSCRIPTEN_KEEPALIVE const char* helm_wam_metadata(Instrument* synth) {return synth->metadata.c_str();}
EMSCRIPTEN_KEEPALIVE int helm_wam_set(Instrument* synth,int index,double value) {
  if(!synth||index<0||size_t(index)>=synth->values.size()||!std::isfinite(value))return 0;
  synth->values[size_t(index)]->set(value);return 1;
}
EMSCRIPTEN_KEEPALIVE double helm_wam_get(Instrument* synth,int index) {
  return synth&&index>=0&&size_t(index)<synth->values.size()?synth->values[size_t(index)]->value():0;
}
EMSCRIPTEN_KEEPALIVE void helm_wam_clear_routes(Instrument* synth) {
  for(const auto& route:synth->routes) synth->engine.disconnectModulation(route.get());
  synth->routes.clear();
}
EMSCRIPTEN_KEEPALIVE int helm_wam_route(Instrument* synth,const char* source,const char* destination,double amount) {
  if(!std::isfinite(amount)||std::abs(amount)>1e6||synth->routes.size()>=256)return 0;
  const auto sources=synth->engine.getModulationSources();
  const auto found=sources.find(source);
  if(found==sources.end()||!synth->engine.getModulationDestination(destination,found->second->owner->isPolyphonic()))return 0;
  auto connection=std::make_unique<mopo::ModulationConnection>(source,destination);
  connection->amount.set(amount);synth->engine.connectModulation(connection.get());
  synth->routes.push_back(std::move(connection));return 1;
}
EMSCRIPTEN_KEEPALIVE void helm_wam_midi(Instrument* synth,int status,int first,int second) {
  if(status<0x80||status>=0xf0||first<0||first>127||second<0||second>127)return;
  auto& engine=synth->engine;const int type=status&0xf0,channel=status&15;
  if(type==0x90&&second)engine.noteOn(first,second/127.,0,channel);
  else if(type==0x80||type==0x90)engine.noteOff(first);
  else if(type==0xa0)engine.setAftertouch(first,second/double(mopo::MIDI_SIZE));
  else if(type==0xd0)engine.setChannelAftertouch(channel+1,first/127.);
  else if(type==0xe0)engine.setPitchWheel(2.*(first+second*128)/16383.-1.,channel+1);
  else if(type==0xb0) {
    if(first==1)engine.setModWheel(second/127.,channel+1);
    else if(first==64){if(second>=64)engine.sustainOn();else engine.sustainOff();}
    else if(first==120||first==123){engine.sustainOff();engine.allNotesOff();}
  }
}
EMSCRIPTEN_KEEPALIVE void helm_wam_transport(Instrument* synth,double ppq,double bpm) {
  if(std::isfinite(ppq)&&std::isfinite(bpm)&&bpm>0)
    synth->engine.correctToTime(ppq*60./bpm*synth->engine.getSampleRate());
}
EMSCRIPTEN_KEEPALIVE float* helm_wam_render(Instrument* synth,int frames) {
  if(!synth||frames<1||frames>256)return nullptr;
  ++helm_audio_callback_depth;
  synth->engine.setBufferSize(frames);synth->engine.process();
  for(int i=0;i<frames;++i){synth->output[i]=float(synth->engine.output(0)->buffer[i]);synth->output[256+i]=float(synth->engine.output(1)->buffer[i]);}
  --helm_audio_callback_depth;
  return synth->output.data();
}
}
