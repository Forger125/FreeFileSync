// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef TRAY_MENU_H_3967857420987534253245
#define TRAY_MENU_H_3967857420987534253245

#include <wx/string.h>
#include "xml_proc.h"


namespace rts
{
enum class AbortReason
{
    REQUEST_GUI,
    REQUEST_EXIT
};
AbortReason runFolderMonitor(const XmlRealConfig& config, const wxString& jobname); //jobname may be empty
}

#endif //TRAY_MENU_H_3967857420987534253245
