// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef FOLDER_SELECTOR2_H_073246031245342566
#define FOLDER_SELECTOR2_H_073246031245342566

#include <zen/zstring.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <wx+/file_drop.h>

namespace rts
{
//handle drag and drop, tooltip, label and manual input, coordinating a wxWindow, wxButton, and wxTextCtrl

class FolderSelector2 : public wxEvtHandler
{
public:
    FolderSelector2(wxWindow&     dropWindow,
                    wxButton&     selectButton,
                    wxTextCtrl&   folderPathCtrl,
                    wxStaticText* staticText); //optional

    ~FolderSelector2();

    Zstring getPath() const;
    void setPath(const Zstring& dirpath);

private:
    void onMouseWheel    (wxMouseEvent& event);
    void onFilesDropped  (zen::FileDropEvent& event);
    void onEditFolderPath(wxCommandEvent& event);
    void onSelectDir     (wxCommandEvent& event);

    wxWindow&     dropWindow_;
    wxButton&     selectButton_;
    wxTextCtrl&   folderPathCtrl_;
    wxStaticText* staticText_ = nullptr; //optional
};
}

#endif //FOLDER_SELECTOR2_H_073246031245342566
