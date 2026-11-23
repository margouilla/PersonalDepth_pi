// PersonalDepth_pi - lecture JSON minimale
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Json.h"

#include "PointCloud.h"  // ParseFieldNumber : indépendant de la langue du système

#include <cstdlib>
#include <cstring>

namespace pd {

static const Json kNull;

double Json::num(double def) const {
  if (t_ == Number) return n_;
  if (t_ == String && !s_.empty()) {
    double v;
    if (ParseFieldNumber(s_, '.', v)) return v;
  }
  return def;
}

std::string Json::str(const std::string& def) const {
  if (t_ == String) return s_;
  if (t_ == Number) {
    char b[32];
    if (n_ == (double)(long long)n_) std::snprintf(b, sizeof b, "%lld", (long long)n_);
    else std::snprintf(b, sizeof b, "%.15g", n_);
    return b;
  }
  return def;
}

const Json& Json::operator[](size_t i) const { return (t_ == Array && i < a_.size()) ? a_[i] : kNull; }

const Json& Json::operator[](const std::string& k) const {
  if (t_ != Object) return kNull;
  auto it = o_.find(k);
  return it == o_.end() ? kNull : it->second;
}

class JsonParser {
 public:
  JsonParser(const std::string& s) : p_(s.data()), b_(s.data()), e_(s.data() + s.size()) {}

  bool parse(Json& out, std::string& err) {
    if (e_ - p_ >= 3 && (unsigned char)p_[0] == 0xEF && (unsigned char)p_[1] == 0xBB && (unsigned char)p_[2] == 0xBF)
      p_ += 3;  // BOM
    if (!value(out, 0)) { err = err_ + " (" + std::to_string(p_ - b_) + ")"; return false; }
    ws();
    if (p_ != e_) { err = "JSON : caractères après la fin (" + std::to_string(p_ - b_) + ")"; return false; }
    return true;
  }

 private:
  void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
  bool fail(const char* m) { err_ = m; return false; }

  static void put(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
      s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63));
    } else {
      s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 63));
      s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63));
    }
  }

  bool hex4(unsigned& v) {
    if (e_ - p_ < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool string(std::string& s) {
    ++p_;  // "
    while (p_ < e_ && *p_ != '"') {
      if (*p_ != '\\') { s += *p_++; continue; }
      if (++p_ >= e_) break;
      const char c = *p_++;
      switch (c) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          unsigned v;
          if (!hex4(v)) return fail("JSON : \\u invalide");
          if (v >= 0xD800 && v < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            unsigned lo;
            if (!hex4(lo)) return fail("JSON : \\u invalide");
            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          }
          put(s, v);
          break;
        }
        default: return fail("JSON : échappement invalide");
      }
    }
    if (p_ >= e_) return fail("JSON : chaîne non terminée");
    ++p_;
    return true;
  }

  bool value(Json& v, int depth) {
    if (depth > 64) return fail("JSON : imbrication trop profonde");
    ws();
    if (p_ >= e_) return fail("JSON : fin inattendue");
    const char c = *p_;
    if (c == '{') {
      v.t_ = Json::Object;
      ++p_;
      ws();
      if (p_ < e_ && *p_ == '}') { ++p_; return true; }
      for (;;) {
        ws();
        if (p_ >= e_ || *p_ != '"') return fail("JSON : nom de membre attendu");
        std::string k;
        if (!string(k)) return false;
        ws();
        if (p_ >= e_ || *p_ != ':') return fail("JSON : « : » attendu");
        ++p_;
        Json m;
        if (!value(m, depth + 1)) return false;
        v.o_[k] = std::move(m);
        ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == '}') { ++p_; return true; }
        return fail("JSON : « , » ou « } » attendu");
      }
    }
    if (c == '[') {
      v.t_ = Json::Array;
      ++p_;
      ws();
      if (p_ < e_ && *p_ == ']') { ++p_; return true; }
      for (;;) {
        Json m;
        if (!value(m, depth + 1)) return false;
        v.a_.push_back(std::move(m));
        ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == ']') { ++p_; return true; }
        return fail("JSON : « , » ou « ] » attendu");
      }
    }
    if (c == '"') { v.t_ = Json::String; return string(v.s_); }
    auto word = [&](const char* w) {
      const size_t n = std::strlen(w);
      if ((size_t)(e_ - p_) >= n && std::strncmp(p_, w, n) == 0) { p_ += n; return true; }
      return false;
    };
    if (word("true")) { v.t_ = Json::Bool; v.b_ = true; return true; }
    if (word("false")) { v.t_ = Json::Bool; v.b_ = false; return true; }
    if (word("null")) { v.t_ = Json::Null; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const char* s = p_;
      while (p_ < e_ && (std::strchr("+-.eE", *p_) || (*p_ >= '0' && *p_ <= '9'))) ++p_;
      const std::string num(s, p_);
      if (!ParseFieldNumber(num, '.', v.n_)) return fail("JSON : nombre invalide");
      v.t_ = Json::Number;
      return true;
    }
    return fail("JSON : valeur invalide");
  }

  const char* p_;
  const char* b_;
  const char* e_;
  std::string err_;
};

bool Json::Parse(const std::string& text, Json& out, std::string& err) {
  out = Json();
  JsonParser p(text);
  return p.parse(out, err);
}

}  // namespace pd
