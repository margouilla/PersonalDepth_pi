// PersonalDepth_pi - marégraphes en ligne : listes des stations et mesures
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
// wxEVT_DOWNLOAD_EVENT est défini dans OpenCPN : sous Windows, il faut le
// déclarer importé (dllimport), ce que ocpn_plugin.h fait avec MAKING_PLUGIN
#ifndef MAKING_PLUGIN
#define MAKING_PLUGIN
#endif
#include "GaugeManager.h"

#include <wx/filename.h>

#include <ctime>

#include "PersonalDepth_pi.h"
#include "core/FileUtil.h"
#include "core/Lang.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)

using namespace pd;

namespace {
std::string ReadText(const wxString& path) {
  std::string s;
  FILE* f = OpenFile(path.ToStdString(wxConvUTF8), "rb");
  if (!f) return s;
  char b[16384];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}
const int kStationsMaxAge = 30 * 86400;
}  // namespace

GaugeManager::GaugeManager(PersonalDepth_pi* pi) : m_pi(pi) {
  Connect(wxEVT_DOWNLOAD_EVENT, (wxObjectEventFunction)(wxEventFunction)&GaugeManager::OnDownload);
}

GaugeManager::~GaugeManager() { Stop(); }

void GaugeManager::Stop() {
  m_queue.clear();
  if (m_busy) {
    OCPN_cancelDownloadFileBackground(m_handle);
    m_busy = false;
    const wxString part = m_cur.dest + ".part";
    if (wxFileExists(part)) wxRemoveFile(part);
  }
}

wxString GaugeManager::ProviderDir(const GaugeProvider& p) const {
  return m_pi->ExternalDir() + wxFileName::GetPathSeparator() + W8(p.dirName().c_str());
}

wxString GaugeManager::StationDir(const GaugeProvider& p, const GaugeStation& s) const {
  return ProviderDir(p) + wxFileName::GetPathSeparator() + W8(s.folder().c_str());
}

bool GaugeManager::Fresh(const wxString& file, int maxAgeSec) const {
  if (!wxFileExists(file)) return false;
  const wxDateTime m = wxFileName(file).GetModificationTime();
  return m.IsValid() && (wxDateTime::Now() - m).GetSeconds().ToLong() < maxAgeSec;
}

const std::vector<GaugeStation>* GaugeManager::Stations(const GaugeProvider& p, bool background) {
  const wxString cache = ProviderDir(p) + wxFileName::GetPathSeparator() + "stations.json";
  auto it = m_stations.find(p.id());
  if (it == m_stations.end() && wxFileExists(cache)) {
    std::vector<GaugeStation> v;
    std::string err;
    if (p.parseStations(ReadText(cache), v, err)) it = m_stations.emplace(p.id(), std::move(v)).first;
  }
  if (background && !Fresh(cache, kStationsMaxAge)) {
    const long long now = (long long)std::time(nullptr);
    long long& tried = m_stationsTried[p.id()];
    if (now - tried > 600) {  // un essai toutes les 10 min au plus (hors connexion)
      tried = now;
      Job j;
      j.url = W8(p.stationsUrl().c_str());
      j.dest = cache;
      j.provider = &p;
      j.stations = true;
      Enqueue(j);
    }
  }
  return it == m_stations.end() ? nullptr : &it->second;
}

bool GaugeManager::LoadStationsNow(const GaugeProvider& p, bool refresh, wxWindow* parent, wxString& err) {
  const wxString cache = ProviderDir(p) + wxFileName::GetPathSeparator() + "stations.json";
  std::string e;
  std::vector<GaugeStation> v;
  if (!refresh && Fresh(cache, kStationsMaxAge) && p.parseStations(ReadText(cache), v, e)) {
    m_stations[p.id()] = std::move(v);
    return true;
  }
  const wxString tmp = cache + ".new";
  if (m_pi->Download(W8(p.stationsUrl().c_str()), tmp, W8(p.title().c_str()), U8("Liste des marégraphes"), parent,
                     err)) {
    if (p.parseStations(ReadText(tmp), v, e)) {
      if (wxFileExists(cache)) wxRemoveFile(cache);
      wxRenameFile(tmp, cache);
      m_stations[p.id()] = std::move(v);
      ++m_version;
      return true;
    }
    wxRemoveFile(tmp);
    err = W8(e.c_str());
  }
  // hors connexion : ancienne liste
  if (wxFileExists(cache) && p.parseStations(ReadText(cache), v, e)) {
    m_stations[p.id()] = std::move(v);
    return true;
  }
  return false;
}

bool GaugeManager::Ensure(const GaugeProvider& p, const GaugeStation& s, long long t0, long long t1, int refreshSec) {
  const long long now = (long long)std::time(nullptr);
  t1 = std::min(t1, now + 60);
  if (t1 <= t0) return true;
  const wxString dir = StationDir(p, s), sep = wxFileName::GetPathSeparator();
  bool complete = true;
  for (const auto& c : p.chunks(s.id, t0, t1, 1)) {
    const wxString f = dir + sep + W8(c.file.c_str());
    const bool recent = c.t1 + 2 * 86400 > now;  // données brutes encore complétées par le fournisseur
    // tranche du moment : reprise après refreshSec ; jours récents : 30 min
    const int age = (c.t0 <= now && now < c.t1) ? refreshSec : std::max(refreshSec, 1800);
    if (wxFileExists(f) && (!recent || Fresh(f, age))) continue;
    auto e = m_emptyUntil.find(c.url);
    if (e != m_emptyUntil.end() && e->second > now) continue;  // vide au dernier essai
    if (!wxFileExists(f)) complete = false;
    Job j;
    j.url = W8(c.url.c_str());
    j.dest = f;
    j.provider = &p;
    Enqueue(j);
  }
  return complete;
}

void GaugeManager::Enqueue(const Job& j) {
  if (m_busy && m_cur.dest == j.dest) return;
  for (const auto& q : m_queue)
    if (q.dest == j.dest) return;
  m_queue.push_back(j);
  if (!m_busy) Next();
}

void GaugeManager::Next() {
  while (!m_busy && !m_queue.empty()) {
    m_cur = m_queue.front();
    m_queue.pop_front();
    const wxFileName fn(m_cur.dest);
    if (!wxDirExists(fn.GetPath())) wxFileName::Mkdir(fn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    const wxString part = m_cur.dest + ".part";
    if (wxFileExists(part)) wxRemoveFile(part);
    m_handle = 0;
    if (OCPN_downloadFileBackground(m_cur.url, part, this, &m_handle) == OCPN_DL_STARTED) {
      m_busy = true;
      return;
    }
    // échec immédiat (hors connexion) : tranche ou liste notée comme vide un moment
    m_emptyUntil[m_cur.url.ToStdString(wxConvUTF8)] = (long long)std::time(nullptr) + 600;
  }
}

void GaugeManager::OnDownload(OCPN_downloadEvent& ev) {
  if (ev.getDLEventCondition() != OCPN_DL_EVENT_TYPE_END || !m_busy) return;
  m_busy = false;
  const wxString part = m_cur.dest + ".part";
  const bool ok = ev.getDLEventStatus() == OCPN_DL_NO_ERROR && wxFileExists(part);
  const long long now = (long long)std::time(nullptr);
  bool got = false;
  if (ok && m_cur.provider) {
    const std::string text = ReadText(part);
    std::vector<GaugeStation> v;
    std::string err;
    const bool good = m_cur.stations ? m_cur.provider->parseStations(text, v, err) : m_cur.provider->hasData(text);
    if (good) {
      if (!m_cur.stations) {  // entête du fournisseur au besoin (référence, unité)
        const std::string stored = m_cur.provider->store(text);
        if (stored != text) {
          if (FILE* f = OpenFile(part.ToStdString(wxConvUTF8), "wb")) {
            std::fwrite(stored.data(), 1, stored.size(), f);
            std::fclose(f);
          }
        }
      }
      if (wxFileExists(m_cur.dest)) wxRemoveFile(m_cur.dest);
      got = wxRenameFile(part, m_cur.dest);
      if (m_cur.stations && got) m_stations[m_cur.provider->id()] = std::move(v);
    }
  }
  if (wxFileExists(part)) wxRemoveFile(part);
  if (!got) m_emptyUntil[m_cur.url.ToStdString(wxConvUTF8)] = now + (ok ? 1800 : 600);
  else ++m_version;
  Next();
  if (m_onUpdate) m_onUpdate();
}
