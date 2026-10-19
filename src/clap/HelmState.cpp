// SPDX-License-Identifier: GPL-3.0-or-later
#include "HelmState.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace helm {
namespace {
constexpr size_t maxText = 4 << 20, maxDepth = 32;
const char* const infoKeys[] = {"patch_name", "author", "folder_name", "license"};

struct Parser {
  const std::string& text;
  size_t at = 0;
  std::string error;
  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
  }
  bool fail(const char* message) {
    if (error.empty()) error = message;
    return false;
  }
  bool literal(const char* word) {
    const std::string w = word;
    if (text.compare(at, w.size(), w) != 0) return fail("Invalid JSON literal");
    at += w.size();
    return true;
  }
  static void utf8(std::string& out, unsigned code) {
    if (code < 0x80) out += char(code);
    else if (code < 0x800) { out += char(0xc0 | code >> 6); out += char(0x80 | (code & 63)); }
    else if (code < 0x10000) { out += char(0xe0 | code >> 12); out += char(0x80 | (code >> 6 & 63)); out += char(0x80 | (code & 63)); }
    else { out += char(0xf0 | code >> 18); out += char(0x80 | (code >> 12 & 63)); out += char(0x80 | (code >> 6 & 63)); out += char(0x80 | (code & 63)); }
  }
  bool hex4(unsigned& code) {
    if (at + 4 > text.size()) return fail("Invalid JSON escape");
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text[at++];
      code <<= 4;
      if (c >= '0' && c <= '9') code |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') code |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= unsigned(c - 'A' + 10);
      else return fail("Invalid JSON escape");
    }
    return true;
  }
  bool string(std::string& out) {
    if (at >= text.size() || text[at] != '"') return fail("Expected a JSON string");
    ++at;
    while (at < text.size()) {
      const char c = text[at++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("Control character in JSON string");
      if (c != '\\') { out += c; continue; }
      if (at >= text.size()) break;
      const char e = text[at++];
      switch (e) {
        case '"': case '\\': case '/': out += e; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned code;
          if (!hex4(code)) return false;
          if (code >= 0xd800 && code < 0xdc00) {
            unsigned low;
            if (text.compare(at, 2, "\\u") != 0) return fail("Unpaired JSON surrogate");
            at += 2;
            if (!hex4(low) || low < 0xdc00 || low >= 0xe000) return fail("Unpaired JSON surrogate");
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
          } else if (code >= 0xdc00 && code < 0xe000) return fail("Unpaired JSON surrogate");
          utf8(out, code);
          break;
        }
        default: return fail("Invalid JSON escape");
      }
    }
    return fail("Unterminated JSON string");
  }
  bool number(double& out) {
    const size_t start = at;
    if (at < text.size() && text[at] == '-') ++at;
    while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == '.' ||
                                text[at] == 'e' || text[at] == 'E' || text[at] == '+' || text[at] == '-'))
      ++at;
    const std::string token = text.substr(start, at - start);
    if (token.empty() || token == "-") return fail("Invalid JSON number");
    char* end = nullptr;
    out = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(out)) return fail("Invalid JSON number");
    return true;
  }
  bool value(Json& out, size_t depth) {
    if (depth > maxDepth) return fail("JSON nesting too deep");
    space();
    if (at >= text.size()) return fail("Unexpected end of JSON");
    const char c = text[at];
    if (c == '{') {
      out.type = Json::Object;
      ++at;
      space();
      if (at < text.size() && text[at] == '}') { ++at; return true; }
      while (true) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (at >= text.size() || text[at++] != ':') return fail("Expected ':' in JSON object");
        Json item;
        if (!value(item, depth + 1)) return false;
        for (const auto& member : out.members)
          if (member.first == key) return fail("Duplicate JSON key");
        out.members.emplace_back(std::move(key), std::move(item));
        space();
        if (at < text.size() && text[at] == ',') { ++at; continue; }
        if (at < text.size() && text[at] == '}') { ++at; return true; }
        return fail("Expected ',' or '}' in JSON object");
      }
    }
    if (c == '[') {
      out.type = Json::Array;
      ++at;
      space();
      if (at < text.size() && text[at] == ']') { ++at; return true; }
      while (true) {
        Json item;
        if (!value(item, depth + 1)) return false;
        out.items.push_back(std::move(item));
        space();
        if (at < text.size() && text[at] == ',') { ++at; continue; }
        if (at < text.size() && text[at] == ']') { ++at; return true; }
        return fail("Expected ',' or ']' in JSON array");
      }
    }
    if (c == '"') { out.type = Json::String; return string(out.string); }
    if (c == 't') { out.type = Json::Bool; out.boolean = true; return literal("true"); }
    if (c == 'f') { out.type = Json::Bool; return literal("false"); }
    if (c == 'n') { out.type = Json::Null; return literal("null"); }
    out.type = Json::Number;
    return number(out.number);
  }
};
}  // namespace

const Json* Json::find(const std::string& key) const {
  if (type != Object) return nullptr;
  for (const auto& member : members)
    if (member.first == key) return &member.second;
  return nullptr;
}

bool Json::parse(const std::string& text, Json& out, std::string& error) {
  if (text.size() > maxText) { error = "JSON exceeds 4 MiB"; return false; }
  Parser parser{text};
  out = Json{};
  if (!parser.value(out, 0)) { error = parser.error; return false; }
  parser.space();
  if (parser.at != text.size()) { error = "Trailing data after JSON"; return false; }
  return true;
}

std::string jsonQuote(const std::string& value) {
  std::string result = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') { result += '\\'; result += c; }
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned(c));
      result += escaped;
    } else result += c;
  }
  return result + '"';
}

std::string jsonNumber(double value) {
  if (!std::isfinite(value)) return "0";
  char text[32];
  std::snprintf(text, sizeof text, "%.17g", value);
  return text;
}

State defaultState(const Instrument& instrument) {
  State state;
  for (const auto& info : instrument.parameters()) state.values.push_back(info.defaultValue);
  state.info = {{"patch_name", "init"}, {"author", ""}, {"folder_name", ""}, {"license", ""}};
  return state;
}

// Same rules as `state()` in web/wam/state.ts for schema v1.
bool stateFromJson(const Json& input, const Instrument& instrument, State& out, std::string& error) {
  const auto bad = [&](const std::string& message) { error = message; return false; };
  if (input.type != Json::Object) return bad("Invalid Helm state");
  if (input.find("settings") || input.find("synth_version"))
    return bad("Legacy .helm patches must be imported through the Helm GUI");
  const Json* version = input.find("schemaVersion");
  if (!version || version->type != Json::Number || version->number != 1) return bad("Unsupported Helm state");
  const Json* info = input.find("info");
  const Json* values = input.find("parameterValues");
  const Json* modulations = input.find("modulations");
  if (!info || info->type != Json::Object) return bad("Unsupported Helm state");
  if (!values || values->type != Json::Object) return bad("Expected a parameter map");
  State state;
  const auto& parameters = instrument.parameters();
  state.values.assign(parameters.size(), 0);
  std::vector<bool> seen(parameters.size());
  for (const auto& member : values->members) {
    const int index = instrument.indexOf(member.first);
    const Json& entry = member.second;
    if (index < 0 || entry.type != Json::Object) return bad("Invalid parameter: " + member.first);
    const Json* id = entry.find("id");
    const Json* value = entry.find("value");
    const Json* normalized = entry.find("normalized");
    if (!id || id->type != Json::String || id->string != member.first || !value || value->type != Json::Number)
      return bad("Invalid parameter: " + member.first);
    if (normalized && normalized->type != Json::Bool) return bad("Invalid normalization flag");
    const auto& range = parameters[size_t(index)];
    const double v = normalized && normalized->boolean
        ? range.minimum + value->number * (range.maximum - range.minimum) : value->number;
    if (v < range.minimum || v > range.maximum) return bad("Parameter outside range: " + member.first);
    state.values[size_t(index)] = v;
    seen[size_t(index)] = true;
  }
  for (const bool present : seen)
    if (!present) return bad("Incomplete Helm state");
  if (!modulations || modulations->type != Json::Array || modulations->items.size() > 256)
    return bad("Invalid modulation list");
  std::set<std::string> routeKeys;
  for (const auto& route : modulations->items) {
    const Json* source = route.find("source");
    const Json* destination = route.find("destination");
    const Json* amount = route.find("amount");
    if (!source || source->type != Json::String || !destination || destination->type != Json::String ||
        !amount || amount->type != Json::Number || std::abs(amount->number) > 1e6)
      return bad("Invalid modulation route");
    bool allowed = false;
    for (const auto& candidate : instrument.modulationSources())
      if (candidate.id == source->string)
        for (const auto& d : candidate.destinations) allowed = allowed || d == destination->string;
    if (!allowed) return bad("Invalid modulation route");
    if (!routeKeys.insert(source->string + '\0' + destination->string).second)
      return bad("Duplicate modulation route");
    state.routes.push_back({source->string, destination->string, amount->number});
  }
  for (const char* key : infoKeys) {
    const Json* value = info->find(key);
    if (value && (value->type != Json::String || value->string.size() > 4096))
      return bad("Invalid patch information");
    state.info[key] = value ? value->string : "";
  }
  out = std::move(state);
  return true;
}

bool parseState(const std::string& text, const Instrument& instrument, State& out, std::string& error) {
  Json json;
  return Json::parse(text, json, error) && stateFromJson(json, instrument, out, error);
}

std::string stateToJson(const State& state, const Instrument& instrument) {
  std::string json = "{\"schemaVersion\":1,\"parameterValues\":{";
  const auto& parameters = instrument.parameters();
  for (size_t i = 0; i < parameters.size(); ++i) {
    if (i) json += ',';
    json += jsonQuote(parameters[i].id) + ":{\"id\":" + jsonQuote(parameters[i].id) +
            ",\"value\":" + jsonNumber(state.values[i]) + ",\"normalized\":false}";
  }
  json += "},\"modulations\":[";
  for (size_t i = 0; i < state.routes.size(); ++i) {
    const auto& route = state.routes[i];
    if (i) json += ',';
    json += "{\"source\":" + jsonQuote(route.source) + ",\"destination\":" + jsonQuote(route.destination) +
            ",\"amount\":" + jsonNumber(route.amount) + "}";
  }
  json += "],\"info\":{";
  bool first = true;
  for (const char* key : infoKeys) {
    if (!first) json += ',';
    first = false;
    const auto found = state.info.find(key);
    json += jsonQuote(key) + ":" + jsonQuote(found == state.info.end() ? "" : found->second);
  }
  return json + "}}";
}
}  // namespace helm
