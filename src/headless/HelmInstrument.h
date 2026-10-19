// SPDX-License-Identifier: GPL-3.0-or-later
// Headless Helm instrument shared by the WAM and CLAP/WCLAP targets. It wraps the
// same production engine as AU/VST3 and the standalone app.
#pragma once
#include "helm_engine.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

extern "C" thread_local int helm_audio_callback_depth;
extern "C" int helm_browser_audio_pool_reserve(size_t);

namespace helm {
struct ParameterInfo {
  std::string id, label, units;
  double minimum, maximum, defaultValue;
  bool stepped;
  int skew;  // mopo::ValueDetails::DisplaySkew
  double offset, multiplier;
  bool invert;
};

struct ModulationSource {
  std::string id;
  std::vector<std::string> destinations;
};

// Display text using the same transform as the original editor and WAM GUI.
std::string formatParameter(const ParameterInfo& info, double value);

class Instrument {
 public:
  static constexpr int maxFrames = 256;
  explicit Instrument(int sampleRate);
  ~Instrument();
  Instrument(const Instrument&) = delete;
  Instrument& operator=(const Instrument&) = delete;

  const std::vector<ParameterInfo>& parameters() const { return infos_; }
  const std::vector<ModulationSource>& modulationSources() const { return sources_; }
  const std::string& metadataJson() const { return metadata_; }
  int indexOf(const std::string& id) const;

  bool set(int index, double value);
  double get(int index) const;
  void clearRoutes();
  bool route(const std::string& source, const std::string& destination, double amount);
  // Ends held notes and adopts routing/arp changes before new events arrive.
  void prepareState();
  void midi(int status, int first, int second);
  void noteOn(int key, double velocity, int channel);
  void noteOff(int key);
  void allNotesOff();
  void transport(double ppq, double bpm);
  // Renders up to maxFrames; left at output()[0..frames), right at output()[maxFrames..].
  const float* render(int frames);
  void setSampleRate(int sampleRate);
  int sampleRate() const;

 private:
  mopo::HelmEngine engine_;
  std::vector<mopo::Value*> values_;
  std::vector<ParameterInfo> infos_;
  std::vector<ModulationSource> sources_;
  std::vector<std::unique_ptr<mopo::ModulationConnection>> routes_;
  std::array<float, 2 * maxFrames> output_{};
  std::string metadata_;
};
}  // namespace helm
