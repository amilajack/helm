// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only libc PRNG replacement. Identical random sequences exercise noise,
// unison, random LFO and arpeggiator code without changing production randomness.
#include "helm_engine.h"
#include "helm_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
static uint32_t randomState=1;
extern "C" int rand() noexcept(noexcept(std::rand())) {
  randomState=randomState*1664525u+1013904223u;
  return int(randomState>>1);
}
extern "C" void srand(unsigned seed) noexcept(noexcept(std::srand(seed))) {randomState=seed;}
int main(int argc,char** argv) {
  if(argc<6){std::fprintf(stderr,"usage: renderer output case rate block frames\n");return 2;}
  const int which=std::atoi(argv[2]), rate=std::atoi(argv[3]), block=std::atoi(argv[4]), frames=std::atoi(argv[5]);
  if(block<1||block>mopo::MAX_BUFFER_SIZE||frames<1||rate<8000)return 2;
  std::srand(1234567);
  mopo::HelmEngine engine;
  engine.setSampleRate(rate);engine.setBufferSize(block);
  auto controls=engine.getControls();
  auto set=[&](const std::string& name,double value){
    auto found=controls.find(name);
    if(found==controls.end()){std::fprintf(stderr,"Unknown control: %s\n",name.c_str());std::exit(3);}
    found->second->set(value);
  };
  for(auto& entry:controls)set(entry.first,mopo::Parameters::getDetails(entry.first).default_value);
  set("amp_attack",0);set("amp_release",0.04);set("volume",0.4);
  set("osc_1_unison_voices",1);set("osc_2_unison_voices",1);
  set("osc_2_volume",0);set("noise_volume",0);set("filter_on",0);
  if(which>=0&&which<11)set("osc_1_waveform",which);
  else if(which>=20&&which<27){set("filter_on",1);set("filter_style",(which-20)/3);set("filter_blend",(which-20)%3);set("filter_shelf",(which-20)%3);set("cutoff",65);set("resonance",0.6);}
  else if(which>=30&&which<34){set("distortion_on",1);set("distortion_type",which-30);set("distortion_drive",12);set("distortion_mix",0.65);}
  else if(which==40){set("delay_on",1);set("delay_sync",0);set("delay_frequency",8);set("delay_feedback",0.4);set("delay_dry_wet",0.5);}
  else if(which==41){set("reverb_on",1);set("reverb_dry_wet",0.5);}
  else if(which==42){set("stutter_on",1);set("stutter_sync",0);set("stutter_resample_sync",0);}
  else if(which==43){set("noise_volume",0.5);}
  else if(which==44){set("osc_1_unison_voices",8);set("osc_2_unison_voices",4);set("osc_2_volume",0.5);set("cross_modulation",0.2);}
  else if(which==45){set("arp_on",1);set("arp_pattern",3);}
  else if(which==46){set("formant_on",1);}
  mopo::ModulationConnection modulation("mono_lfo_1","cutoff");
  if(which>=50&&which<63){
    set("filter_on",1);set("cutoff",65);set("mono_lfo_1_waveform",which-50);
    modulation.amount.set(24);engine.connectModulation(&modulation);
  }
  std::vector<float> output;output.reserve(size_t(frames)*2);
  engine.process(); // Adopt arp/routing changes before the first performance event.
  engine.noteOn(60,0.8);engine.noteOn(67,0.6);
  for(int offset=0;offset<frames;offset+=block){
    const int count=std::min(block,frames-offset);engine.setBufferSize(count);
    if(offset<=frames/2&&offset+count>frames/2){engine.sustainOn();engine.noteOff(60,frames/2-offset);}
    if(offset<=frames*3/4&&offset+count>frames*3/4){engine.sustainOff();engine.allNotesOff(frames*3/4-offset);}
    engine.process();
    for(int i=0;i<count;++i)for(int c=0;c<2;++c){
      const float sample=engine.output(c)->buffer[i];
      if(!std::isfinite(sample)){std::fprintf(stderr,"Non-finite audio\n");return 4;}
      output.push_back(sample);
    }
  }
  if(which>=50&&which<63)engine.disconnectModulation(&modulation);
  auto* file=std::fopen(argv[1],"wb");if(!file)return 5;
  const bool ok=std::fwrite(output.data(),sizeof(float),output.size(),file)==output.size();
  std::fclose(file);return ok?0:6;
}
