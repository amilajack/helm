// SPDX-License-Identifier: GPL-3.0-or-later
// Helm as a CLAP instrument. The same code builds a native `.clap` and a WCLAP
// (CLAP compiled to wasm32 with wasi-sdk). It uses only the CLAP C API, not JUCE.
#include "HelmInstrument.h"
#include "HelmState.h"
#include "WebviewResources.h"
#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using helm::Instrument;

std::string bundlePath;

constexpr uint32_t guiWidth = 760, guiHeight = 720;
constexpr size_t maxStateBytes = 4 << 20;

uint32_t fnv1a(const std::string& text) {
  uint32_t hash = 0x811c9dc5u;
  for (const unsigned char c : text) hash = (hash ^ c) * 0x01000193u;
  return hash;
}

// Spin lock that the audio thread only ever tries; it never waits on the GUI.
class TryLock {
 public:
  bool tryLock() { return !flag_.test_and_set(std::memory_order_acquire); }
  void lock() { while (flag_.test_and_set(std::memory_order_acquire)) {} }
  void unlock() { flag_.clear(std::memory_order_release); }
 private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// Main thread → audio thread commands (GUI edits and audition notes).
struct Command {
  enum Kind : uint8_t { Param, GestureBegin, GestureEnd, Midi } kind;
  int32_t index;
  double value;
  std::array<uint8_t, 3> bytes;
};
class CommandQueue {
 public:
  static constexpr uint32_t capacity = 1024;
  bool push(const Command& command) {
    const auto write = head_.load(std::memory_order_relaxed);
    if (write - tail_.load(std::memory_order_acquire) >= capacity) return false;
    commands_[write % capacity] = command;
    head_.store(write + 1, std::memory_order_release);
    return true;
  }
  template <class Deliver> void drain(Deliver deliver) {
    auto read = tail_.load(std::memory_order_relaxed);
    const auto end = head_.load(std::memory_order_acquire);
    for (; read != end; ++read) deliver(commands_[read % capacity]);
    tail_.store(read, std::memory_order_release);
  }
 private:
  std::array<Command, capacity> commands_{};
  alignas(64) std::atomic<uint32_t> head_{0};
  alignas(64) std::atomic<uint32_t> tail_{0};
};

std::string resourcesDirectory() {
  // macOS bundles keep files in Contents/Resources; WCLAP bundles keep them at the root.
  std::ifstream probe(bundlePath + "/Contents/Resources/library/manifest.json");
  return probe ? bundlePath + "/Contents/Resources" : bundlePath;
}

bool isLibraryPath(const std::string& path) {
  if (path == "/library/manifest.json") return true;
  static const std::string prefix = "/library/objects/";
  if (path.compare(0, prefix.size(), prefix) != 0 || path.size() != prefix.size() + 64) return false;
  return std::all_of(path.begin() + long(prefix.size()), path.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool writeAll(const clap_ostream_t* stream, const void* data, size_t size) {
  auto bytes = static_cast<const uint8_t*>(data);
  while (size) {
    const auto written = stream->write(stream, bytes, size);
    if (written <= 0) return false;
    bytes += written;
    size -= size_t(written);
  }
  return true;
}

class Plugin {
 public:
  Plugin(const clap_host_t* host, const clap_plugin_descriptor_t* descriptor) : host_(host) {
    plugin_.desc = descriptor;
    plugin_.plugin_data = this;
    plugin_.init = [](const clap_plugin_t* p) { return self(p)->init(); };
    plugin_.destroy = [](const clap_plugin_t* p) { delete self(p); };
    plugin_.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t) { return self(p)->activate(rate); };
    plugin_.deactivate = [](const clap_plugin_t*) {};
    plugin_.start_processing = [](const clap_plugin_t*) { return true; };
    plugin_.stop_processing = [](const clap_plugin_t*) {};
    plugin_.reset = [](const clap_plugin_t* p) { self(p)->reset(); };
    plugin_.process = [](const clap_plugin_t* p, const clap_process_t* process) { return self(p)->process(process); };
    plugin_.get_extension = [](const clap_plugin_t* p, const char* id) { return self(p)->extension(id); };
    plugin_.on_main_thread = [](const clap_plugin_t*) {};
  }
  const clap_plugin_t* clap() const { return &plugin_; }

 private:
  static Plugin* self(const clap_plugin_t* plugin) { return static_cast<Plugin*>(plugin->plugin_data); }

  // ---- lifecycle -----------------------------------------------------------
  bool init() {
    if (!helm_browser_audio_pool_reserve(size_t(32) << 20)) return false;
    instrument_ = std::make_unique<Instrument>(48000);
    const auto& parameters = instrument_->parameters();
    shadow_ = std::make_unique<std::atomic<double>[]>(parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) {
      const auto id = fnv1a(parameters[i].id);
      if (id == CLAP_INVALID_ID) return false;
      for (const auto other : ids_)
        if (other == id) return false;  // Parameter ids must stay unique and stable.
      ids_.push_back(id);
      shadow_[i].store(parameters[i].defaultValue, std::memory_order_relaxed);
    }
    state_ = helm::defaultState(*instrument_);
    hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostWebview_ = static_cast<const clap_host_webview_t*>(host_->get_extension(host_, CLAP_EXT_WEBVIEW));
    return true;
  }

  bool activate(double rate) {
    // mopo sizes its buffers for at most MAX_SAMPLE_RATE (192 kHz).
    if (!(rate >= 1000 && rate <= mopo::MAX_SAMPLE_RATE)) return false;
    lock_.lock();
    instrument_->setSampleRate(int(std::lround(rate)));
    instrument_->prepareState();
    lock_.unlock();
    return true;
  }

  // [audio-thread] Never waits: if a patch is loading, release notes next block.
  void reset() {
    if (!lock_.tryLock()) { pendingReset_ = true; return; }
    instrument_->allNotesOff();
    lock_.unlock();
  }

  int indexOf(clap_id id) const {
    for (size_t i = 0; i < ids_.size(); ++i)
      if (ids_[i] == id) return int(i);
    return -1;
  }

  // ---- audio thread --------------------------------------------------------
  void setParameter(int index, double value) {
    const auto& info = instrument_->parameters()[size_t(index)];
    value = std::clamp(value, info.minimum, info.maximum);
    instrument_->set(index, value);
    shadow_[size_t(index)].store(value, std::memory_order_relaxed);
  }

  void applyEvent(const clap_event_header_t* header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (header->type) {
      case CLAP_EVENT_NOTE_ON: {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
        if (note->key >= 0) instrument_->noteOn(note->key, note->velocity, note->channel);
        break;
      }
      case CLAP_EVENT_NOTE_OFF:
      case CLAP_EVENT_NOTE_CHOKE: {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
        if (note->key >= 0) instrument_->noteOff(note->key);
        else instrument_->allNotesOff();
        break;
      }
      case CLAP_EVENT_MIDI: {
        const auto* midi = reinterpret_cast<const clap_event_midi_t*>(header);
        instrument_->midi(midi->data[0], midi->data[1] & 127, midi->data[2] & 127);
        break;
      }
      case CLAP_EVENT_PARAM_VALUE: {
        const auto* param = reinterpret_cast<const clap_event_param_value_t*>(header);
        const int index = indexOf(param->param_id);
        if (index >= 0 && std::isfinite(param->value)) setParameter(index, param->value);
        break;
      }
      default: break;
    }
  }

  void pushOut(const clap_output_events_t* out, clap_id id, uint16_t type, double value) {
    if (!out) return;
    if (type == CLAP_EVENT_PARAM_VALUE) {
      clap_event_param_value_t event{{sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                                      CLAP_EVENT_PARAM_VALUE, 0}, id, nullptr, -1, -1, -1, -1, value};
      out->try_push(out, &event.header);
    } else {
      clap_event_param_gesture_t event{{sizeof(clap_event_param_gesture_t), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0}, id};
      out->try_push(out, &event.header);
    }
  }

  // GUI edits enter here: applied to the engine and reported to the host.
  void drainCommands(const clap_output_events_t* out) {
    commands_.drain([&](const Command& command) {
      const auto id = ids_[size_t(command.index)];
      switch (command.kind) {
        case Command::Param:
          setParameter(command.index, command.value);
          pushOut(out, id, CLAP_EVENT_PARAM_VALUE, command.value);
          break;
        case Command::GestureBegin: pushOut(out, id, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0); break;
        case Command::GestureEnd: pushOut(out, id, CLAP_EVENT_PARAM_GESTURE_END, 0); break;
        case Command::Midi: instrument_->midi(command.bytes[0], command.bytes[1], command.bytes[2]); break;
      }
    });
  }

  void transport(const clap_event_transport_t* transport) {
    if (!transport) return;
    const bool hasTempo = transport->flags & CLAP_TRANSPORT_HAS_TEMPO;
    if (hasTempo && transport->tempo >= 20 && transport->tempo <= 300 && transport->tempo != lastTempo_) {
      lastTempo_ = transport->tempo;
      const int bpm = instrument_->indexOf("beats_per_minute");
      if (bpm >= 0) setParameter(bpm, transport->tempo / 60);
    }
    const bool playing = transport->flags & CLAP_TRANSPORT_IS_PLAYING;
    // Follow the host while playing; while stopped, align once to the cursor.
    if (hasTempo && (transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (playing || playing != wasPlaying_ || !aligned_)) {
      instrument_->transport(double(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR, transport->tempo);
      aligned_ = true;
    }
    wasPlaying_ = playing;
  }

  clap_process_status process(const clap_process_t* process) {
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 ||
        !process->audio_outputs[0].data32)
      return CLAP_PROCESS_ERROR;
    float* left = process->audio_outputs[0].data32[0];
    float* right = process->audio_outputs[0].data32[1];
    const uint32_t frames = process->frames_count;
    const auto* in = process->in_events;
    const uint32_t count = in ? in->size(in) : 0;
    if (!lock_.tryLock()) {
      // The GUI is replacing the patch. Keep parameter values, render silence.
      for (uint32_t i = 0; i < count; ++i) {
        const auto* header = in->get(in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
          const auto* param = reinterpret_cast<const clap_event_param_value_t*>(header);
          const int index = indexOf(param->param_id);
          if (index >= 0 && std::isfinite(param->value)) shadow_[size_t(index)].store(param->value, std::memory_order_relaxed);
        }
      }
      resync_ = true;
      std::fill(left, left + frames, 0.f);
      std::fill(right, right + frames, 0.f);
      return CLAP_PROCESS_CONTINUE;
    }
    ++helm_audio_callback_depth;
    if (pendingReset_) {
      pendingReset_ = false;
      instrument_->allNotesOff();
    }
    if (resync_) {
      resync_ = false;
      for (size_t i = 0; i < ids_.size(); ++i) instrument_->set(int(i), shadow_[i].load(std::memory_order_relaxed));
    }
    drainCommands(process->out_events);
    transport(process->transport);
    uint32_t frame = 0, next = 0;
    while (frame < frames) {
      while (next < count) {
        const auto* header = in->get(in, next);
        if (header->time > frame) break;
        applyEvent(header);
        ++next;
      }
      uint32_t end = frames;
      if (next < count) end = std::min(end, in->get(in, next)->time);
      const uint32_t chunk = std::min<uint32_t>(end - frame, Instrument::maxFrames);
      const float* output = instrument_->render(int(chunk));
      std::copy(output, output + chunk, left + frame);
      std::copy(output + Instrument::maxFrames, output + Instrument::maxFrames + chunk, right + frame);
      frame += chunk;
    }
    for (; next < count; ++next) applyEvent(in->get(in, next));
    --helm_audio_callback_depth;
    lock_.unlock();
    return CLAP_PROCESS_CONTINUE;
  }

  void flush(const clap_input_events_t* in, const clap_output_events_t* out) {
    // Called on the audio thread while active, otherwise on the main thread.
    if (!lock_.tryLock()) return;
    drainCommands(out);
    for (uint32_t i = 0, n = in ? in->size(in) : 0; i < n; ++i) {
      const auto* header = in->get(in, i);
      if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) applyEvent(header);
    }
    lock_.unlock();
  }

  // ---- main thread: state --------------------------------------------------
  helm::State currentState() const {
    auto state = state_;
    for (size_t i = 0; i < state.values.size(); ++i) state.values[i] = shadow_[i].load(std::memory_order_relaxed);
    return state;
  }

  bool applyState(const helm::State& state, std::string& error) {
    lock_.lock();
    // Commands queued before the new patch refer to the old one.
    commands_.drain([](const Command&) {});
    for (size_t i = 0; i < state.values.size(); ++i) instrument_->set(int(i), state.values[i]);
    instrument_->clearRoutes();
    bool ok = true;
    for (const auto& route : state.routes)
      ok = instrument_->route(route.source, route.destination, route.amount) && ok;
    instrument_->prepareState();
    for (size_t i = 0; i < state.values.size(); ++i) shadow_[i].store(state.values[i], std::memory_order_relaxed);
    resync_ = false;
    lock_.unlock();
    state_ = state;
    if (!ok) error = "Unable to construct modulation route";
    if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    return ok;
  }

  bool saveState(const clap_ostream_t* stream) {
    const auto json = helm::stateToJson(currentState(), *instrument_);
    return writeAll(stream, json.data(), json.size());
  }

  bool loadState(const clap_istream_t* stream) {
    std::string text;
    char buffer[4096];
    while (true) {
      const auto read = stream->read(stream, buffer, sizeof buffer);
      if (read < 0) return false;
      if (read == 0) break;
      text.append(buffer, size_t(read));
      if (text.size() > maxStateBytes) return false;
    }
    helm::State state;
    std::string error;
    return helm::parseState(text, *instrument_, state, error) && applyState(state, error);
  }

  // ---- main thread: GUI edits ----------------------------------------------
  bool queue(const Command& command) {
    if (!commands_.push(command)) return false;
    if (hostParams_) hostParams_->request_flush(host_);
    return true;
  }

  // ---- main thread: webview messages ---------------------------------------
  void send(const std::string& message) {
    if (hostWebview_) hostWebview_->send(host_, message.data(), uint32_t(message.size()));
  }

  void reply(const helm::Json* rid, const std::string& type, const std::string& body) {
    std::string message = "{\"type\":" + helm::jsonQuote(type);
    if (rid && rid->type == helm::Json::Number) message += ",\"rid\":" + helm::jsonNumber(rid->number);
    send(message + body + "}");
  }

  bool receive(const void* buffer, uint32_t size) {
    if (!buffer || size > maxStateBytes) return false;
    helm::Json message;
    std::string error;
    if (!helm::Json::parse(std::string(static_cast<const char*>(buffer), size), message, error)) return false;
    const auto* type = message.find("type");
    const auto* rid = message.find("rid");
    if (!type || type->type != helm::Json::String) return false;
    const auto& kind = type->string;
    if (kind == "metadata") {
      reply(rid, "metadata", ",\"metadata\":" + instrument_->metadataJson());
      return true;
    }
    if (kind == "getState") {
      reply(rid, "state", ",\"state\":" + helm::stateToJson(currentState(), *instrument_));
      return true;
    }
    if (kind == "setState") {
      helm::State state;
      const auto* input = message.find("state");
      if (!input || !helm::stateFromJson(*input, *instrument_, state, error) || !applyState(state, error)) {
        reply(rid, "error", ",\"message\":" + helm::jsonQuote(error));
        return true;
      }
      if (hostState_) hostState_->mark_dirty(host_);
      reply(rid, "state", ",\"state\":" + helm::stateToJson(currentState(), *instrument_));
      return true;
    }
    if (kind == "param" || kind == "gesture") {
      const auto* id = message.find("id");
      const int index = id && id->type == helm::Json::String ? instrument_->indexOf(id->string) : -1;
      if (index < 0) return false;
      if (kind == "gesture") {
        const auto* begin = message.find("begin");
        if (!begin || begin->type != helm::Json::Bool) return false;
        return queue({begin->boolean ? Command::GestureBegin : Command::GestureEnd, index, 0, {}});
      }
      const auto* value = message.find("value");
      const auto& info = instrument_->parameters()[size_t(index)];
      if (!value || value->type != helm::Json::Number || value->number < info.minimum || value->number > info.maximum)
        return false;
      if (!queue({Command::Param, index, value->number, {}})) return false;
      shadow_[size_t(index)].store(value->number, std::memory_order_relaxed);
      return true;
    }
    if (kind == "midi") {
      const auto* bytes = message.find("bytes");
      if (!bytes || bytes->type != helm::Json::Array || bytes->items.size() < 2 || bytes->items.size() > 3) return false;
      std::array<uint8_t, 3> data{};
      for (size_t i = 0; i < bytes->items.size(); ++i) {
        const auto& item = bytes->items[i];
        const double limit = i ? 127 : 239;
        if (item.type != helm::Json::Number || item.number != std::floor(item.number) || item.number < 0 || item.number > limit)
          return false;
        data[i] = uint8_t(item.number);
      }
      if (data[0] < 0x80) return false;
      return queue({Command::Midi, 0, 0, data});
    }
    return false;
  }

  bool resource(const char* rawPath, char* mime, uint32_t mimeCapacity, const clap_ostream_t* stream) {
    if (!rawPath || !stream) return false;
    const std::string path = rawPath;
    const auto provide = [&](const char* type, const void* data, size_t size) {
      if (mime && mimeCapacity) {
        std::strncpy(mime, type, mimeCapacity - 1);
        mime[mimeCapacity - 1] = '\0';
      }
      return writeAll(stream, data, size);
    };
    for (uint32_t i = 0; i < helm::webviewResourceCount; ++i) {
      const auto& file = helm::webviewResources[i];
      if (path == file.path || (path == "/" && std::strcmp(file.path, "/index.html") == 0))
        return provide(file.mime, file.data, file.size);
    }
    if (!isLibraryPath(path)) return false;  // Only the fixed, hash-addressed factory library.
    std::ifstream file(resourcesDirectory() + path, std::ios::binary);
    if (!file) return false;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return provide(path == "/library/manifest.json" ? "application/json" : "application/octet-stream",
                   data.data(), data.size());
  }

  // ---- extensions ----------------------------------------------------------
  const void* extension(const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &stateExtension;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
    if (!std::strcmp(id, CLAP_EXT_WEBVIEW)) return &webview;
    return nullptr;
  }

  static const clap_plugin_audio_ports_t audioPorts;
  static const clap_plugin_note_ports_t notePorts;
  static const clap_plugin_params_t params;
  static const clap_plugin_state_t stateExtension;
  static const clap_plugin_gui_t gui;
  static const clap_plugin_webview_t webview;

  clap_plugin_t plugin_{};
  const clap_host_t* host_;
  const clap_host_params_t* hostParams_ = nullptr;
  const clap_host_state_t* hostState_ = nullptr;
  const clap_host_webview_t* hostWebview_ = nullptr;
  std::unique_ptr<Instrument> instrument_;
  std::unique_ptr<std::atomic<double>[]> shadow_;
  std::vector<clap_id> ids_;
  helm::State state_;  // routes and patch information; values live in shadow_
  CommandQueue commands_;
  TryLock lock_;
  bool resync_ = false, pendingReset_ = false, wasPlaying_ = false, aligned_ = false;
  double lastTempo_ = 0;
};

const clap_plugin_audio_ports_t Plugin::audioPorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 0 : 1; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) {
      if (input || index) return false;
      info->id = 0;
      std::snprintf(info->name, sizeof info->name, "%s", "Output");
      info->flags = CLAP_AUDIO_PORT_IS_MAIN;
      info->channel_count = 2;
      info->port_type = CLAP_PORT_STEREO;
      info->in_place_pair = CLAP_INVALID_ID;
      return true;
    }};

const clap_plugin_note_ports_t Plugin::notePorts = {
    [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 1 : 0; },
    [](const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* info) {
      if (!input || index) return false;
      info->id = 0;
      info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
      std::snprintf(info->name, sizeof info->name, "%s", "Notes");
      return true;
    }};

const clap_plugin_params_t Plugin::params = {
    [](const clap_plugin_t* p) { return uint32_t(self(p)->ids_.size()); },
    [](const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
      auto* plugin = self(p);
      if (index >= plugin->ids_.size()) return false;
      const auto& parameter = plugin->instrument_->parameters()[index];
      std::memset(info, 0, sizeof *info);
      info->id = plugin->ids_[index];
      info->flags = CLAP_PARAM_IS_AUTOMATABLE | (parameter.stepped ? CLAP_PARAM_IS_STEPPED : 0);
      info->cookie = nullptr;
      std::snprintf(info->name, sizeof info->name, "%s",
                    (parameter.label.empty() ? parameter.id : parameter.label).c_str());
      info->min_value = parameter.minimum;
      info->max_value = parameter.maximum;
      info->default_value = parameter.defaultValue;
      return true;
    },
    [](const clap_plugin_t* p, clap_id id, double* value) {
      const int index = self(p)->indexOf(id);
      if (index < 0) return false;
      *value = self(p)->shadow_[size_t(index)].load(std::memory_order_relaxed);
      return true;
    },
    [](const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t capacity) {
      const int index = self(p)->indexOf(id);
      if (index < 0 || !capacity) return false;
      std::snprintf(text, capacity, "%s",
                    helm::formatParameter(self(p)->instrument_->parameters()[size_t(index)], value).c_str());
      return true;
    },
    [](const clap_plugin_t* p, clap_id id, const char* text, double* value) {
      const int index = self(p)->indexOf(id);
      if (index < 0 || !text) return false;
      char* end = nullptr;
      double shown = std::strtod(text, &end);
      if (end == text || !std::isfinite(shown)) return false;
      // Invert formatParameter(): multiplier, inversion, offset, then skew.
      const auto& info = self(p)->instrument_->parameters()[size_t(index)];
      if (info.multiplier) shown /= info.multiplier;
      if (info.invert) shown = 1 / shown;
      shown -= info.offset;
      double raw = info.skew == 1 ? std::sqrt(std::max(shown, 0.0))
                 : info.skew == 2 ? shown * shown
                 : info.skew == 3 ? std::log2(shown) : shown;
      if (!std::isfinite(raw)) return false;
      *value = std::clamp(raw, info.minimum, info.maximum);
      return true;
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
      self(p)->flush(in, out);
    }};

const clap_plugin_state_t Plugin::stateExtension = {
    [](const clap_plugin_t* p, const clap_ostream_t* stream) { return self(p)->saveState(stream); },
    [](const clap_plugin_t* p, const clap_istream_t* stream) { return self(p)->loadState(stream); }};

const clap_plugin_gui_t Plugin::gui = {
    [](const clap_plugin_t*, const char* api, bool floating) { return !floating && !std::strcmp(api, CLAP_WINDOW_API_WEBVIEW); },
    [](const clap_plugin_t*, const char** api, bool* floating) {
      *api = CLAP_WINDOW_API_WEBVIEW;
      *floating = false;
      return true;
    },
    // The host owns the webview; the plugin only serves resources and messages.
    [](const clap_plugin_t*, const char* api, bool floating) { return !floating && !std::strcmp(api, CLAP_WINDOW_API_WEBVIEW); },
    [](const clap_plugin_t*) {},
    [](const clap_plugin_t*, double) { return false; },
    [](const clap_plugin_t*, uint32_t* width, uint32_t* height) {
      *width = guiWidth;
      *height = guiHeight;
      return true;
    },
    [](const clap_plugin_t*) { return true; },
    [](const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
      hints->can_resize_horizontally = hints->can_resize_vertically = true;
      hints->preserve_aspect_ratio = false;
      hints->aspect_ratio_width = hints->aspect_ratio_height = 1;
      return true;
    },
    [](const clap_plugin_t*, uint32_t* width, uint32_t* height) {
      *width = std::max<uint32_t>(*width, 320);
      *height = std::max<uint32_t>(*height, 240);
      return true;
    },
    [](const clap_plugin_t*, uint32_t, uint32_t) { return true; },
    [](const clap_plugin_t*, const clap_window_t*) { return true; },
    [](const clap_plugin_t*, const clap_window_t*) { return true; },
    [](const clap_plugin_t*, const char*) {},
    [](const clap_plugin_t*) { return true; },
    [](const clap_plugin_t*) { return true; }};

const clap_plugin_webview_t Plugin::webview = {
    [](const clap_plugin_t*, char* uri, uint32_t capacity) -> int32_t {
      static const char start[] = "/index.html";
      if (capacity) std::snprintf(uri, capacity, "%s", start);
      return int32_t(sizeof start);
    },
    [](const clap_plugin_t* p, const char* path, char* mime, uint32_t capacity, const clap_ostream_t* stream) {
      return self(p)->resource(path, mime, capacity, stream);
    },
    [](const clap_plugin_t* p, const void* buffer, uint32_t size) { return self(p)->receive(buffer, size); }};

const char* const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, "org.tytel.helm", "Helm", "Matt Tytel", "https://github.com/amilajack/helm",
    "https://github.com/amilajack/helm", "https://github.com/amilajack/helm", "0.9.0",
    "Helm's original C++ polyphonic synthesizer.", features};

const clap_plugin_factory_t factory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* {
      return index == 0 ? &descriptor : nullptr;
    },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
      if (!host || !id || std::strcmp(id, descriptor.id) || !clap_version_is_compatible(host->clap_version))
        return nullptr;
      return (new Plugin(host, &descriptor))->clap();
    }};
}  // namespace

#ifdef __wasi__
// WCLAP hosts allocate host-side structures in plugin memory through `malloc`.
// The allocation audit wraps the malloc family, so export the names explicitly.
extern "C" __attribute__((export_name("malloc"))) void* helm_wclap_malloc(size_t size) { return std::malloc(size); }
extern "C" __attribute__((export_name("free"))) void helm_wclap_free(void* pointer) { std::free(pointer); }
#endif

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char* path) {
      bundlePath = path ? path : "";
      while (bundlePath.size() > 1 && bundlePath.back() == '/') bundlePath.pop_back();
      return true;
    },
    [] { bundlePath.clear(); },
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory; }};
