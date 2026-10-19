// SPDX-License-Identifier: GPL-3.0-or-later
// AudioWorklet lifecycle follows the pinned Surge browser migration. The engine
// gate is never waited on by the audio thread. UI painting uses output snapshots.
#include "BrowserSynth.h"
#include "BrowserControl.h"
#include "MidiQueue.h"
#include "Transport.h"
#include <emscripten.h>
#include <emscripten/webaudio.h>
#include <array>
#include <atomic>
extern "C" void helm_retire_audio_thread(void*);
extern "C" thread_local int helm_audio_callback_depth;
extern "C" int helm_browser_audio_pool_reserve(size_t);
extern "C" int helm_enable_audio();
extern "C" void helm_browser_audio_changed(int);
extern "C" void helm_browser_audio_failed(int);
namespace {
BrowserSynth* synth{};
mopo::Value* tempo{};
EMSCRIPTEN_WEBAUDIO_T context{}, node{};
alignas(16) std::array<unsigned char, 1024*1024> stack;
Helm::Web::MidiQueue events;
Helm::Web::Transport transport;
std::atomic<double> ppqSnapshot{0};
std::atomic<bool> rewindRequested{false};
std::atomic<int> status{0};
std::atomic<unsigned> blocks{0}, skipped{0}, peakMillionths{0};
bool pending=false, closing=false, threadInitialized=false, preparePending=false, failed=false;
bool offline=false;
double deadline=0, rate=48000;
uint64_t offlineFrame=0;
float lastSample[2]{};
bool fadeIn=false;
void report(int value, const char* message) {
  status.store(value);
  EM_ASM({document.getElementById('status').textContent=UTF8ToString($0);}, message);
}
void deliver(const Helm::Web::MidiQueue::Event& event, int offset) {
  auto* engine=synth->getEngine();
  const int type=event.bytes[0]&0xf0, channel=event.bytes[0]&15;
  const int first=event.bytes[1], second=event.bytes[2];
  if (type==0x90 && second) engine->noteOn(first, second/127., offset, channel);
  else if (type==0x80 || type==0x90) engine->noteOff(first, offset);
  else if (type==0xa0) engine->setAftertouch(first, second/double(mopo::MIDI_SIZE), offset);
  else if (type==0xd0) engine->setChannelAftertouch(channel+1, first/127., offset);
  else if (type==0xe0) engine->setPitchWheel(2.*(first+128*second)/16383.-1., channel+1);
  else if (type==0xb0) {
    if (first==1) engine->setModWheel(second/127., channel+1);
    else if (first==64) { if (second>=64) engine->sustainOn(); else engine->sustainOff(); }
    else if (first==120 || first==123) { engine->sustainOff(); engine->allNotesOff(offset); }
  }
}
void process(float* left, float* right, int n, uint64_t firstFrame) {
  const auto position=transport.beginBlock();
  const auto bpm=tempo->value()*60.;
  if (position.playing || rewindRequested.exchange(false))
    synth->getEngine()->correctToTime(position.ppq*60./bpm*rate);
  // Worklet quanta currently contain 128 samples, but accept variable lengths.
  for (int offset=0;offset<n;offset+=mopo::MAX_BUFFER_SIZE) {
    const int count=std::min(mopo::MAX_BUFFER_SIZE,n-offset);
    if (events.drain(firstFrame+offset,count,deliver)) {
      synth->getEngine()->sustainOff(); synth->getEngine()->allNotesOff();
    }
    synth->render(left+offset,right+offset,count);
  }
  transport.advance(n,rate,bpm);
  ppqSnapshot.store(transport.position(),std::memory_order_relaxed);
}
bool render(int, const AudioSampleFrame*, int count, AudioSampleFrame* output,
            int, const AudioParamFrame*, void*) {
  if (count!=1 || output[0].numberOfChannels!=2) return true;
  auto& out=output[0];
  int expected=0;
  if (!helmEngineGate.compare_exchange_strong(expected,1,std::memory_order_acquire)) {
    for (int channel=0;channel<2;++channel) {
      for (int i=0;i<out.samplesPerChannel;++i)
        out.data[channel*out.samplesPerChannel+i]=lastSample[channel]*(1.f-float(i+1)/out.samplesPerChannel);
      lastSample[channel]=0;
    }
    fadeIn=true;
    skipped.fetch_add(1,std::memory_order_relaxed);
    return true;
  }
  ++helm_audio_callback_depth;
  process(out.data,out.data+out.samplesPerChannel,out.samplesPerChannel,
          static_cast<uint64_t>(EM_ASM_DOUBLE({return currentFrame;})));
  if (fadeIn) {
    for (int channel=0;channel<2;++channel)
      for (int i=0;i<out.samplesPerChannel;++i)
        out.data[channel*out.samplesPerChannel+i]*=float(i+1)/out.samplesPerChannel;
    fadeIn=false;
  }
  for (int channel=0;channel<2;++channel)lastSample[channel]=out.data[(channel+1)*out.samplesPerChannel-1];
  float peak=0;
  for (int i=0;i<2*out.samplesPerChannel;++i) peak=std::max(peak,std::abs(out.data[i]));
  peakMillionths.store(unsigned(std::min(1000.f,peak)*1000000),std::memory_order_relaxed);
  --helm_audio_callback_depth;
  helmEngineGate.store(0,std::memory_order_release);
  blocks.fetch_add(1,std::memory_order_relaxed);
  return true;
}
void ready(EMSCRIPTEN_WEBAUDIO_T handle, bool success, void*) {
  if (handle!=context || closing || !pending) return;
  pending=false;
  if (!success) { report(-1,"Audio processor creation failed. Enable audio to retry."); return; }
  int channels[]={2};
  EmscriptenAudioWorkletNodeCreateOptions options{};
  options.numberOfInputs=0; options.numberOfOutputs=1; options.outputChannelCounts=channels;
  node=emscripten_create_wasm_audio_worklet_node(context,"helm",&options,render,nullptr);
  if (!node) { report(-1,"Audio node creation failed. Enable audio to retry."); return; }
  EM_ASM({const h=$0;HelmAudioLifecycle.watchProcessor(emscriptenGetAudioObject(h),emscriptenGetAudioObject($1),()=>Module._helm_browser_audio_failed(h));},context,node);
  emscripten_audio_node_connect(node,context,0,0);
  helm_browser_audio_changed(context);
}
void started(EMSCRIPTEN_WEBAUDIO_T handle, bool success, void*) {
  if (handle!=context || closing || !pending) return;
  if (!success) { pending=false; report(-1,"AudioWorklet startup failed. Enable audio to retry."); return; }
  WebAudioWorkletProcessorCreateOptions options{}; options.name="helm";
  emscripten_create_wasm_audio_worklet_processor_async(context,&options,ready,nullptr);
}
void prepareAndStart() {
  if (!preparePending || !context || closing || !helmTryControl()) return;
  rate=EM_ASM_DOUBLE({return emscriptenGetAudioObject($0).sampleRate;},context);
  helm_browser_audio_pool_reserve(size_t(64)<<20);
  synth->prepare(rate);
  // Every voice/router is constructed before callbacks begin.
  std::array<float,128> left{},right{};
  synth->render(left.data(),right.data(),128);
  preparePending=false;
  helmReleaseControl();
  deadline=emscripten_get_now()+15000;
  pending=true; threadInitialized=true;
  report(1,"Starting audio…");
  emscripten_start_wasm_audio_worklet_thread_async(context,stack.data(),stack.size(),started,nullptr);
}
}
void helmAttachAudio(BrowserSynth* value) { synth=value;tempo=synth->getControls().at("beats_per_minute"); }
void helmPollAudio() {
  prepareAndStart();
  if (pending && status.load()==1 && emscripten_get_now()>deadline)
    report(-1,"Audio startup is still pending. Enable audio to continue waiting.");
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_enable_audio() {
  if (!synth || closing) return 0;
  if (context && status.load()<0) {
    if (pending) {
      deadline=emscripten_get_now()+15000;
      report(1,"Still starting audio…"); emscripten_resume_audio_context_sync(context); return 1;
    }
    closing=true;
    report(3,"Closing previous audio context…");
    EM_ASM({const h=$0;HelmAudioLifecycle.close(emscriptenGetAudioObject(h),$1?emscriptenGetAudioObject($1):null).then(()=>Module._helm_browser_audio_closed(h,1),()=>Module._helm_browser_audio_closed(h,0));},context,node);
    return 1;
  }
  if (context) { emscripten_resume_audio_context_sync(context); return 1; }
  offline=false; failed=false; events.panic();
  EmscriptenWebAudioCreateAttributes options{}; options.latencyHint="interactive";
  context=emscripten_create_audio_context(&options);
  if (!context) {report(-1,"Unable to create an audio context."); return 0;}
  EM_ASM({const h=$0;const c=emscriptenGetAudioObject(h);HelmAudioLifecycle.guard(c);c.onstatechange=()=>Module._helm_browser_audio_changed(h);},context);
  preparePending=true;
  report(1,"Preparing audio…");
  emscripten_resume_audio_context_sync(context);
  prepareAndStart();
  return 1;
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_audio_changed(int handle) {
  if (handle!=context || closing) return;
  auto state=emscripten_audio_context_state(context);
  if (state==AUDIO_CONTEXT_STATE_CLOSED) {
    // A late worklet import is not cancellation. Keep its stack reserved until
    // the SDK callback resolves; retries must not reuse a live stack.
    preparePending=false; events.panic();
    report(-1,"Audio closed. Enable audio to restart.");
  } else if (node && !failed) {
    if (state==AUDIO_CONTEXT_STATE_RUNNING) report(2,"Audio enabled");
    else {events.panic();report(4,"Audio suspended. Enable audio to resume.");}
  }
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_audio_failed(int handle) {
  if (handle!=context || closing) return;
  failed=true;events.panic();report(-1,"Audio processor stopped. Enable audio to restart.");
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_audio_closed(int handle,int success) {
  if (!closing || handle!=context) return;
  closing=false;
  if (!success) {report(-1,"Audio cleanup failed. Enable audio to retry.");return;}
  if (threadInitialized) {helm_retire_audio_thread(stack.data());threadInitialized=false;}
  EM_ASM({delete emAudio[$0];if($1)delete emAudio[$1];},context,node);
  context=node=0;pending=false;
  int expected=1;helmEngineGate.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
  report(0,"Restarting audio…");helm_enable_audio();
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_audio_status() {return status.load();}
extern "C" EMSCRIPTEN_KEEPALIVE unsigned helm_browser_audio_blocks() {return blocks.load();}
extern "C" EMSCRIPTEN_KEEPALIVE unsigned helm_browser_audio_skipped() {return skipped.load();}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_audio_time() {return context?EM_ASM_DOUBLE({return emscriptenGetAudioObject($0).currentTime;},context):0.;}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_audio_rate() {return rate;}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_panic() {events.panic();}
extern "C" EMSCRIPTEN_KEEPALIVE unsigned helm_browser_midi_overflows() {return events.overflowCount();}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_midi(int statusByte,int first,int second,double frame) {
  if (status.load()!=2 && !offline) return 0;
  if (!events.push(statusByte,first,second,frame)) return 0;
  // Patch selection and MIDI-learn touch files, mappings and GUI. They run only
  // on the control thread. Performance events stay sample-timestamped above.
  if ((statusByte&0xf0)==0xc0 || (statusByte&0xf0)==0xb0)
    helmQueueControl([=]{synth->mappedMidiControl(statusByte,first,second);});
  return 1;
}
void BrowserSynth::queueKeyboard(const MidiMessage& message) {
  const auto* data=message.getRawData();
  helm_browser_midi(data[0],data[1],message.getRawDataSize()>2?data[2]:0,
                   std::floor(helm_browser_audio_time()*rate));
}
// Exclusive offline path used for native/Wasm numerical parity.
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_offline_begin(double sampleRate) {
  if (context || !synth || !std::isfinite(sampleRate) || sampleRate<8000 || sampleRate>192000 || !helmTryControl()) return 0;
  rate=sampleRate;offlineFrame=0;events.panic();events.drain(0,128,[](auto&,int){});
  synth->getEngine()->allNotesOff();synth->prepare(rate);synth->controlTick();
  helm_browser_audio_pool_reserve(size_t(64)<<20);
  offline=true;helmReleaseControl();return 1;
}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_offline_render(int frames) {
  if (!offline || context || frames<1 || frames>192000 || !helmTryControl()) return -1;
  std::array<float,256> left{},right{};double energy=0;
  for(int offset=0;offset<frames;offset+=256) {
    const int count=std::min(256,frames-offset);
    ++helm_audio_callback_depth;process(left.data(),right.data(),count,offlineFrame);--helm_audio_callback_depth;
    for(int i=0;i<count;++i) energy+=double(left[i])*left[i]+double(right[i])*right[i];
    offlineFrame+=count;
  }
  helmReleaseControl();return energy;
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_offline_end() {
  if (context || !offline || !helmTryControl()) return;
  synth->getEngine()->allNotesOff();offline=false;events.panic();helmReleaseControl();
}

extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_audio_peak() {return peakMillionths.load()/1000000.;}

extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_transport(int playing,int numerator,int denominator) {
  return transport.configure(playing!=0,numerator,denominator);
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_rewind() {transport.rewind();rewindRequested=true;}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_transport_tempo(double bpm) {
  if(!std::isfinite(bpm)||bpm<20||bpm>300)return 0;
  helmQueueControl([bpm]{synth->valueChangedInternal("beats_per_minute",bpm/60.);synth->editor->updateGuiControl("beats_per_minute",bpm/60.);});
  return 1;
}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_transport_bpm() {return tempo?tempo->value()*60.:120.;}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_transport_playing() {return transport.requested().playing;}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_transport_position() {return ppqSnapshot.load();}
