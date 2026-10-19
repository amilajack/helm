// SPDX-License-Identifier: GPL-3.0-or-later
// Plugin state in the WAM `State` schema v1 (web/wam/types.ts), so a saved state
// moves between the WAM and CLAP/WCLAP targets unchanged.
#pragma once
#include "HelmInstrument.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace helm {
// Minimal JSON document, enough for state and GUI messages. Parsing is strict and
// bounded (size, depth); it never throws.
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;
  const Json* find(const std::string& key) const;
  static bool parse(const std::string& text, Json& out, std::string& error);
};
std::string jsonQuote(const std::string& value);
std::string jsonNumber(double value);

struct Route {
  std::string source, destination;
  double amount;
};

struct State {
  std::vector<double> values;  // by Instrument parameter index
  std::vector<Route> routes;
  std::map<std::string, std::string> info;  // patch_name, author, folder_name, license
};

State defaultState(const Instrument& instrument);
bool stateFromJson(const Json& input, const Instrument& instrument, State& out, std::string& error);
bool parseState(const std::string& text, const Instrument& instrument, State& out, std::string& error);
std::string stateToJson(const State& state, const Instrument& instrument);
}  // namespace helm
