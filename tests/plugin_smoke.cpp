#include <juce_audio_utils/juce_audio_utils.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace juce;
static void require(bool condition, const String& message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

int main(int argc, char** argv) {
    ScopedJuceInitialiser_GUI gui;
    try {
        require(argc >= 3, "Usage: HelmPluginTests plugin.vst3 legacy-parameters.txt [--editor]");
        const bool isAU = String(argv[1]).endsWithIgnoreCase(".component");
        CFBundleRef auBundle = nullptr;
        std::unique_ptr<AudioPluginFormat> format;
        if (isAU) {
            // Register only in this test process; no installation or registry reset required.
            auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
                reinterpret_cast<const UInt8*>(argv[1]), std::strlen(argv[1]), true);
            auBundle = CFBundleCreate(nullptr, url);
            CFRelease(url);
            require(auBundle != nullptr, "Cannot open AU bundle");
            auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
                CFBundleGetFunctionPointerForName(auBundle, CFSTR("helmFactory")));
            require(factory != nullptr, "Cannot load AU factory");
            AudioComponentDescription description {'aumu', 'Helm', 'Tyte', 0, 0};
            require(AudioComponentRegister(&description, CFSTR("Matt Tytel: Helm"), 0x900, factory) != nullptr,
                    "Cannot register AU factory");
            format = std::make_unique<AudioUnitPluginFormat>();
        } else {
            format = std::make_unique<VST3PluginFormat>();
        }
        OwnedArray<PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, argv[1]);
        require(descriptions.size() == 1, "Expected one instrument");
        require(descriptions[0]->isInstrument, "Plugin must be an instrument");
        String error;
        auto plugin = format->createInstanceFromDescription(*descriptions[0], 48000, 256, error);
        require(plugin != nullptr, "Plugin load failed: " + error);
        require(plugin->acceptsMidi(), "MIDI input unavailable");
        auto names = StringArray::fromLines(File(argv[2]).loadFileAsString());
        names.removeEmptyStrings();
        auto& parameters = plugin->getParameters();
        require(parameters.size() >= names.size(), "Missing parameters");
        for (int i = 0; i < names.size(); ++i) {
            require(parameters[i]->getName(128) == names[i],
                    "Parameter " + String(i) + ": expected " + names[i] + ", got " + parameters[i]->getName(128));
            auto* hosted = dynamic_cast<HostedAudioProcessorParameter*>(parameters[i]);
            // JUCE 5 hashes the decimal index for plain AudioProcessorParameter objects.
            const auto legacyID = static_cast<uint32>(String(i).hashCode()) & 0x7fffffff;
            require(hosted != nullptr && hosted->getParameterID() == String(legacyID),
                    "Legacy parameter ID changed at " + String(i));
        }
        std::cout << "Verified " << names.size() << " legacy parameter names and IDs\n";

        juce::AudioBuffer<float> audio(2, 256);
        MidiBuffer midi;
        plugin->prepareToPlay(48000, 256);
        auto process = [&] { audio.clear(); plugin->processBlock(audio, midi); midi.clear(); };
        process();
        std::vector<float> expected;
        for (int i = 0; i < names.size(); ++i) expected.push_back(parameters[i]->getValue());
        MemoryBlock state;
        plugin->getStateInformation(state);
        require(state.getSize() > 0, "Empty saved state");
        parameters[0]->setValueNotifyingHost(0.75f);
        process();
        require(std::abs(parameters[0]->getValue() - 0.75f) < 1.0e-5f, "Automation did not reach the synth");
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        process();
        for (int i = 0; i < names.size(); ++i)
            require(std::abs(parameters[i]->getValue() - expected[i]) < 1.0e-5f,
                    "State restore changed " + names[i]);
        if (!isAU) {
            // Reconstruct JUCE 5.3.2's component-state envelope independently of the current wrapper.
            // A minimal Helm 0.9.0 preset uses synth-space attack=1 (normalised=0.25).
            const String legacyJSON = R"({"synth_version":"0.9.0","settings":{"amp_attack":1,"modulations":[]}})";
            MemoryOutputStream payload;
            payload.writeString(legacyJSON);
            MemoryOutputStream component;
            component.writeIntBigEndian(0x56737457); // VstW
            component.writeIntBigEndian(8);
            component.writeIntBigEndian(1);
            component.writeIntBigEndian(0);
            component.writeIntBigEndian(0x43636e4b); // CcnK
            component.writeIntBigEndian(152 + static_cast<int>(payload.getDataSize()));
            component.writeIntBigEndian(0x46424368); // FBCh
            component.writeIntBigEndian(2);
            component.writeIntBigEndian(0x48656c6d); // Helm
            component.writeIntBigEndian(0x900);
            component.writeIntBigEndian(0);
            component.writeIntBigEndian(0);
            const char reserved[124] {};
            component.write(reserved, sizeof(reserved));
            component.writeIntBigEndian(static_cast<int>(payload.getDataSize()));
            component.write(payload.getData(), payload.getDataSize());
            XmlElement oldState("VST3PluginState");
            oldState.createNewChildElement("IComponent")->addTextElement(
                component.getMemoryBlock().toBase64Encoding());
            MemoryBlock oldHostState;
            AudioProcessor::copyXmlToBinary(oldState, oldHostState);
            plugin->setStateInformation(oldHostState.getData(), static_cast<int>(oldHostState.getSize()));
            process();
            require(std::abs(parameters[0]->getValue() - 0.25f) < 1.0e-5f,
                    "JUCE 5 VST3 session state did not restore");
            plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            process();
        }
        require(plugin->getNumPrograms() > 1, "Bundled factory presets not discovered");
        // Helm intentionally ignores program changes for 500 ms after restoring a session.
        MessageManager::getInstance()->runDispatchLoopUntil(600);
        plugin->setCurrentProgram(1);
        process();
        MemoryBlock presetState;
        plugin->getStateInformation(presetState);
        require(presetState != state, "Factory preset did not change state");
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        process();
        plugin->releaseResources();

        for (auto channels : {1, 2}) {
            auto layout = plugin->getBusesLayout();
            layout.outputBuses.set(0, channels == 1 ? AudioChannelSet::mono() : AudioChannelSet::stereo());
            require(plugin->setBusesLayout(layout), "Unsupported output layout");
            for (auto rate : {44100.0, 48000.0, 96000.0}) {
                for (auto block : {1, 64, 255, 256, 257, 1024}) {
                    audio.setSize(channels, block);
                    plugin->prepareToPlay(rate, block);
                    midi.addEvent(MidiMessage::noteOn(1, 60, (uint8)100), 0);
                    midi.addEvent(MidiMessage::pitchWheel(1, 9000), 0);
                    midi.addEvent(MidiMessage::controllerEvent(1, 64, 127), 0);
                    float peak = 0;
                    for (int b = 0; b < static_cast<int>(rate / block / 10) + 1; ++b) {
                        process();
                        for (int c = 0; c < channels; ++c)
                            for (int s = 0; s < block; ++s) {
                                require(std::isfinite(audio.getSample(c, s)), "Non-finite audio");
                                peak = std::max(peak, std::abs(audio.getSample(c, s)));
                            }
                    }
                    require(peak > 1.0e-6f, "Silent note output");
                    midi.addEvent(MidiMessage::noteOff(1, 60), 0);
                    midi.addEvent(MidiMessage::controllerEvent(1, 64, 0), 0);
                    midi.addEvent(MidiMessage::allNotesOff(1), 0);
                    for (int b = 0; b < static_cast<int>(rate * 2 / block) + 1; ++b) process();
                    require(audio.getMagnitude(0, block) < 1.0e-4f, "Note did not release");
                    plugin->releaseResources();
                }
            }
        }
        if (argc > 3 && String(argv[3]) == "--editor") {
            for (int i = 0; i < 3; ++i) {
                std::unique_ptr<AudioProcessorEditor> editor(plugin->createEditorIfNeeded());
                require(editor != nullptr, "Editor unavailable");
                editor->addToDesktop(ComponentPeer::windowIsTemporary);
                editor->setVisible(true);
                MessageManager::getInstance()->runDispatchLoopUntil(500);
                editor->setSize(editor->getWidth() * 3 / 4, editor->getHeight() * 3 / 4);
                MessageManager::getInstance()->runDispatchLoopUntil(500);
                plugin->editorBeingDeleted(editor.get());
            }
        }
        std::cout << "PASS: plugin state, presets, mono/stereo, MIDI, audio and release tails\n";
        plugin.reset();
        if (auBundle != nullptr) CFRelease(auBundle);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
