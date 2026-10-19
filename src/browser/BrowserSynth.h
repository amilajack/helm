// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_base.h"
#include "synth_gui_interface.h"
class BrowserSynth : public SynthBase {
public:
  SynthGuiInterface* editor = nullptr;
  const CriticalSection& getCriticalSection() override { return critical; }
  SynthGuiInterface* getGuiInterface() override { return editor; }
  void controlTick() {
    processControlChanges();
    processModulationChanges(); // routing allocation belongs to the control thread
    MidiBuffer keyboard;
    keyboard_state_->processNextMidiBuffer(keyboard, 0, 128, true);
    for (auto event : keyboard) queueKeyboard(event.getMessage());
  }
  void prepare(double rate) { engine_.setSampleRate(rate); engine_.setBufferSize(128); }
  void render(float* left, float* right, int frames) {
    float* channels[] = {left, right};
    AudioBuffer<float> buffer(channels, 2, frames);
    for (int offset=0; offset<frames; offset+=mopo::MAX_BUFFER_SIZE)
      processAudio(&buffer, 2, std::min(mopo::MAX_BUFFER_SIZE, frames-offset), offset);
  }
  void mappedMidiControl(int status, int first, int second) {
    if ((status & 0xf0) == 0xc0 || first == 0 || first == 32)
      midi_manager_->processMidiMessage(MidiMessage(status, first, second), 0);
    else midi_manager_->midiInput(first, second);
  }
  void queueKeyboard(const MidiMessage& message);
private:
  CriticalSection critical;
};
extern BrowserSynth* helmSynth;
void helmAttachAudio(BrowserSynth*);
void helmPollAudio();
