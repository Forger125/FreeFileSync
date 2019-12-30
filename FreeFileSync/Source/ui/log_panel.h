// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef LOG_PANEL_3218470817450193
#define LOG_PANEL_3218470817450193

#include <zen/error_log.h>
#include "gui_generated.h"
#include <wx+/grid.h>


namespace fff
{
class MessageView;

class LogPanel : public LogPanelGenerated
{
public:
    LogPanel(wxWindow* parent) : LogPanelGenerated(parent) { setLog(nullptr); }

    void setLog(const std::shared_ptr<const zen::ErrorLog>& log);

private:
    MessageView& getDataView();
    void updateGrid();

    void OnErrors  (wxCommandEvent& event) override;
    void OnWarnings(wxCommandEvent& event) override;
    void OnInfo    (wxCommandEvent& event) override;
    void onGridButtonEvent(wxKeyEvent& event);
    void onMsgGridContext (zen::GridClickEvent& event);
    void onLocalKeyEvent  (wxKeyEvent& event);

    void copySelectionToClipboard();

    bool processingKeyEventHandler_ = false;
};
}

#endif //LOG_PANEL_3218470817450193
