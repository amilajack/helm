// SPDX-License-Identifier: GPL-3.0-or-later
// Web MIDI is exposed through the browser's permission-gated MIDI input picker.
// Native UMP services and operating-system Bluetooth pairing are unavailable.
namespace juce::universal_midi_packets {
std::unique_ptr<Endpoints::Impl::Native> Endpoints::Impl::Native::make(EndpointsListener&) { return {}; }
}
