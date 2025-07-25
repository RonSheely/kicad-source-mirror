/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KICAD_PANEL_SETUP_ANNOTATION_H
#define KICAD_PANEL_SETUP_ANNOTATION_H

#include <widgets/unit_binder.h>
#include "panel_setup_annotation_base.h"

class SCH_EDIT_FRAME;
class SCHEMATIC_SETTINGS;
class GAL_OPTIONS_PANEL;


class PANEL_SETUP_ANNOTATION : public PANEL_SETUP_ANNOTATION_BASE
{
public:
    PANEL_SETUP_ANNOTATION( wxWindow* aWindow, SCH_EDIT_FRAME* aFrame  );

    bool TransferDataToWindow() override;
    bool TransferDataFromWindow() override;

    void ResetPanel() override;

    void ImportSettingsFrom( SCHEMATIC_SETTINGS& aSettings );

private:
    SCH_EDIT_FRAME*    m_frame;
};


#endif // KICAD_PANEL_SETUP_ANNOTATION_H
