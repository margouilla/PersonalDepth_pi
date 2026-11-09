// PersonalDepth_pi - courbe des hauteurs d'eau observées
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "WaterLevelDialog.h"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <algorithm>
#include <cmath>

#include "core/Lang.h"
#include "core/WaterLevel.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)

namespace {
double NiceStep(double range, int target) {
  if (!(range > 0)) return 1;
  const double raw = range / std::max(1, target), p = std::pow(10.0, std::floor(std::log10(raw)));
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (raw <= m * p) return m * p;
  return 10 * p;
}

// Pas de temps « rond » pour les graduations (secondes)
long long NiceTimeStep(long long range, int target) {
  static const long long steps[] = {60,     300,    600,     1800,    3600,    3 * 3600, 6 * 3600,
                                    12 * 3600, 86400, 2 * 86400, 7 * 86400, 14 * 86400, 30 * 86400,
                                    91 * 86400, 182 * 86400, 365 * 86400};
  for (long long s : steps)
    if (range / s <= target) return s;
  return 365 * 86400 * ((range / (365 * 86400)) / target + 1);
}

class WaterLevelPanel : public wxPanel {
 public:
  WaterLevelPanel(wxWindow* p, std::shared_ptr<const pd::WaterLevelSeries> s, double k, const wxString& unit)
      : wxPanel(p, wxID_ANY, wxDefaultPosition, p->FromDIP(wxSize(720, 340))), m_s(std::move(s)), m_k(k), m_unit(unit) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &WaterLevelPanel::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { m_mouse = e.GetPosition(); Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_mouse = wxPoint(-1, -1); Refresh(); });
    // molette : zoom sur le temps autour du curseur ; double-clic : tout voir
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
      if (!m_plot.width) return;
      const double f = e.GetWheelRotation() > 0 ? 0.8 : 1.25;
      const double tc = m_x0 + (double)(e.GetX() - m_plot.x) / m_plot.width * (m_x1 - m_x0);
      double a = tc - (tc - m_x0) * f, b = tc + (m_x1 - tc) * f;
      const double lo = (double)m_s->tMin(), hi = (double)m_s->tMax();
      if (b - a < 600) return;
      if (b - a >= hi - lo) { a = lo; b = hi; }
      if (a < lo) { b += lo - a; a = lo; }
      if (b > hi) { a -= b - hi; b = hi; }
      m_x0 = a; m_x1 = b;
      Refresh();
    });
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { Reset(); Refresh(); });
    Reset();
  }

 private:
  void Reset() {
    m_x0 = (double)m_s->tMin();
    m_x1 = (double)m_s->tMax();
    if (m_x1 <= m_x0) { m_x0 -= 3600; m_x1 += 3600; }
  }

  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC pdc(this);
    wxGCDC dc(pdc);
    const wxSize sz = GetClientSize();
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetFont(GetFont());
    const int lh = dc.GetTextExtent("0").y;
    const auto& T = m_s->times();
    const auto& H = m_s->heights();
    // plage verticale sur la partie visible
    double y0 = 1e30, y1 = -1e30;
    for (size_t i = 0; i < T.size(); ++i)
      if (T[i] >= m_x0 - 1 && T[i] <= m_x1 + 1) { y0 = std::min(y0, H[i] / m_k); y1 = std::max(y1, H[i] / m_k); }
    if (y0 > y1) { y0 = m_s->hMin() / m_k; y1 = m_s->hMax() / m_k; }
    if (y1 - y0 < 0.02) { y0 -= 0.05; y1 += 0.05; }
    const double ys = NiceStep(y1 - y0, 6);
    y0 = std::floor(y0 / ys) * ys;
    y1 = std::ceil(y1 / ys) * ys;
    const int left = dc.GetTextExtent("-0000.00").x + 10, right = 16, top = 10, bottom = 2 * lh + 12;
    m_plot = wxRect(left, top, std::max(10, sz.x - left - right), std::max(10, sz.y - top - bottom));
    const wxRect& R = m_plot;
    auto X = [&](double t) { return R.x + (t - m_x0) / (m_x1 - m_x0) * R.width; };
    auto Y = [&](double h) { return R.y + R.height - (h - y0) / (y1 - y0) * R.height; };
    // grille et graduations
    dc.SetPen(wxPen(wxColour(225, 225, 225)));
    dc.SetTextForeground(wxColour(90, 90, 90));
    for (double v = y0; v <= y1 + ys * 0.01; v += ys) {
      const int y = (int)Y(v);
      dc.DrawLine(R.x, y, R.GetRight(), y);
      const wxString s = wxString::Format(ys < 0.1 ? "%.2f" : "%.1f", v);
      dc.DrawText(s, R.x - 6 - dc.GetTextExtent(s).x, y - lh / 2);
    }
    const long long span = (long long)(m_x1 - m_x0);
    const long long st = NiceTimeStep(std::max<long long>(1, span), std::max(2, R.width / 110));
    for (long long t = ((long long)m_x0 / st + 1) * st; t <= (long long)m_x1; t += st) {
      const int x = (int)X((double)t);
      dc.DrawLine(x, R.y, x, R.GetBottom());
      std::string s = pd::FormatTime(t);  // AAAA-MM-JJ hh:mm
      const wxString lab = st >= 86400 ? W8(s.substr(0, 10).c_str()) : W8(s.substr(11).c_str());
      dc.DrawText(lab, x - dc.GetTextExtent(lab).x / 2, R.GetBottom() + 4);
      if (st < 86400 && s.substr(11) == "00:00")
        dc.DrawText(W8(s.substr(0, 10).c_str()), x - dc.GetTextExtent(W8(s.substr(0, 10).c_str())).x / 2,
                    R.GetBottom() + 4 + lh);
    }
    dc.SetPen(wxPen(wxColour(120, 120, 120)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(R);
    // courbe
    dc.SetClippingRegion(R);
    dc.SetPen(wxPen(wxColour(31, 119, 180), 2));
    std::vector<wxPoint> pts;
    for (size_t i = 0; i < T.size(); ++i) {
      if (T[i] < m_x0 - st && i + 1 < T.size() && T[i + 1] < m_x0) continue;
      pts.emplace_back((int)X((double)T[i]), (int)Y(H[i] / m_k));
      if (T[i] > m_x1) break;
    }
    if (pts.size() > 1) dc.DrawLines((int)pts.size(), pts.data());
    if (pts.size() < 200) {
      dc.SetBrush(wxBrush(wxColour(31, 119, 180)));
      for (const auto& p : pts) dc.DrawCircle(p, 2);
    }
    // lecture au curseur
    if (R.Contains(m_mouse)) {
      const double t = m_x0 + (double)(m_mouse.x - R.x) / R.width * (m_x1 - m_x0);
      float h;
      dc.SetPen(wxPen(wxColour(214, 39, 40), 1, wxPENSTYLE_DOT));
      dc.DrawLine(m_mouse.x, R.y, m_mouse.x, R.GetBottom());
      wxString txt = W8(pd::FormatTime((long long)t).c_str());
      if (m_s->at((long long)t, h)) {
        txt += wxString::Format("   %.2f ", h / m_k) + m_unit;
        dc.SetBrush(wxBrush(wxColour(214, 39, 40)));
        dc.DrawCircle(m_mouse.x, (int)Y(h / m_k), 3);
      }
      const wxSize ts = dc.GetTextExtent(txt);
      int tx = std::min(m_mouse.x + 8, R.GetRight() - ts.x - 4);
      dc.SetBrush(wxBrush(wxColour(255, 255, 255, 220)));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRectangle(tx - 3, R.y + 3, ts.x + 6, ts.y + 4);
      dc.SetTextForeground(*wxBLACK);
      dc.DrawText(txt, tx, R.y + 5);
    }
    dc.DestroyClippingRegion();
  }

  std::shared_ptr<const pd::WaterLevelSeries> m_s;
  double m_k = 1.0;  // mètres par unité affichée
  wxString m_unit;
  double m_x0 = 0, m_x1 = 1;
  wxRect m_plot;
  wxPoint m_mouse{-1, -1};
};
}  // namespace

WaterLevelDialog::WaterLevelDialog(wxWindow* parent, std::shared_ptr<const pd::WaterLevelSeries> s,
                                   double k, const wxString& unit)
    : wxDialog(parent, wxID_ANY, U8("Hauteurs d'eau observées"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
  auto* v = new wxBoxSizer(wxVERTICAL);
  wxString info = wxString::Format(U8("%d fichier(s), %d hauteurs du %s au %s ; de %.2f à %.2f %s"),
                                   (int)s->files().size(), (int)s->size(), W8(pd::FormatTime(s->tMin()).c_str()),
                                   W8(pd::FormatTime(s->tMax()).c_str()), s->hMin() / k, s->hMax() / k, unit);
  if (s->skippedLines())
    info += wxString::Format(U8(" ; %d ligne(s) illisible(s) ignorée(s)"), s->skippedLines());
  info += U8("\nUne sonde est corrigée par interpolation entre les deux hauteurs qui l'encadrent "
             "(valeur extrême gardée jusqu'à %s hors de la période).");
  {
    const long long tol = s->tolerance();
    info.Replace("%s", tol >= 86400 ? wxString(U8("1 jour")) : wxString::Format("%lld min", tol / 60));
  }
  info += U8("\nDates et heures en UTC.");
  v->Add(new wxStaticText(this, wxID_ANY, info), 0, wxEXPAND | wxALL, 8);
  v->Add(new WaterLevelPanel(this, s, k, unit), 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
  v->Add(new wxStaticText(this, wxID_ANY, U8("Molette : zoom sur le temps ; double-clic : toute la période.")),
         0, wxALL, 8);
  v->Add(CreateStdDialogButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 8);
  Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
  SetEscapeId(wxID_CLOSE);
  SetSizerAndFit(v);
  CentreOnParent();
}
