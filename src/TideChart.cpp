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

#include "core/Lang.h"
#include "TideChart.h"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

#define U8(s) wxString::FromUTF8(pd::tr(s))

namespace {
const time_t kHalfCycle = 44700;  // 12 h 25 min : un jour lunaire (24 h 50) autour de l'instant
const int kSamples = 288;         // points par courbe

const wxColour kColours[] = {wxColour(31, 119, 180), wxColour(214, 39, 40),  wxColour(44, 160, 44),
                             wxColour(255, 127, 14), wxColour(148, 103, 189), wxColour(140, 86, 75),
                             wxColour(227, 119, 194), wxColour(127, 127, 127), wxColour(188, 189, 34),
                             wxColour(23, 190, 207)};

// pas « rond » : 1, 2, 2,5 ou 5 × 10^n
double NiceStep(double range, int target) {
  if (!(range > 0)) return 1;
  const double raw = range / std::max(1, target), p = std::pow(10.0, std::floor(std::log10(raw)));
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (raw <= m * p) return m * p;
  return 10 * p;
}

time_t MakeTime(wxDatePickerCtrl* d, wxTimePickerCtrl* h) {
  wxDateTime v = d->GetValue();
  const wxDateTime tm = h->GetValue();
  v.SetHour(tm.GetHour());
  v.SetMinute(tm.GetMinute());
  v.SetSecond(0);
  return v.GetTicks();
}
}  // namespace

// --------------------------------------------------------------- tracé
class TideChartPanel : public wxPanel {
 public:
  explicit TideChartPanel(TideChartDialog* dlg)
      : wxPanel(dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE),
        m_dlg(dlg) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(560, 300)));
    Bind(wxEVT_PAINT, &TideChartPanel::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &TideChartPanel::OnClick, this);
  }
  std::function<void()> onToggle;

 private:
  void OnClick(wxMouseEvent& e) {
    for (size_t k = 0; k < m_hit.size() && k < m_dlg->m_series.size(); ++k)
      if (m_hit[k].Contains(e.GetPosition())) {
        m_dlg->m_series[k].visible = !m_dlg->m_series[k].visible;
        if (onToggle) onToggle();
        Refresh();
        return;
      }
  }

  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC pdc(this);
    pdc.SetBackground(*wxWHITE_BRUSH);
    pdc.Clear();
    wxGCDC dc(pdc);
    const TideChartDialog& D = *m_dlg;
    const wxSize sz = GetClientSize();
    dc.SetFont(GetFont());
    const wxColour ink(40, 40, 40), grid(225, 225, 225);
    m_hit.clear();
    if (D.m_series.empty()) {
      dc.SetTextForeground(ink);
      const wxString m = D.m_message.empty() ? U8("Aucun point de marée") : D.m_message;
      const wxSize e = dc.GetTextExtent(m);
      dc.DrawText(m, (sz.x - e.x) / 2, (sz.y - e.y) / 2);
      return;
    }
    const int legendW = FromDIP(230), L = FromDIP(62), T = FromDIP(28), B = FromDIP(46);
    const wxRect plot(L, T, std::max(50, sz.x - L - legendW - FromDIP(16)), std::max(50, sz.y - T - B));
    const double x0 = (double)D.m_x0, x1 = (double)D.m_x1, y0 = D.m_y0, y1 = D.m_y1;
    if (!(x1 > x0) || !(y1 > y0)) return;
    auto X = [&](double t) { return plot.x + (t - x0) / (x1 - x0) * plot.width; };
    auto Y = [&](double h) { return plot.y + plot.height - (h - y0) / (y1 - y0) * plot.height; };

    // grille et graduations — hauteurs
    dc.SetTextForeground(ink);
    const double ys = NiceStep(y1 - y0, 6);
    for (double v = std::ceil(y0 / ys) * ys; v <= y1 + 1e-9; v += ys) {
      const int y = (int)std::lround(Y(v));
      dc.SetPen(wxPen(std::fabs(v) < 1e-9 ? wxColour(160, 160, 160) : grid, 1));
      dc.DrawLine(plot.x, y, plot.GetRight(), y);
      const wxString s = wxString::Format("%g", std::round(v * 1000) / 1000);
      const wxSize e = dc.GetTextExtent(s);
      dc.DrawText(s, plot.x - e.x - 6, y - e.y / 2);
    }
    // temps : pas rond en heures locales
    const double span = x1 - x0;
    double xs = 900;
    for (double c : {900.0, 1800.0, 3600.0, 7200.0, 10800.0, 21600.0, 43200.0, 86400.0, 172800.0})
      if (span / c <= 9) { xs = c; break; } else xs = c;
    const long tz = wxDateTime::TimeZone(wxDateTime::Local).GetOffset();
    for (double t = std::ceil((x0 + tz) / xs) * xs - tz; t <= x1 + 1; t += xs) {
      const int x = (int)std::lround(X(t));
      dc.SetPen(wxPen(grid, 1));
      dc.DrawLine(x, plot.y, x, plot.GetBottom());
      const wxDateTime w((time_t)t);
      const wxString s = w.Format("%H:%M");
      wxSize e = dc.GetTextExtent(s);
      dc.DrawText(s, x - e.x / 2, plot.GetBottom() + 4);
      if (w.GetHour() == 0 && w.GetMinute() == 0) {  // changement de jour
        const wxString d = w.Format("%d/%m");
        e = dc.GetTextExtent(d);
        dc.DrawText(d, x - e.x / 2, plot.GetBottom() + 4 + e.y);
      }
    }
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(plot);
    {  // titres d'axes
      const wxString yl = U8("Hauteur d'eau au-dessus du ZH (") + D.m_unit + ")";
      dc.DrawRotatedText(yl, FromDIP(4), plot.y + plot.height / 2 + dc.GetTextExtent(yl).x / 2, 90);
      const wxString xl = wxDateTime(D.m_x0).Format("%d/%m/%Y %H:%M") + U8("  →  ") +
                          wxDateTime(D.m_x1).Format("%d/%m/%Y %H:%M") + U8("  (heure locale)");
      const wxSize e = dc.GetTextExtent(xl);
      dc.DrawText(xl, plot.x + (plot.width - e.x) / 2, sz.y - e.y - 2);
    }

    // courbes
    dc.SetClippingRegion(plot);
    for (const auto& s : D.m_series) {
      if (!s.visible) continue;
      dc.SetPen(wxPen(s.colour, 2));
      std::vector<wxPoint> run;
      auto flush = [&]() {
        if (run.size() > 1) dc.DrawLines((int)run.size(), run.data());
        run.clear();
      };
      for (size_t i = 0; i < D.m_t.size() && i < s.h.size(); ++i) {
        if (std::isnan(s.h[i])) { flush(); continue; }
        run.emplace_back((int)std::lround(X((double)D.m_t[i])), (int)std::lround(Y(s.h[i])));
      }
      flush();
    }
    // marqueur de l'instant choisi
    std::vector<double> atMarker(D.m_series.size(), NAN);
    const bool mk = D.m_marker >= D.m_x0 && D.m_marker <= D.m_x1 && D.m_t.size() > 1;
    if (mk) {
      const double xm = X((double)D.m_marker);
      dc.SetPen(wxPen(wxColour(200, 0, 0), 1, wxPENSTYLE_SHORT_DASH));
      dc.DrawLine((int)xm, plot.y, (int)xm, plot.GetBottom());
      const double f = (double)(D.m_marker - D.m_t.front()) / (double)(D.m_t.back() - D.m_t.front()) *
                       (double)(D.m_t.size() - 1);
      const size_t i0 = (size_t)std::max(0.0, std::min(std::floor(f), (double)D.m_t.size() - 2));
      const double w = f - (double)i0;
      for (size_t k = 0; k < D.m_series.size(); ++k) {
        const auto& h = D.m_series[k].h;
        if (i0 + 1 >= h.size() || std::isnan(h[i0]) || std::isnan(h[i0 + 1])) continue;
        atMarker[k] = h[i0] * (1 - w) + h[i0 + 1] * w;
        if (!D.m_series[k].visible) continue;
        dc.SetPen(wxPen(D.m_series[k].colour, 2));
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawCircle((int)xm, (int)std::lround(Y(atMarker[k])), FromDIP(4));
      }
    }
    dc.DestroyClippingRegion();
    if (mk) {
      dc.SetTextForeground(wxColour(200, 0, 0));
      const wxString s = wxDateTime(D.m_marker).Format("%d/%m %H:%M");
      const wxSize e = dc.GetTextExtent(s);
      const int xm = (int)X((double)D.m_marker);
      dc.DrawText(s, std::max(plot.x, std::min(plot.GetRight() - e.x, xm - e.x / 2)), plot.y - e.y - 4);
    }

    // légende à cocher
    int lx = plot.GetRight() + FromDIP(16), ly = plot.y;
    dc.SetTextForeground(ink);
    if (!D.m_source.empty()) {
      dc.DrawText(D.m_source, lx, ly);
      ly += dc.GetTextExtent("Ag").y + FromDIP(8);
    }
    const int box = FromDIP(12), lh = std::max(box, dc.GetTextExtent("Ag").y) + FromDIP(6);
    for (size_t k = 0; k < D.m_series.size(); ++k) {
      const auto& s = D.m_series[k];
      dc.SetPen(wxPen(s.colour, 2));
      dc.SetBrush(s.visible ? wxBrush(s.colour) : *wxWHITE_BRUSH);
      dc.DrawRectangle(lx, ly + (lh - box) / 2 - 2, box, box);
      wxString t = s.pt.name;
      if (!std::isnan(atMarker[k])) t += wxString::Format("  %+.2f %s", atMarker[k], D.m_unit);
      dc.SetTextForeground(s.visible ? ink : wxColour(160, 160, 160));
      dc.DrawText(t, lx + box + FromDIP(6), ly);
      const wxString c = wxString::Format(U8("%.4f°, %.4f°"), s.pt.lat, s.pt.lon);
      wxFont fSmall = GetFont();
      fSmall.SetPointSize(std::max(6, fSmall.GetPointSize() - 1));
      dc.SetFont(fSmall);
      dc.SetTextForeground(wxColour(130, 130, 130));
      dc.DrawText(c, lx + box + FromDIP(6), ly + lh - FromDIP(4));
      dc.SetFont(GetFont());
      m_hit.push_back(wxRect(lx, ly, legendW, 2 * lh - FromDIP(4)));
      ly += 2 * lh;
    }
  }

  TideChartDialog* m_dlg;
  std::vector<wxRect> m_hit;
};

// -------------------------------------------------------------- fenêtre
TideChartDialog::TideChartDialog(wxWindow* parent, PersonalDepth_pi* pi)
    : wxDialog(parent, wxID_ANY, U8("PersonalDepth — courbe de marée"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi) {
  auto* col = new wxBoxSizer(wxVERTICAL);
  auto* r = new wxBoxSizer(wxHORIZONTAL);
  m_auto = new wxCheckBox(this, wxID_ANY, U8("Bornes automatiques"));
  m_auto->SetValue(true);
  m_auto->SetToolTip(U8("Un cycle de marée (24 h 50) centré sur l'instant choisi, hauteurs "
                        "englobant les courbes affichées. Modifier une borne repasse en manuel."));
  m_d0 = new wxDatePickerCtrl(this, wxID_ANY);
  m_h0 = new wxTimePickerCtrl(this, wxID_ANY);
  m_d1 = new wxDatePickerCtrl(this, wxID_ANY);
  m_h1 = new wxTimePickerCtrl(this, wxID_ANY);
  m_ymin = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                wxSP_ARROW_KEYS, -100, 100, 0, 0.1);
  m_ymax = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                wxSP_ARROW_KEYS, -100, 100, 1, 0.1);
  m_ymin->SetDigits(2);
  m_ymax->SetDigits(2);
  m_yUnit = new wxStaticText(this, wxID_ANY, "m");
  r->Add(m_auto, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  r->Add(new wxStaticText(this, wxID_ANY, U8("Du")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_d0, 0, wxRIGHT, 2);
  r->Add(m_h0, 0, wxRIGHT, 8);
  r->Add(new wxStaticText(this, wxID_ANY, U8("au")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_d1, 0, wxRIGHT, 2);
  r->Add(m_h1, 0, wxRIGHT, 12);
  r->Add(new wxStaticText(this, wxID_ANY, U8("Hauteur de")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_ymin, 0, wxRIGHT, 4);
  r->Add(new wxStaticText(this, wxID_ANY, U8("à")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_ymax, 0, wxRIGHT, 4);
  r->Add(m_yUnit, 0, wxALIGN_CENTER_VERTICAL);
  col->Add(r, 0, wxALL, 8);
  m_panel = new TideChartPanel(this);
  col->Add(m_panel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  col->Add(new wxStaticText(this, wxID_ANY,
                            U8("Cliquer sur un point de la légende pour l'afficher ou le masquer. "
                               "Trait rouge : instant choisi dans le cadre Marée.")),
           0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
  SetSizerAndFit(col);
  SetSize(FromDIP(wxSize(920, 520)));

  m_auto->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { OnAuto(); });
  for (wxDatePickerCtrl* d : {m_d0, m_d1})
    d->Bind(wxEVT_DATE_CHANGED, [this](wxDateEvent&) { OnBoundsEdited(); });
  for (wxTimePickerCtrl* h : {m_h0, m_h1})
    h->Bind(wxEVT_TIME_CHANGED, [this](wxDateEvent&) { OnBoundsEdited(); });
  for (wxSpinCtrlDouble* s : {m_ymin, m_ymax})
    s->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { OnBoundsEdited(); });
  m_panel->onToggle = [this]() {
    if (m_auto->GetValue()) { AutoY(); FillBoundsControls(); }
  };
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Hide(); });
}

// Nouvel instant ou nouveaux points (appelé à chaque mise à jour de la marée)
void TideChartDialog::TideUpdated() {
  if (!IsShown()) return;
  wxString why;
  const auto pts = m_pi->TideChartPoints(m_source, why);
  m_message = why;
  m_marker = m_pi->TideChartTime();
  const DisplaySettings& st = m_pi->Settings();
  m_unit = st.heightUnit;
  std::string key = std::string(m_source.ToUTF8().data()) + "|" + std::to_string(st.heightK) + "|" +
                    std::to_string(st.tideSource);
  for (const auto& p : pts) {
    char b[96];
    std::snprintf(b, sizeof b, "|%.5f %.5f ", p.lon, p.lat);
    key += b;
    key += p.name.ToUTF8().data();
  }
  const bool changed = key != m_key;
  if (changed) {  // points changés : nouvelles séries, cases cochées gardées par nom
    std::map<wxString, bool> vis;
    for (const auto& s : m_series) vis[s.pt.name] = s.visible;
    m_series.clear();
    for (size_t k = 0; k < pts.size(); ++k) {
      Series s;
      s.pt = pts[k];
      s.colour = kColours[k % (sizeof kColours / sizeof kColours[0])];
      auto it = vis.find(pts[k].name);
      s.visible = it == vis.end() ? true : it->second;
      m_series.push_back(s);
    }
    m_key = key;
  }
  const time_t span = m_x1 - m_x0;
  const bool offCentre = m_marker < m_x0 + span / 4 || m_marker > m_x1 - span / 4;
  if (m_auto->GetValue() && (changed || offCentre || span <= 0)) {
    AutoX();
    Recompute(true);
    AutoY();
  } else if (changed) {
    if (span <= 0) AutoX();
    Recompute(true);
    if (m_auto->GetValue()) AutoY();
  }
  m_yUnit->SetLabel(m_unit);
  FillBoundsControls();
  m_panel->Refresh();
}

void TideChartDialog::AutoX() {
  m_x0 = m_marker - kHalfCycle;
  m_x1 = m_marker + kHalfCycle;
}

void TideChartDialog::AutoY() {
  double lo = 1e30, hi = -1e30;
  for (const auto& s : m_series) {
    if (!s.visible) continue;
    for (double v : s.h)
      if (!std::isnan(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
  }
  if (lo > hi) { m_y0 = 0; m_y1 = 1; return; }
  const double pad = std::max(0.05, 0.08 * (hi - lo)), st = NiceStep(hi - lo + 2 * pad, 6);
  m_y0 = std::floor((lo - pad) / st) * st;
  m_y1 = std::ceil((hi + pad) / st) * st;
}

void TideChartDialog::Recompute(bool) {
  m_t.clear();
  if (!(m_x1 > m_x0)) return;
  const double dt = std::max(60.0, (double)(m_x1 - m_x0) / kSamples);
  for (double t = (double)m_x0; t <= (double)m_x1 + 1; t += dt) m_t.push_back((time_t)t);
  std::vector<TideChartPoint> pts;
  for (const auto& s : m_series) pts.push_back(s.pt);
  std::vector<std::vector<double>> h;
  m_pi->TideChartSeries(pts, m_t, h);
  const double k = m_pi->Settings().heightK;
  for (size_t i = 0; i < m_series.size() && i < h.size(); ++i) {
    m_series[i].h = h[i];
    for (double& v : m_series[i].h) v *= k;  // unité de hauteur d'OpenCPN
  }
}

void TideChartDialog::FillBoundsControls() {
  m_updating = true;
  if (m_x1 > m_x0) {
    m_d0->SetValue(wxDateTime(m_x0));
    m_h0->SetValue(wxDateTime(m_x0));
    m_d1->SetValue(wxDateTime(m_x1));
    m_h1->SetValue(wxDateTime(m_x1));
  }
  m_ymin->SetRange(-1000, 1000);
  m_ymax->SetRange(-1000, 1000);
  m_ymin->SetValue(m_y0);
  m_ymax->SetValue(m_y1);
  m_updating = false;
}

// Une borne modifiée : passage en bornes manuelles
void TideChartDialog::OnBoundsEdited() {
  if (m_updating) return;
  m_auto->SetValue(false);
  const time_t a = MakeTime(m_d0, m_h0), b = MakeTime(m_d1, m_h1);
  const double y0 = m_ymin->GetValue(), y1 = m_ymax->GetValue();
  if (b > a && (a != m_x0 || b != m_x1)) {
    m_x0 = a;
    m_x1 = b;
    Recompute(true);
  }
  if (y1 > y0) { m_y0 = y0; m_y1 = y1; }
  m_panel->Refresh();
}

void TideChartDialog::OnAuto() {
  if (!m_auto->GetValue()) return;
  AutoX();
  Recompute(true);
  AutoY();
  FillBoundsControls();
  m_panel->Refresh();
}
