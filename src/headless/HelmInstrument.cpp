// SPDX-License-Identifier: GPL-3.0-or-later
#include "HelmInstrument.h"
#include "helm_common.h"
#include <cmath>
#include <iomanip>
#include <sstream>

namespace helm {
namespace {
std::string quote(const std::string& value) {
  std::string result = "\"";
  for (auto c : value) {
    if (c == '"' || c == '\\') result += '\\';
    if (c == '\n') { result += "\\n"; continue; }
    result += c;
  }
  return result + '"';
}
}  // namespace

std::string formatParameter(const ParameterInfo& info, double value) {
  double shown = info.skew == 1 ? value * value
               : info.skew == 2 ? std::sqrt(value)
               : info.skew == 3 ? std::pow(2.0, value) : value;
  shown += info.offset;
  if (info.invert) shown = 1 / shown;
  shown *= info.multiplier;
  std::ostringstream text;
  if (std::isfinite(shown)) text << std::setprecision(5) << shown;
  else text << "∞";
  if (!info.units.empty()) text << ' ' << info.units;
  return text.str();
}

Instrument::Instrument(int sampleRate) {
  engine_.setSampleRate(sampleRate);
  engine_.setBufferSize(128);
  const auto controls = engine_.getControls();
  std::ostringstream json;
  json << std::setprecision(17) << "{\"parameters\":[";
  bool first = true;
  for (const auto& entry : controls) {
    const auto& details = mopo::Parameters::getDetails(entry.first);
    values_.push_back(entry.second);
    entry.second->set(details.default_value);
    // Preserve the native reader's historical factory-preset ranges.
    double minimum = details.min, maximum = details.max;
    if (entry.first == "cutoff") minimum = 0;
    if (entry.first == "stutter_frequency") minimum = -7;
    if (entry.first == "distortion_drive") maximum = 72;
    infos_.push_back({entry.first, details.display_name, details.display_units, minimum, maximum,
                      details.default_value, details.steps > 1, int(details.display_skew),
                      details.post_offset, details.display_multiply, details.display_invert});
    if (!first) json << ',';
    first = false;
    json << "{\"id\":" << quote(entry.first) << ",\"label\":" << quote(details.display_name)
         << ",\"minValue\":" << minimum << ",\"maxValue\":" << maximum
         << ",\"defaultValue\":" << details.default_value << ",\"discreteStep\":0,\"nativeStep\":"
         << (details.steps > 1 ? 1 : 0) << ",\"display\":{\"skew\":" << int(details.display_skew)
         << ",\"offset\":" << details.post_offset
         << ",\"invert\":" << (details.display_invert ? "true" : "false")
         << ",\"multiplier\":" << details.display_multiply
         << ",\"units\":" << quote(details.display_units) << "}}";
  }
  json << "],\"modulationSources\":[";
  first = true;
  for (const auto& source : engine_.getModulationSources()) {
    ModulationSource described{source.first, {}};
    if (!first) json << ',';
    first = false;
    json << "{\"id\":" << quote(source.first) << ",\"destinations\":[";
    bool firstDestination = true;
    for (const auto& info : infos_)
      if (engine_.getModulationDestination(info.id, source.second->owner->isPolyphonic())) {
        if (!firstDestination) json << ',';
        firstDestination = false;
        json << quote(info.id);
        described.destinations.push_back(info.id);
      }
    json << "]}";
    sources_.push_back(std::move(described));
  }
  json << "]}";
  metadata_ = json.str();
  engine_.process();  // Adopt the initial routing/arp controls before notes arrive.
}

Instrument::~Instrument() { clearRoutes(); }

int Instrument::indexOf(const std::string& id) const {
  for (size_t i = 0; i < infos_.size(); ++i)
    if (infos_[i].id == id) return int(i);
  return -1;
}

bool Instrument::set(int index, double value) {
  if (index < 0 || size_t(index) >= values_.size() || !std::isfinite(value)) return false;
  values_[size_t(index)]->set(value);
  return true;
}

double Instrument::get(int index) const {
  return index >= 0 && size_t(index) < values_.size() ? values_[size_t(index)]->value() : 0;
}

void Instrument::clearRoutes() {
  for (const auto& route : routes_) engine_.disconnectModulation(route.get());
  routes_.clear();
}

bool Instrument::route(const std::string& source, const std::string& destination, double amount) {
  if (!std::isfinite(amount) || std::abs(amount) > 1e6 || routes_.size() >= 256) return false;
  const auto sources = engine_.getModulationSources();
  const auto found = sources.find(source);
  if (found == sources.end() ||
      !engine_.getModulationDestination(destination, found->second->owner->isPolyphonic()))
    return false;
  auto connection = std::make_unique<mopo::ModulationConnection>(source, destination);
  connection->amount.set(amount);
  engine_.connectModulation(connection.get());
  routes_.push_back(std::move(connection));
  return true;
}

void Instrument::prepareState() {
  engine_.sustainOff();
  engine_.allNotesOff();
  engine_.setBufferSize(128);
  engine_.process();
}

void Instrument::midi(int status, int first, int second) {
  if (status < 0x80 || status >= 0xf0 || first < 0 || first > 127 || second < 0 || second > 127)
    return;
  const int type = status & 0xf0, channel = status & 15;
  if (type == 0x90 && second) engine_.noteOn(first, second / 127., 0, channel);
  else if (type == 0x80 || type == 0x90) engine_.noteOff(first);
  else if (type == 0xa0) engine_.setAftertouch(first, second / double(mopo::MIDI_SIZE));
  else if (type == 0xd0) engine_.setChannelAftertouch(channel + 1, first / 127.);
  else if (type == 0xe0) engine_.setPitchWheel(2. * (first + second * 128) / 16383. - 1., channel + 1);
  else if (type == 0xb0) {
    if (first == 1) engine_.setModWheel(second / 127., channel + 1);
    else if (first == 64) {
      if (second >= 64) engine_.sustainOn();
      else engine_.sustainOff();
    } else if (first == 120 || first == 123) allNotesOff();
  }
}

void Instrument::noteOn(int key, double velocity, int channel) {
  if (key < 0 || key > 127 || !std::isfinite(velocity)) return;
  if (velocity <= 0) { engine_.noteOff(key); return; }
  engine_.noteOn(key, std::min(velocity, 1.0), 0, channel < 0 ? 0 : channel & 15);
}

void Instrument::noteOff(int key) {
  if (key >= 0 && key <= 127) engine_.noteOff(key);
}

void Instrument::allNotesOff() {
  engine_.sustainOff();
  engine_.allNotesOff();
}

void Instrument::transport(double ppq, double bpm) {
  if (std::isfinite(ppq) && std::isfinite(bpm) && bpm > 0)
    engine_.correctToTime(ppq * 60. / bpm * engine_.getSampleRate());
}

const float* Instrument::render(int frames) {
  if (frames < 1 || frames > maxFrames) return nullptr;
  ++helm_audio_callback_depth;
  engine_.setBufferSize(frames);
  engine_.process();
  for (int i = 0; i < frames; ++i) {
    output_[size_t(i)] = float(engine_.output(0)->buffer[i]);
    output_[size_t(maxFrames + i)] = float(engine_.output(1)->buffer[i]);
  }
  --helm_audio_callback_depth;
  return output_.data();
}

void Instrument::setSampleRate(int sampleRate) { engine_.setSampleRate(sampleRate); }
int Instrument::sampleRate() const { return engine_.getSampleRate(); }
}  // namespace helm
