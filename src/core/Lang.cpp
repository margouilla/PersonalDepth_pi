// PersonalDepth_pi - personal bathymetry plugin for OpenCPN
// Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Lang.h"

#include <atomic>
#include <string>
#include <unordered_map>

namespace pd {
namespace {
std::atomic<bool> g_english{false};

const std::unordered_map<std::string, const char*>& Table() {
  static const std::unordered_map<std::string, const char*> t = {
#include "LangTable.inc"
  };
  return t;
}
}  // namespace

void SetEnglish(bool english) { g_english = english; }
bool IsEnglish() { return g_english; }

const char* tr(const char* fr) {
  if (!fr || !g_english) return fr;
  const auto& t = Table();
  auto it = t.find(fr);
  return it == t.end() ? fr : it->second;
}
}  // namespace pd
