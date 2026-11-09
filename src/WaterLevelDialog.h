// PersonalDepth_pi - courbe des hauteurs d'eau observées
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <wx/wx.h>

#include <memory>

namespace pd { class WaterLevelSeries; }

class WaterLevelDialog : public wxDialog {
 public:
  // hauteurs de la série en m, affichées en unité : valeur / toMeters
  WaterLevelDialog(wxWindow* parent, std::shared_ptr<const pd::WaterLevelSeries> s, double toMeters,
                   const wxString& unit);
};
