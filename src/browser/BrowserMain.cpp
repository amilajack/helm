// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowserSynth.h"
#include "BrowserControl.h"
#include "default_look_and_feel.h"
#include "helm_computer_keyboard.h"
#include <emscripten.h>
#include <emscripten/eventloop.h>
extern "C" void helm_dispatch_messages();
static_assert(sizeof(void*)==4 && JUCE_LITTLE_ENDIAN, "The browser target requires little-endian wasm32");
BrowserSynth* helmSynth = nullptr;
class BrowserEditor : public Component, public SynthGuiInterface, public FileDragAndDropTarget {
public:
  explicit BrowserEditor(BrowserSynth* synth) : SynthGuiInterface(synth), keyboard(synth->getEngine(), synth->getKeyboardState()) {
    setName("Helm synthesizer");
    addAndMakeVisible(gui_);
    gui_->setOutputMemory(synth->getOutputMemory());
    addKeyListener(&keyboard);
    setWantsKeyboardFocus(true);
    setSize(mopo::DEFAULT_WINDOW_WIDTH, mopo::DEFAULT_WINDOW_HEIGHT);
  }
  void resized() override { gui_->setBounds(getLocalBounds()); }
  bool isInterestedInFileDrag(const StringArray& files) override {
    return files.size()==1 && files[0].endsWithIgnoreCase(".helm");
  }
  void filesDropped(const StringArray& files,int,int) override {
    if (isInterestedInFileDrag(files)) {
      const File file(files[0]);
      if (synth_->loadFromFile(file)) externalPatchLoaded(file);
    }
  }
private:
  HelmComputerKeyboard keyboard;
};
static BrowserEditor* editor;
extern "C" int helm_browser_font_generation;
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_fonts_added() {
  Typeface::scanFolderForFonts(File("/fonts/fallback"));
  ++helm_browser_font_generation;
  editor->repaint();
}
int main() {
  setenv("HOME", "/user", 1);
  File("/user/patches").createDirectory();
  File("/factory").createDirectory();
  static ScopedJuceInitialiser_GUI init;
  LookAndFeel::setDefaultLookAndFeel(DefaultLookAndFeel::instance());
  static BrowserSynth synth;
  helmSynth=&synth;
  synth.loadInitPatch();
  static BrowserEditor view(&synth);
  editor=&view;
  synth.editor=&view;
  helmAttachAudio(&synth);
  view.addToDesktop(0);
  view.setVisible(true);
  emscripten_set_interval([](void*) {
    helmPumpControls();
    if (!helmTryControl()) return;
    helm_dispatch_messages();
    helmSynth->controlTick();
    helmSynth->browserSnapshot();
    helmPollAudio();
    helmReleaseControl();
  }, 16, nullptr);
  emscripten_set_main_loop([] {
    editor->repaint();
    for (int i=0;i<ComponentPeer::getNumPeers();++i)
      ComponentPeer::getPeer(i)->performAnyPendingRepaintsNow();
  }, 0, false);
}
namespace juce {
bool BluetoothMidiDevicePairingDialogue::isAvailable() { return false; }
bool BluetoothMidiDevicePairingDialogue::open(ModalComponentManager::Callback* callback, Rectangle<int>*) {
  if (callback) {callback->modalStateFinished(0); delete callback;}
  return false;
}
}
