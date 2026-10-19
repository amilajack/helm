// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal CLAP host that loads Helm.clap and checks the parameters, sample-accurate
// note timing, state round trips and the webview message channel.
// Usage: HelmClapTests <binary> <bundle-or-binary path> legacy-parameters.txt
#include <clap/clap.h>
#include <clap/ext/draft/webview.h>
#include <dlfcn.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> messages;
bool webviewSend(const clap_host_t*, const void* buffer, uint32_t size) {
  messages.emplace_back(static_cast<const char*>(buffer), size);
  return true;
}
const clap_host_webview_t hostWebview{webviewSend};
const clap_host_t host{CLAP_VERSION_INIT, nullptr, "Helm tests", "Helm", "", "1",
  [](const clap_host_t*, const char* id) -> const void* {
    return std::strcmp(id, CLAP_EXT_WEBVIEW) ? nullptr : &hostWebview;
  },
  [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}};

struct Events {
  std::vector<clap_event_note_t> notes;
  std::vector<clap_event_param_value_t> params;
  std::vector<const clap_event_header_t*> list;
  clap_input_events_t in{this, [](const clap_input_events_t* e) {
    return uint32_t(static_cast<const Events*>(e->ctx)->list.size()); },
    [](const clap_input_events_t* e, uint32_t i) { return static_cast<const Events*>(e->ctx)->list[i]; }};
};
clap_output_events_t discard{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};

struct Buffer {
  std::string data;
  size_t at = 0;
  clap_ostream_t out{this, [](const clap_ostream_t* s, const void* b, uint64_t n) -> int64_t {
    static_cast<Buffer*>(s->ctx)->data.append(static_cast<const char*>(b), size_t(n)); return int64_t(n); }};
  clap_istream_t in{this, [](const clap_istream_t* s, void* b, uint64_t n) -> int64_t {
    auto* self = static_cast<Buffer*>(s->ctx);
    const size_t count = std::min<size_t>(size_t(n), self->data.size() - self->at);
    std::memcpy(b, self->data.data() + self->at, count); self->at += count; return int64_t(count); }};
};

uint32_t fnv1a(const std::string& text) {
  uint32_t hash = 0x811c9dc5u;
  for (const unsigned char c : text) hash = (hash ^ c) * 0x01000193u;
  return hash;
}

template <class T> const T* extension(const clap_plugin_t* plugin, const char* id) {
  auto result = static_cast<const T*>(plugin->get_extension(plugin, id));
  require(result != nullptr, std::string("Missing extension ") + id);
  return result;
}

std::string send(const clap_plugin_t* plugin, const std::string& message) {
  messages.clear();
  require(extension<clap_plugin_webview_t>(plugin, CLAP_EXT_WEBVIEW)->receive(plugin, message.data(), uint32_t(message.size())),
          "Webview message rejected: " + message);
  return messages.empty() ? "" : messages.back();
}

// Renders `frames` in 512-frame blocks with events at absolute frames; returns left channel.
std::vector<float> render(const clap_plugin_t* plugin, uint32_t frames, std::vector<std::pair<uint32_t, clap_event_note_t>> notes) {
  std::vector<float> left(frames), right(frames);
  for (uint32_t start = 0; start < frames; start += 512) {
    const uint32_t count = std::min<uint32_t>(512, frames - start);
    Events events;
    for (auto& [frame, note] : notes)
      if (frame >= start && frame < start + count) {
        events.notes.push_back(note);
        events.notes.back().header.time = frame - start;
      }
    for (auto& note : events.notes) events.list.push_back(&note.header);
    float* channels[2] = {left.data() + start, right.data() + start};
    clap_audio_buffer_t output{channels, nullptr, 2, 0, 0};
    clap_process_t process{};
    process.steady_time = start;
    process.frames_count = count;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.in_events = &events.in;
    process.out_events = &discard;
    require(plugin->process(plugin, &process) == CLAP_PROCESS_CONTINUE, "process failed");
  }
  return left;
}

clap_event_note_t note(uint16_t type, int key) {
  clap_event_note_t event{};
  event.header = {sizeof event, 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
  event.note_id = -1;
  event.port_index = 0;
  event.channel = 0;
  event.key = int16_t(key);
  event.velocity = 0.8;
  return event;
}

double energy(const std::vector<float>& samples, size_t from, size_t to) {
  double sum = 0;
  for (size_t i = from; i < to; ++i) sum += double(samples[i]) * samples[i];
  return sum;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 4, "Usage: HelmClapTests <binary> <bundle> legacy-parameters.txt");
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(library != nullptr, std::string("dlopen failed: ") + (dlerror() ?: ""));
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
    require(entry && clap_version_is_compatible(entry->clap_version), "Missing clap_entry");
    require(entry->init(argv[2]), "clap_entry.init failed");
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    require(factory && factory->get_plugin_count(factory) == 1, "Expected one plugin");
    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    require(!std::strcmp(descriptor->id, "org.tytel.helm"), "Unexpected plugin id");
    require(!factory->create_plugin(factory, &host, "org.example.other"), "Created an unknown plugin");

    auto create = [&] {
      const clap_plugin_t* plugin = factory->create_plugin(factory, &host, descriptor->id);
      require(plugin && plugin->init(plugin), "Plugin init failed");
      require(plugin->activate(plugin, 48000, 1, 512) && plugin->start_processing(plugin), "Activation failed");
      return plugin;
    };
    const clap_plugin_t* a = create();

    // Parameters: the 139 legacy ids, with stable FNV-1a CLAP ids.
    std::ifstream legacyFile(argv[3]);
    std::vector<std::string> legacy;
    for (std::string line; std::getline(legacyFile, line);) if (!line.empty()) legacy.push_back(line);
    const auto metadata = send(a, R"({"type":"metadata","rid":1})");
    require(metadata.find(R"("type":"metadata","rid":1)") != std::string::npos, "Bad metadata reply");
    auto* params = extension<clap_plugin_params_t>(a, CLAP_EXT_PARAMS);
    require(params->count(a) == legacy.size(), "Expected " + std::to_string(legacy.size()) + " parameters");
    for (const auto& id : legacy) {
      require(metadata.find("{\"id\":\"" + id + "\"") != std::string::npos, "Missing parameter " + id);
      double value;
      require(params->get_value(a, fnv1a(id), &value), "No CLAP id for " + id);
    }
    const clap_id cutoff = fnv1a("cutoff");
    char text[256];
    double parsed;
    require(params->value_to_text(a, cutoff, 70, text, sizeof text) && params->text_to_value(a, cutoff, text, &parsed) &&
            std::abs(parsed - 70) < 1e-3, std::string("cutoff text round trip: ") + text);

    // Sample-accurate notes: silence until the note at frame 4800.
    const auto audio = render(a, 48000, {{4800, note(CLAP_EVENT_NOTE_ON, 60)}, {24000, note(CLAP_EVENT_NOTE_OFF, 60)}});
    require(energy(audio, 0, 4800) == 0, "Output before the first note");
    require(energy(audio, 4800, 24000) > 1e-3, "No output after note on");

    // State: a parameter edit and a modulation route survive save/load into a new instance.
    require(send(a, R"({"type":"param","id":"cutoff","value":55})").empty(), "param should not reply");
    render(a, 512, {});
    auto state = send(a, R"({"type":"getState","rid":2})");
    const auto start = state.find("\"state\":");
    require(start != std::string::npos, "Missing state");
    std::string body = state.substr(start + 8, state.size() - start - 9);
    const std::string route = R"("modulations":[{"source":"mono_lfo_1","destination":"cutoff","amount":12}])";
    const auto modulations = body.find(R"("modulations":[])");
    require(modulations != std::string::npos, "Expected no routes");
    body.replace(modulations, std::strlen(R"("modulations":[])"), route);
    const auto applied = send(a, R"({"type":"setState","rid":3,"state":)" + body + "}");
    require(applied.find(R"("type":"state","rid":3)") != std::string::npos, "setState failed: " + applied);
    require(send(a, R"({"type":"setState","rid":4,"state":{"schemaVersion":2}})").find(R"("type":"error")") != std::string::npos,
            "Invalid state accepted");
    Buffer saved;
    require(extension<clap_plugin_state_t>(a, CLAP_EXT_STATE)->save(a, &saved.out), "save failed");
    const clap_plugin_t* b = create();
    require(extension<clap_plugin_state_t>(b, CLAP_EXT_STATE)->load(b, &saved.in), "load failed");
    double restored = 0;
    require(params->get_value(b, cutoff, &restored) && restored == 55, "cutoff not restored");
    require(send(b, R"({"type":"getState","rid":5})").find(route) != std::string::npos, "route not restored");
    Buffer broken;
    broken.data = "{\"schemaVersion\":1";
    require(!extension<clap_plugin_state_t>(b, CLAP_EXT_STATE)->load(b, &broken.in), "Truncated state accepted");

    // Webview resources: embedded GUI, hash-addressed library only.
    auto* webview = extension<clap_plugin_webview_t>(a, CLAP_EXT_WEBVIEW);
    char uri[64], mime[64];
    require(webview->get_uri(a, uri, sizeof uri) > 0 && !std::strcmp(uri, "/index.html"), "Bad webview URI");
    Buffer page;
    require(webview->get_resource(a, "/index.html", mime, sizeof mime, &page.out) &&
            page.data.find("gui.js") != std::string::npos, "Missing GUI page");
    for (const char* path : {"/../COPYING", "/library/../module.wasm", "/library/objects/zz", "library/manifest.json"}) {
      Buffer ignored;
      require(!webview->get_resource(a, path, mime, sizeof mime, &ignored.out), std::string("Served ") + path);
    }
#ifdef __APPLE__
    Buffer manifest;
    require(webview->get_resource(a, "/library/manifest.json", mime, sizeof mime, &manifest.out) &&
            manifest.data.find("\"patches\"") != std::string::npos, "Missing factory library");
#endif

    for (auto* plugin : {a, b}) {
      plugin->stop_processing(plugin);
      plugin->deactivate(plugin);
      plugin->destroy(plugin);
    }
    entry->deinit();
    std::cout << "CLAP host checks passed for " << legacy.size() << " parameters\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
