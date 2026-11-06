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

#include "FileUtil.h"

#include <sys/stat.h>
#include <system_error>

namespace pd {
FILE* OpenFile(const std::string& utf8, const char* mode) {
#ifdef _WIN32
  const std::wstring wm(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(U8Path(utf8).wstring().c_str(), wm.c_str());
#else
  return std::fopen(utf8.c_str(), mode);
#endif
}

long long FileMTime(const std::string& utf8) {
#ifdef _WIN32
  struct _stat64 st;
  if (_wstat64(U8Path(utf8).wstring().c_str(), &st) != 0) return 0;
#else
  struct stat st;
  if (stat(utf8.c_str(), &st) != 0) return 0;
#endif
  return (long long)st.st_mtime;
}

void RemoveFile(const std::string& utf8) {
  std::error_code ec;
  std::filesystem::remove(U8Path(utf8), ec);
}
}  // namespace pd
