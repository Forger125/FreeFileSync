// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "main_dlg.h"
#include <zen/format_unit.h>
#include <zen/file_access.h>
#include <zen/file_io.h>
#include <zen/thread.h>
#include <zen/shell_execute.h>
#include <zen/perf.h>
#include <zen/shutdown.h>
#include <wx/clipbrd.h>
#include <wx/wupdlock.h>
#include <wx/sound.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h>
#include <wx/valtext.h>
#include <wx+/context_menu.h>
#include <wx+/bitmap_button.h>
#include <wx+/app_main.h>
#include <wx+/toggle_button.h>
#include <wx+/no_flicker.h>
#include <wx+/rtl.h>
#include <wx+/font_size.h>
#include <wx+/popup_dlg.h>
#include <wx+/window_tools.h>
#include <wx+/image_resources.h>
#include "cfg_grid.h"
#include "version_check.h"
#include "gui_status_handler.h"
#include "small_dlgs.h"
#include "progress_indicator.h"
#include "folder_pair.h"
#include "search_grid.h"
#include "batch_config.h"
#include "triple_splitter.h"
#include "app_icon.h"
#include "../afs/concrete.h"
#include "../afs/native.h"
#include "../base/comparison.h"
#include "../base/synchronization.h"
#include "../base/algorithm.h"
#include "../base/resolve_path.h"
#include "../base/lock_holder.h"
#include "../base/icon_loader.h"
#include "../ffs_paths.h"
#include "../localization.h"
#include "../version/version.h"

using namespace zen;
using namespace fff;


namespace
{
const size_t EXT_APP_MASS_INVOKE_THRESHOLD = 10; //more is likely a user mistake (Explorer uses limit of 15)
const size_t EXT_APP_MAX_TOTAL_WAIT_TIME_MS = 1000;

const int TOP_BUTTON_OPTIMAL_WIDTH_DIP = 170;
const std::chrono::milliseconds LAST_USED_CFG_EXISTENCE_CHECK_TIME_MAX(500);
const std::chrono::milliseconds FILE_GRID_POST_UPDATE_DELAY(400);


IconBuffer::IconSize convert(FileIconSize isize)
{
    switch (isize)
    {
        case FileIconSize::small:
            return IconBuffer::SIZE_SMALL;
        case FileIconSize::medium:
            return IconBuffer::SIZE_MEDIUM;
        case FileIconSize::large:
            return IconBuffer::SIZE_LARGE;
    }
    return IconBuffer::SIZE_SMALL;
}


bool acceptDialogFileDrop(const std::vector<Zstring>& shellItemPaths)
{
    return std::any_of(shellItemPaths.begin(), shellItemPaths.end(), [](const Zstring& shellItemPath)
    {
        const Zstring ext = getFileExtension(shellItemPath);
        return equalAsciiNoCase(ext, "ffs_gui") ||
               equalAsciiNoCase(ext, "ffs_batch");
    });
}


}

//------------------------------------------------------------------
/*    class hierarchy:

           template<>
           FolderPairPanelBasic
                    /|\
                     |
           template<>
           FolderPairCallback   FolderPairPanelGenerated
                    /|\                  /|\
            _________|_________   ________|
           |                   | |
    FolderPairFirst      FolderPairPanel
*/

template <class GuiPanel>
class fff::FolderPairCallback : public FolderPairPanelBasic<GuiPanel> //implements callback functionality to MainDialog as imposed by FolderPairPanelBasic
{
public:
    FolderPairCallback(GuiPanel& basicPanel, MainDialog& mainDialog,

                       wxPanel&          dropWindow1L,
                       wxPanel&          dropWindow1R,
                       wxButton&         selectFolderButtonL,
                       wxButton&         selectFolderButtonR,
                       wxButton&         selectSftpButtonL,
                       wxButton&         selectSftpButtonR,
                       FolderHistoryBox& dirpathL,
                       FolderHistoryBox& dirpathR,
                       Zstring& folderLastSelectedL,
                       Zstring& folderLastSelectedR,
                       Zstring& sftpKeyFileLastSelected,
                       wxStaticText*     staticTextL,
                       wxStaticText*     staticTextR,
                       wxWindow*         dropWindow2L,
                       wxWindow*         dropWindow2R) :
        FolderPairPanelBasic<GuiPanel>(basicPanel), //pass FolderPairPanelGenerated part...
        mainDlg_(mainDialog),
        folderSelectorLeft_ (&mainDialog, dropWindow1L, selectFolderButtonL, selectSftpButtonL, dirpathL, folderLastSelectedL, sftpKeyFileLastSelected, staticTextL, dropWindow2L, droppedPathsFilter_, getDeviceParallelOps_, setDeviceParallelOps_),
        folderSelectorRight_(&mainDialog, dropWindow1R, selectFolderButtonR, selectSftpButtonR, dirpathR, folderLastSelectedR, sftpKeyFileLastSelected, staticTextR, dropWindow2R, droppedPathsFilter_, getDeviceParallelOps_, setDeviceParallelOps_)
    {
        folderSelectorLeft_ .setSiblingSelector(&folderSelectorRight_);
        folderSelectorRight_.setSiblingSelector(&folderSelectorLeft_);

        folderSelectorLeft_ .Bind(EVENT_ON_FOLDER_SELECTED, [&mainDialog](wxCommandEvent& event) { mainDialog.onDirSelected(event); });
        folderSelectorRight_.Bind(EVENT_ON_FOLDER_SELECTED, [&mainDialog](wxCommandEvent& event) { mainDialog.onDirSelected(event); });

        folderSelectorLeft_ .Bind(EVENT_ON_FOLDER_MANUAL_EDIT, [&mainDialog](wxCommandEvent& event) { mainDialog.onDirManualCorrection(event); });
        folderSelectorRight_.Bind(EVENT_ON_FOLDER_MANUAL_EDIT, [&mainDialog](wxCommandEvent& event) { mainDialog.onDirManualCorrection(event); });
    }

    void setValues(const LocalPairConfig& lpc)
    {
        this->setConfig(lpc.localCmpCfg, lpc.localSyncCfg, lpc.localFilter);
        folderSelectorLeft_ .setPath(lpc.folderPathPhraseLeft);
        folderSelectorRight_.setPath(lpc.folderPathPhraseRight);
    }

    LocalPairConfig getValues() const
    {
        return LocalPairConfig(folderSelectorLeft_.getPath(), folderSelectorRight_.getPath(), this->getCompConfig(), this->getSyncConfig(), this->getFilterConfig());
    }

private:
    MainConfiguration getMainConfig() const override { return mainDlg_.getConfig().mainCfg; }
    wxWindow*         getParentWindow() override { return &mainDlg_; }
    std::unique_ptr<FilterConfig>& getFilterCfgOnClipboardRef() override { return mainDlg_.filterCfgOnClipboard_; }

    void onLocalCompCfgChange  () override { mainDlg_.applyCompareConfig(false /*setDefaultViewType*/); }
    void onLocalSyncCfgChange  () override { mainDlg_.applySyncDirections(); }
    void onLocalFilterCfgChange() override { mainDlg_.applyFilterConfig(); } //re-apply filter

    const std::function<bool(const std::vector<Zstring>& shellItemPaths)> droppedPathsFilter_ = [&](const std::vector<Zstring>& shellItemPaths)
    {
        if (acceptDialogFileDrop(shellItemPaths))
        {
            assert(!shellItemPaths.empty());
            mainDlg_.loadConfiguration(shellItemPaths);
            return false; //don't set dropped paths
        }
        return true; //do set dropped paths
    };

    const std::function<size_t(const Zstring& folderPathPhrase)> getDeviceParallelOps_ = [&](const Zstring& folderPathPhrase)
    {
        return getDeviceParallelOps(mainDlg_.currentCfg_.mainCfg.deviceParallelOps, folderPathPhrase);
    };

    const std::function<void(const Zstring& folderPathPhrase, size_t parallelOps)> setDeviceParallelOps_ = [&](const Zstring& folderPathPhrase, size_t parallelOps)
    {
        setDeviceParallelOps(mainDlg_.currentCfg_.mainCfg.deviceParallelOps, folderPathPhrase, parallelOps);
        mainDlg_.updateUnsavedCfgStatus();
    };

    MainDialog& mainDlg_;
    FolderSelector folderSelectorLeft_;
    FolderSelector folderSelectorRight_;
};


class fff::FolderPairPanel :
    public FolderPairPanelGenerated, //FolderPairPanel "owns" FolderPairPanelGenerated!
    public FolderPairCallback<FolderPairPanelGenerated>
{
public:
    FolderPairPanel(wxWindow* parent,
                    MainDialog& mainDlg,
                    Zstring& folderLastSelectedL,
                    Zstring& folderLastSelectedR,
                    Zstring& sftpKeyFileLastSelected) :
        FolderPairPanelGenerated(parent),
        FolderPairCallback<FolderPairPanelGenerated>(static_cast<FolderPairPanelGenerated&>(*this), mainDlg,

                                                     *m_panelLeft,
                                                     *m_panelRight,
                                                     *m_buttonSelectFolderLeft,
                                                     *m_buttonSelectFolderRight,
                                                     *m_bpButtonSelectAltFolderLeft,
                                                     *m_bpButtonSelectAltFolderRight,
                                                     *m_folderPathLeft,
                                                     *m_folderPathRight,
                                                     folderLastSelectedL,
                                                     folderLastSelectedR,
                                                     sftpKeyFileLastSelected,
                                                     nullptr /*staticText*/, nullptr /*staticText*/,
                                                     nullptr /*dropWindow2*/, nullptr /*dropWindow2*/) {}
};


class fff::FolderPairFirst : public FolderPairCallback<MainDialogGenerated>
{
public:
    FolderPairFirst(MainDialog& mainDlg,
                    Zstring& folderLastSelectedL,
                    Zstring& folderLastSelectedR,
                    Zstring& sftpKeyFileLastSelected) :
        FolderPairCallback<MainDialogGenerated>(mainDlg, mainDlg,

                                                *mainDlg.m_panelTopLeft,
                                                *mainDlg.m_panelTopRight,
                                                *mainDlg.m_buttonSelectFolderLeft,
                                                *mainDlg.m_buttonSelectFolderRight,
                                                *mainDlg.m_bpButtonSelectAltFolderLeft,
                                                *mainDlg.m_bpButtonSelectAltFolderRight,
                                                *mainDlg.m_folderPathLeft,
                                                *mainDlg.m_folderPathRight,
                                                folderLastSelectedL,
                                                folderLastSelectedR,
                                                sftpKeyFileLastSelected,
                                                mainDlg.m_staticTextResolvedPathL,
                                                mainDlg.m_staticTextResolvedPathR,
                                                &mainDlg.m_gridMainL->getMainWin(),
                                                &mainDlg.m_gridMainR->getMainWin()) {}
};




namespace
{
void updateTopButton(wxBitmapButton& btn, const wxImage& img, const wxString& varName, const char* varIconName /*optional*/, bool makeGrey)
{
    wxImage btnIconImg = makeGrey ? greyScale(img) : img;

    wxImage btnLabelImg = createImageFromText(btn.GetLabel(), btn.GetFont(), wxSystemSettings::GetColour(makeGrey ? wxSYS_COLOUR_GRAYTEXT : wxSYS_COLOUR_BTNTEXT));

    wxImage varLabelImg = createImageFromText(varName,
                                              wxFont(wxNORMAL_FONT->GetPointSize(), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD),
                                              wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    wxImage varImg = varLabelImg;
    if (varIconName)
    {
        wxImage varIcon = mirrorIfRtl(loadImage(varIconName, -1 /*maxWidth*/, getDefaultMenuIconSize()));

        if (makeGrey)
            varIcon = greyScale(varIcon);

        varImg = btn.GetLayoutDirection() != wxLayout_RightToLeft ?
                 stackImages(varLabelImg, varIcon, ImageStackLayout::horizontal, ImageStackAlignment::center, fastFromDIP(5)) :
                 stackImages(varIcon, varLabelImg, ImageStackLayout::horizontal, ImageStackAlignment::center, fastFromDIP(5));
    }

    wxImage btnImg = stackImages(btnLabelImg, varImg, ImageStackLayout::vertical, ImageStackAlignment::center);

    btnImg = btn.GetLayoutDirection() != wxLayout_RightToLeft ?
             stackImages(btnIconImg, btnImg, ImageStackLayout::horizontal, ImageStackAlignment::center, fastFromDIP(5)) :
             stackImages(btnImg, btnIconImg, ImageStackLayout::horizontal, ImageStackAlignment::center, fastFromDIP(5));

    wxSize btnSize = btnImg.GetSize() + wxSize(fastFromDIP(10), fastFromDIP(10)); //add border space
    btnSize.x = std::max(btnSize.x, fastFromDIP(TOP_BUTTON_OPTIMAL_WIDTH_DIP));

    btnImg = resizeCanvas(btnImg, btnSize, wxALIGN_CENTER);

    setImage(btn, btnImg);
}

//##################################################################################################################################

XmlGlobalSettings tryLoadGlobalConfig(const Zstring& globalConfigFilePath) //blocks GUI on errors!
{
    XmlGlobalSettings globalCfg;
    try
    {
        std::wstring warningMsg;
        readConfig(globalConfigFilePath, globalCfg, warningMsg); //throw FileError
        assert(warningMsg.empty()); //ignore parsing errors: should be migration problems only *cross-fingers*
    }
    catch (FileError&)
    {
        try
        {
            if (itemStillExists(globalConfigFilePath)) //throw FileError
                throw;
        }
        catch (const FileError& e2)
        {
            showNotificationDialog(nullptr, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e2.toString())); //no parent window: main dialog not yet created!
        }
    }
    return globalCfg;
}
}


void MainDialog::create(const Zstring& globalConfigFilePath)
{
    const XmlGlobalSettings globalSettings = tryLoadGlobalConfig(globalConfigFilePath);

    std::vector<Zstring> cfgFilePaths = globalSettings.mainDlg.cfgFilesLastUsed;

    //------------------------------------------------------------------------------------------
    //check existence of all files in parallel:
    AsyncFirstResult<std::false_type> firstUnavailableFile;

    for (const Zstring& filePath : cfgFilePaths)
        firstUnavailableFile.addJob([filePath]() -> std::optional<std::false_type>
    {
        assert(!filePath.empty());
        if (!fileAvailable(filePath))
            return std::false_type();
        return {};
    });

    //potentially slow network access: give all checks 500ms to finish
    const bool allFilesAvailable = firstUnavailableFile.timedWait(LAST_USED_CFG_EXISTENCE_CHECK_TIME_MAX) && //false: time elapsed
                                   !firstUnavailableFile.get(); //no missing
    if (!allFilesAvailable)
        cfgFilePaths.clear(); //we do NOT want to show an error due to last config file missing on application start!
    //------------------------------------------------------------------------------------------

    if (cfgFilePaths.empty())
    {
        const Zstring lastRunConfigFilePath = getLastRunConfigPath();
        if (fileAvailable(lastRunConfigFilePath)) //3. try to load auto-save config (should not block)
            cfgFilePaths.push_back(lastRunConfigFilePath);
        //else: not-existing/access error? => user may click on [Last session] later
    }

    XmlGuiConfig guiCfg; //contains default values

    //add default exclusion filter: this is only ever relevant when creating new configurations!
    //a default XmlGuiConfig does not need these user-specific exclusions!
    Zstring& excludeFilter = guiCfg.mainCfg.globalFilter.excludeFilter;
    if (!excludeFilter.empty() && !endsWith(excludeFilter, Zstr('\n')))
        excludeFilter += Zstr('\n');
    excludeFilter += globalSettings.defaultExclusionFilter;

    if (!cfgFilePaths.empty())
        try
        {
            std::wstring warningMsg;
            readAnyConfig(cfgFilePaths, guiCfg, warningMsg); //throw FileError

            if (!warningMsg.empty())
                showNotificationDialog(nullptr, DialogInfoType::warning, PopupDialogCfg().setDetailInstructions(warningMsg));
            //what about showing as changed config on parsing errors????
        }
        catch (const FileError& e)
        {
            showNotificationDialog(nullptr, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        }

    //------------------------------------------------------------------------------------------

    create(globalConfigFilePath, &globalSettings, guiCfg, cfgFilePaths, false);
}


void MainDialog::create(const Zstring& globalConfigFilePath,
                        const XmlGlobalSettings* globalSettings,
                        const XmlGuiConfig& guiCfg,
                        const std::vector<Zstring>& referenceFiles,
                        bool startComparison)
{
    const XmlGlobalSettings globSett = globalSettings ? *globalSettings : tryLoadGlobalConfig(globalConfigFilePath);

    try
    {
        //we need to set language *before* creating MainDialog!
        setLanguage(globSett.programLanguage); //throw FileError
    }
    catch (const FileError& e)
    {
        showNotificationDialog(nullptr, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        //continue!
    }

    MainDialog* frame = new MainDialog(globalConfigFilePath, guiCfg, referenceFiles, globSett, startComparison);
    frame->Show();
}


MainDialog::MainDialog(const Zstring& globalConfigFilePath,
                       const XmlGuiConfig& guiCfg,
                       const std::vector<Zstring>& referenceFiles,
                       const XmlGlobalSettings& globalSettings,
                       bool startComparison) :
    MainDialogGenerated(nullptr),
    globalConfigFilePath_(globalConfigFilePath),
    folderHistoryLeft_ (std::make_shared<HistoryList>(globalSettings.mainDlg.folderHistoryLeft,  globalSettings.folderHistoryMax)),
    folderHistoryRight_(std::make_shared<HistoryList>(globalSettings.mainDlg.folderHistoryRight, globalSettings.folderHistoryMax)),
    imgTrashSmall_([]
{
    try { return extractWxImage(fff::getTrashIcon(getDefaultMenuIconSize())); /*throw SysError*/ }
    catch (SysError&) { assert(false); return loadImage("delete_recycler", getDefaultMenuIconSize()); }
}
())
{
    SetSizeHints(fastFromDIP(640), fastFromDIP(400));

    //setup sash: detach + reparent:
    m_splitterMain->SetSizer(nullptr); //alas wxFormbuilder doesn't allow us to have child windows without a sizer, so we have to remove it here
    m_splitterMain->setupWindows(m_gridMainL, m_gridMainC, m_gridMainR);

    setRelativeFontSize(*m_buttonCompare, 1.4);
    setRelativeFontSize(*m_buttonSync,    1.4);
    setRelativeFontSize(*m_buttonCancel,  1.4);

    SetIcon(getFfsIcon()); //set application icon

    auto generateSaveAsImage = [](const char* layoverName)
    {
        const wxSize oldSize = loadImage("cfg_save").GetSize();

        wxImage backImg = loadImage("cfg_save", oldSize.GetWidth() * 9 / 10);
        backImg = resizeCanvas(backImg, oldSize, wxALIGN_BOTTOM | wxALIGN_LEFT);

        return layOver(backImg, loadImage(layoverName, backImg.GetWidth() * 7 / 10), wxALIGN_TOP | wxALIGN_RIGHT);
    };

    m_bpButtonCmpConfig ->SetBitmapLabel(loadImage("options_compare"));
    m_bpButtonSyncConfig->SetBitmapLabel(loadImage("options_sync"));

    m_bpButtonCmpContext   ->SetBitmapLabel(mirrorIfRtl(loadImage("button_arrow_right")));
    m_bpButtonFilterContext->SetBitmapLabel(mirrorIfRtl(loadImage("button_arrow_right")));
    m_bpButtonSyncContext  ->SetBitmapLabel(mirrorIfRtl(loadImage("button_arrow_right")));
    m_bpButtonViewFilterContext->SetBitmapLabel(mirrorIfRtl(loadImage("button_arrow_right")));

    m_bpButtonNew        ->SetBitmapLabel(loadImage("cfg_new"));
    m_bpButtonOpen       ->SetBitmapLabel(loadImage("cfg_load"));
    m_bpButtonSaveAs     ->SetBitmapLabel(generateSaveAsImage("start_sync"));
    m_bpButtonSaveAsBatch->SetBitmapLabel(generateSaveAsImage("cfg_batch"));

    m_bpButtonAddPair    ->SetBitmapLabel(loadImage("item_add"));
    m_bpButtonHideSearch ->SetBitmapLabel(loadImage("close_panel"));
    m_bpButtonToggleLog  ->SetBitmapLabel(loadImage("log_file"));

    m_bpButtonFilter   ->SetMinSize({loadImage("options_filter").GetWidth() + fastFromDIP(27), -1}); //make the filter button wider
    m_textCtrlSearchTxt->SetMinSize({fastFromDIP(220), -1});

    //----------------------------------------------------------------------------------------
    wxImage labelImage = createImageFromText(_("Select view:"), m_bpButtonViewType->GetFont(), wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));

    labelImage = resizeCanvas(labelImage, labelImage.GetSize() + wxSize(fastFromDIP(10), 0), wxALIGN_CENTER); //add border space

    auto generateViewTypeImage = [&](const char* imgName)
    {
        return stackImages(labelImage, mirrorIfRtl(loadImage(imgName)), ImageStackLayout::vertical, ImageStackAlignment::center);
    };
    m_bpButtonViewType->init(generateViewTypeImage("viewtype_sync_action"),
                             generateViewTypeImage("viewtype_cmp_result"));
    //tooltip is updated dynamically in setViewTypeSyncAction()
    //----------------------------------------------------------------------------------------
    m_bpButtonShowExcluded  ->SetToolTip(_("Show filtered or temporarily excluded files"));
    m_bpButtonShowEqual     ->SetToolTip(_("Show files that are equal"));
    m_bpButtonShowConflict  ->SetToolTip(_("Show conflicts"));

    m_bpButtonShowCreateLeft ->SetToolTip(_("Show files that will be created on the left side"));
    m_bpButtonShowCreateRight->SetToolTip(_("Show files that will be created on the right side"));
    m_bpButtonShowDeleteLeft ->SetToolTip(_("Show files that will be deleted on the left side"));
    m_bpButtonShowDeleteRight->SetToolTip(_("Show files that will be deleted on the right side"));
    m_bpButtonShowUpdateLeft ->SetToolTip(_("Show files that will be updated on the left side"));
    m_bpButtonShowUpdateRight->SetToolTip(_("Show files that will be updated on the right side"));
    m_bpButtonShowDoNothing  ->SetToolTip(_("Show files that won't be copied"));

    m_bpButtonShowLeftOnly  ->SetToolTip(_("Show files that exist on left side only"));
    m_bpButtonShowRightOnly ->SetToolTip(_("Show files that exist on right side only"));
    m_bpButtonShowLeftNewer ->SetToolTip(_("Show files that are newer on left"));
    m_bpButtonShowRightNewer->SetToolTip(_("Show files that are newer on right"));
    m_bpButtonShowDifferent ->SetToolTip(_("Show files that are different"));
    //----------------------------------------------------------------------------------------

    const wxImage& imgFile = IconBuffer::genericFileIcon(IconBuffer::SIZE_SMALL);
    const wxImage& imgDir  = IconBuffer::genericDirIcon (IconBuffer::SIZE_SMALL);

    //init log panel
    setRelativeFontSize(*m_staticTextSyncResult, 1.5);

    m_bitmapItemStat->SetBitmap(imgFile);

    wxImage imgTime = loadImage("time", -1 /*maxWidth*/, imgFile.GetHeight());
    m_bitmapTimeStat->SetBitmap(imgTime);
    m_bitmapTimeStat->SetMinSize({-1, imgFile.GetHeight()});

    logPanel_ = new LogPanel(m_panelLog); //pass ownership
    bSizerLog->Add(logPanel_, 1, wxEXPAND);

    setLastOperationLog(ProcessSummary(), nullptr /*errorLog*/);

    //we have to use the OS X naming convention by default, because wxMac permanently populates the display menu when the wxMenuItem is created for the first time!
    //=> other wx ports are not that badly programmed; therefore revert:
    assert(m_menuItemOptions->GetItemLabel() == _("&Preferences") + L"\tCtrl+,"); //"Ctrl" is automatically mapped to command button!
    m_menuItemOptions->SetItemLabel(_("&Options"));

    //---------------- support for dockable gui style --------------------------------
    bSizerPanelHolder->Detach(m_panelTopButtons);
    bSizerPanelHolder->Detach(m_panelLog);
    bSizerPanelHolder->Detach(m_panelDirectoryPairs);
    bSizerPanelHolder->Detach(m_gridOverview);
    bSizerPanelHolder->Detach(m_panelCenter);
    bSizerPanelHolder->Detach(m_panelConfig);
    bSizerPanelHolder->Detach(m_panelViewFilter);

    auiMgr_.SetDockSizeConstraint(1 /*width_pct*/, 1 /*height_pct*/); //get rid: interferes with programmatic layout changes + doesn't limit what user can do

    auiMgr_.SetManagedWindow(this);
    auiMgr_.SetFlags(wxAUI_MGR_DEFAULT | wxAUI_MGR_LIVE_RESIZE);

    auiMgr_.Bind(wxEVT_AUI_PANE_CLOSE, [this](wxAuiManagerEvent& event)
    {
        //wxAuiManager::ClosePane already calls wxAuiManager::RestorePane if wxAuiPaneInfo::IsMaximized
        if (wxAuiPaneInfo* pi = event.GetPane())
            if (!pi->IsMaximized())
                pi->best_size = pi->rect.GetSize(); //ensure current window sizes will be used when pane is shown again:
        
        assert(event.GetPane()->rect != wxSize());
    });

    //daily WTF: wxAuiManager ignores old directory pane size in wxAuiPaneInfo::rect
    //and calculates new window sizes based on best_size/min_size during wxEVT_AUI_PANE_RESTORE!
    auiMgr_.Bind(wxEVT_AUI_PANE_MAXIMIZE, [this](wxAuiManagerEvent& event)
    {
        wxAuiPaneInfo& dirPane = auiMgr_.GetPane(m_panelDirectoryPairs);
        wxAuiPaneInfo& logPane = auiMgr_.GetPane(m_panelLog);

        //ensure current window sizes will be used during wxEVT_AUI_PANE_RESTORE:
        dirPane.best_size = dirPane.rect.GetSize();
        logPane.best_size = logPane.rect.GetSize();

        assert(dirPane.rect != wxSize());
        assert(logPane.rect != wxSize());
    });

    compareStatus_ = std::make_unique<CompareProgressPanel>(*this); //integrate the compare status panel (in hidden state)

    //caption required for all panes that can be manipulated by the users => used by context menu
    auiMgr_.AddPane(m_panelCenter,
                    wxAuiPaneInfo().Name(L"CenterPanel").CenterPane().PaneBorder(false));

    //set comparison button label tentatively for m_panelTopButtons to receive final height:
    updateTopButton(*m_buttonCompare, loadImage("compare"), L"Dummy", nullptr /*varIconName*/, false /*makeGrey*/);
    m_panelTopButtons->GetSizer()->SetSizeHints(m_panelTopButtons); //~=Fit() + SetMinSize()

    m_buttonCancel->SetMinSize({std::max(m_buttonCancel->GetSize().x, fastFromDIP(TOP_BUTTON_OPTIMAL_WIDTH_DIP)),
                                std::max(m_buttonCancel->GetSize().y, m_buttonCompare->GetSize().y)
                               });

    auiMgr_.AddPane(m_panelTopButtons,
                    wxAuiPaneInfo().Name(L"TopPanel").Layer(2).Top().Row(1).Caption(_("Main Bar")).CaptionVisible(false).
                    PaneBorder(false).Gripper().MinSize(fastFromDIP(TOP_BUTTON_OPTIMAL_WIDTH_DIP), m_panelTopButtons->GetSize().GetHeight()));
    //note: min height is calculated incorrectly by wxAuiManager if panes with and without caption are in the same row => use smaller min-size

    auiMgr_.AddPane(compareStatus_->getAsWindow(),
                    wxAuiPaneInfo().Name(L"ProgressPanel").Layer(2).Top().Row(2).CaptionVisible(false).PaneBorder(false).Hide().
                    //wxAui does not consider the progress panel's wxRAISED_BORDER and set's too small a panel height! => use correct value from wxWindow::GetSize()
                    MinSize(-1, compareStatus_->getAsWindow()->GetSize().GetHeight())); //bonus: minimal height isn't a bad idea anyway

    auiMgr_.AddPane(m_panelDirectoryPairs,
                    wxAuiPaneInfo().Name(L"FoldersPanel").Layer(2).Top().Row(3).Caption(_("Folder Pairs")).CaptionVisible(false).PaneBorder(false).Gripper());

    auiMgr_.AddPane(m_panelSearch,
                    wxAuiPaneInfo().Name(L"SearchPanel").Layer(2).Bottom().Row(3).Caption(_("Find")).CaptionVisible(false).PaneBorder(false).Gripper().
                    MinSize(fastFromDIP(100), m_panelSearch->GetSize().y).Hide());

    auiMgr_.AddPane(m_panelLog,
                    wxAuiPaneInfo().Name(L"LogPanel").Layer(2).Bottom().Row(2).Caption(_("Log")).MaximizeButton().Hide().
                    MinSize(fastFromDIP(100), fastFromDIP(100)).
                    BestSize(fastFromDIP(600), fastFromDIP(300)));

    m_panelViewFilter->GetSizer()->SetSizeHints(m_panelViewFilter); //~=Fit() + SetMinSize()
    auiMgr_.AddPane(m_panelViewFilter,
                    wxAuiPaneInfo().Name(L"ViewFilterPanel").Layer(2).Bottom().Row(1).Caption(_("View Settings")).CaptionVisible(false).
                    PaneBorder(false).Gripper().MinSize(fastFromDIP(100), m_panelViewFilter->GetSize().y));

    m_panelConfig->GetSizer()->SetSizeHints(m_panelConfig); //~=Fit() + SetMinSize()
    auiMgr_.AddPane(m_panelConfig,
                    wxAuiPaneInfo().Name(L"ConfigPanel").Layer(3).Left().Position(1).Caption(_("Configuration")).MinSize(bSizerCfgHistoryButtons->GetSize()));

    auiMgr_.AddPane(m_gridOverview,
                    wxAuiPaneInfo().Name(L"OverviewPanel").Layer(3).Left().Position(2).Caption(_("Overview")).
                    MinSize(fastFromDIP(100), fastFromDIP(100)).
                    BestSize(fastFromDIP(300), m_gridOverview->GetSize().GetHeight()));
    {
        wxAuiDockArt* artProvider = auiMgr_.GetArtProvider();

        wxFont font = artProvider->GetFont(wxAUI_DOCKART_CAPTION_FONT);
        font.SetWeight(wxFONTWEIGHT_BOLD);
        font.SetPointSize(wxNORMAL_FONT->GetPointSize()); //= larger than the wxAuiDockArt default; looks better on OS X
        artProvider->SetFont(wxAUI_DOCKART_CAPTION_FONT, font);
        artProvider->SetMetric(wxAUI_DOCKART_CAPTION_SIZE, font.GetPixelSize().GetHeight() + fastFromDIP(2 + 2));

        //- fix wxWidgets 3.1.0 insane color scheme
        artProvider->SetColor(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, *wxWHITE); //accessibility: always set both foreground AND background colors!
        artProvider->SetColor(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR,          wxColor(51, 147, 223)); //medium blue
        artProvider->SetColor(wxAUI_DOCKART_INACTIVE_CAPTION_GRADIENT_COLOUR, wxColor( 0, 120, 215)); //
        //wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT) -> better than wxBLACK, but which background to use?
    }
    //auiMgr_.Update(); -> redundant; called by setGlobalCfgOnInit() below

    defaultPerspective_ = auiMgr_.SavePerspective(); //does not need wxAuiManager::Update()!
    //----------------------------------------------------------------------------------
    //register view layout context menu
    m_panelTopButtons->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) { onSetLayoutContext(event); });
    m_panelConfig    ->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) { onSetLayoutContext(event); });
    m_panelViewFilter->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) { onSetLayoutContext(event); });
    m_panelStatusBar ->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) { onSetLayoutContext(event); });
    //----------------------------------------------------------------------------------

    //file grid: sorting
    m_gridMainL->Bind(EVENT_GRID_COL_LABEL_MOUSE_LEFT, [this](GridLabelClickEvent& event) { onGridLabelLeftClickRim(event, true  /*leftSide*/); });
    m_gridMainR->Bind(EVENT_GRID_COL_LABEL_MOUSE_LEFT, [this](GridLabelClickEvent& event) { onGridLabelLeftClickRim(event, false /*leftSide*/); });
    m_gridMainC->Bind(EVENT_GRID_COL_LABEL_MOUSE_LEFT, [this](GridLabelClickEvent& event) { onGridLabelLeftClickC(event); });

    m_gridMainL->Bind(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, [this](GridLabelClickEvent& event) { onGridLabelContextRim(event, true  /*leftSide*/); });
    m_gridMainR->Bind(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, [this](GridLabelClickEvent& event) { onGridLabelContextRim(event, false /*leftSide*/); });
    m_gridMainC->Bind(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, [this](GridLabelClickEvent& event) { onGridLabelContextC(event); });

    //file grid: context menu
    m_gridMainL->Bind(EVENT_GRID_CONTEXT_MENU, [this](GridContextMenuEvent& event) { onGridContextRim(event, true  /*leftSide*/); });
    m_gridMainR->Bind(EVENT_GRID_CONTEXT_MENU, [this](GridContextMenuEvent& event) { onGridContextRim(event, false /*leftSide*/); });

    m_gridMainL->Bind(EVENT_GRID_MOUSE_RIGHT_DOWN, [this](GridClickEvent& event) { onGridGroupContextRim(event, true  /*leftSide*/); });
    m_gridMainR->Bind(EVENT_GRID_MOUSE_RIGHT_DOWN, [this](GridClickEvent& event) { onGridGroupContextRim(event, false /*leftSide*/); });

    m_gridMainL->Bind(EVENT_GRID_MOUSE_LEFT_DOUBLE, [this](GridClickEvent& event) { onGridDoubleClickRim(event,  true /*leftSide*/); });
    m_gridMainR->Bind(EVENT_GRID_MOUSE_LEFT_DOUBLE, [this](GridClickEvent& event) { onGridDoubleClickRim(event, false /*leftSide*/); });

    //tree grid:
    m_gridOverview->Bind(EVENT_GRID_CONTEXT_MENU, [this](GridContextMenuEvent& event) { onTreeGridContext  (event); });
    m_gridOverview->Bind(EVENT_GRID_SELECT_RANGE, [this](GridSelectEvent&      event) { onTreeGridSelection(event); });

    //cfg grid:
    m_gridCfgHistory->getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onCfgGridKeyEvent(event); });
    m_gridCfgHistory->Bind(EVENT_GRID_SELECT_RANGE,          [this](GridSelectEvent&      event) { onCfgGridSelection     (event); });
    m_gridCfgHistory->Bind(EVENT_GRID_MOUSE_LEFT_DOUBLE,     [this](GridClickEvent&       event) { onCfgGridDoubleClick   (event); });
    m_gridCfgHistory->Bind(EVENT_GRID_CONTEXT_MENU,          [this](GridContextMenuEvent& event) { onCfgGridContext       (event); });
    m_gridCfgHistory->Bind(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, [this](GridLabelClickEvent&  event) { onCfgGridLabelContext  (event); });
    m_gridCfgHistory->Bind(EVENT_GRID_COL_LABEL_MOUSE_LEFT,  [this](GridLabelClickEvent&  event) { onCfgGridLabelLeftClick(event); });
    //----------------------------------------------------------------------------------

    m_panelSearch->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onSearchPanelKeyPressed(event); });


    //set tool tips with (non-translated!) short cut hint
    m_bpButtonNew        ->SetToolTip(replaceCpy(_("&New"),                  L"&", L"") + L" (Ctrl+N)"); //
    m_bpButtonOpen       ->SetToolTip(replaceCpy(_("&Open..."),              L"&", L"") + L" (Ctrl+O)"); //
    m_bpButtonSave       ->SetToolTip(replaceCpy(_("&Save"),                 L"&", L"") + L" (Ctrl+S)"); //reuse texts from gui builder
    m_bpButtonSaveAs     ->SetToolTip(replaceCpy(_("Save &as..."),           L"&", L""));                //
    m_bpButtonSaveAsBatch->SetToolTip(replaceCpy(_("Save as &batch job..."), L"&", L""));                //

    m_bpButtonToggleLog ->SetToolTip(replaceCpy(_("Show &log"),                 L"&", L"") + L" (F4)"); //
    m_buttonCompare     ->SetToolTip(replaceCpy(_("Start &comparison"),         L"&", L"") + L" (F5)"); //
    m_bpButtonCmpConfig ->SetToolTip(replaceCpy(_("C&omparison settings"),      L"&", L"") + L" (F6)"); //
    m_bpButtonSyncConfig->SetToolTip(replaceCpy(_("S&ynchronization settings"), L"&", L"") + L" (F8)"); //
    m_buttonSync        ->SetToolTip(replaceCpy(_("Start &synchronization"),    L"&", L"") + L" (F9)"); //
    m_bpButtonSwapSides ->SetToolTip(_("Swap sides") + L" (Ctrl+W)");

    m_bpButtonCmpContext ->SetToolTip(m_bpButtonCmpConfig ->GetToolTipText());
    m_bpButtonSyncContext->SetToolTip(m_bpButtonSyncConfig->GetToolTipText());


    m_bitmapSmallDirectoryLeft ->SetBitmap(imgDir);
    m_bitmapSmallFileLeft      ->SetBitmap(imgFile);
    m_bitmapSmallDirectoryRight->SetBitmap(imgDir);
    m_bitmapSmallFileRight     ->SetBitmap(imgFile);

    //---------------------- menu bar----------------------------
    m_menuItemNew        ->SetBitmap(loadImage("cfg_new_sicon"));
    m_menuItemLoad       ->SetBitmap(loadImage("cfg_load_sicon"));
    m_menuItemSave       ->SetBitmap(loadImage("cfg_save_sicon"));
    m_menuItemSaveAsBatch->SetBitmap(loadImage("cfg_batch_sicon"));

    m_menuItemShowLog     ->SetBitmap(loadImage("log_file_sicon"));
    m_menuItemCompare     ->SetBitmap(loadImage("compare_sicon"));
    m_menuItemCompSettings->SetBitmap(loadImage("options_compare_sicon"));
    m_menuItemFilter      ->SetBitmap(loadImage("options_filter_sicon"));
    m_menuItemSyncSettings->SetBitmap(loadImage("options_sync_sicon"));
    m_menuItemSynchronize ->SetBitmap(loadImage("start_sync_sicon"));

    m_menuItemOptions     ->SetBitmap(loadImage("settings_sicon"));
    m_menuItemFind        ->SetBitmap(loadImage("find_sicon"));

    m_menuItemHelp ->SetBitmap(loadImage("help_sicon"));
    m_menuItemAbout->SetBitmap(loadImage("about_sicon"));
    m_menuItemCheckVersionNow->SetBitmap(loadImage("update_check_sicon"));

    auto fixMenuIcons = [](wxMenu& menu) //GTK: image must be set *before* adding wxMenuItem to menu or it won't show
    {
        std::vector<std::pair<wxMenuItem*, size_t /*pos*/>> itemsWithBmp;
        {
            size_t pos = 0;
            for (wxMenuItem* item : menu.GetMenuItems())
            {
                if (item->GetBitmap().IsOk())
                    itemsWithBmp.emplace_back(item, pos);
                ++pos;
            }
        }

        for (const auto& [item, pos] : itemsWithBmp)
            if (!menu.Insert(pos, menu.Remove(item))) //detach + reinsert
                assert(false);
    };
    fixMenuIcons(*m_menuFile);
    fixMenuIcons(*m_menuActions);
    fixMenuIcons(*m_menuTools);
    fixMenuIcons(*m_menuHelp);

    //create language selection menu
    for (const TranslationInfo& ti : getExistingTranslations())
    {
        wxMenuItem* newItem = new wxMenuItem(m_menuLanguages, wxID_ANY, ti.languageName);
        newItem->SetBitmap(loadImage(ti.languageFlag)); //GTK: set *before* inserting into menu

        m_menuLanguages->Bind(wxEVT_COMMAND_MENU_SELECTED, [this, langId = ti.languageID](wxCommandEvent&) { switchProgramLanguage(langId); }, newItem->GetId());
        m_menuLanguages->Append(newItem); //pass ownership
    }

    //set up layout items to toggle showing hidden panels
    m_menuItemShowMain      ->SetItemLabel(replaceCpy(_("Show \"%x\""), L"%x", _("Main Bar")));
    m_menuItemShowFolders   ->SetItemLabel(replaceCpy(_("Show \"%x\""), L"%x", _("Folder Pairs")));
    m_menuItemShowViewFilter->SetItemLabel(replaceCpy(_("Show \"%x\""), L"%x", _("View Settings")));
    m_menuItemShowConfig    ->SetItemLabel(replaceCpy(_("Show \"%x\""), L"%x", _("Configuration")));
    m_menuItemShowOverview  ->SetItemLabel(replaceCpy(_("Show \"%x\""), L"%x", _("Overview")));

    auto setupLayoutMenuEvent = [&](wxMenuItem* menuItem, wxWindow* panelWindow)
    {
        m_menuTools->Bind(wxEVT_COMMAND_MENU_SELECTED, [this, panelWindow](wxCommandEvent&)
        {
            wxAuiPaneInfo& paneInfo = this->auiMgr_.GetPane(panelWindow);
            paneInfo.Show();
            this->auiMgr_.Update();
        }, menuItem->GetId());

        //"hide" menu items by default
        detachedMenuItems_.insert(m_menuTools->Remove(menuItem)); //pass ownership
    };
    setupLayoutMenuEvent(m_menuItemShowMain,       m_panelTopButtons);
    setupLayoutMenuEvent(m_menuItemShowFolders,    m_panelDirectoryPairs);
    setupLayoutMenuEvent(m_menuItemShowViewFilter, m_panelViewFilter);
    setupLayoutMenuEvent(m_menuItemShowConfig,     m_panelConfig);
    setupLayoutMenuEvent(m_menuItemShowOverview,   m_gridOverview);

    m_menuTools->Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) { onOpenMenuTools(event); });


    //notify about (logical) application main window => program won't quit, but stay on this dialog
    setGlobalWindow(this);

    //init handling of first folder pair
    firstFolderPair_ = std::make_unique<FolderPairFirst>(*this,
                                                         globalCfg_.mainDlg.folderLastSelectedLeft,
                                                         globalCfg_.mainDlg.folderLastSelectedRight,
                                                         globalCfg_.sftpKeyFileLastSelected);

    //init grid settings
    filegrid::init(*m_gridMainL, *m_gridMainC, *m_gridMainR);
    treegrid::init(*m_gridOverview);
    cfggrid ::init(*m_gridCfgHistory);

    //initialize and load configuration
    setGlobalCfgOnInit(globalSettings); //calls auiMgr_.Update()
    setConfig(guiCfg, referenceFiles); //expects auiMgr_.Update(): e.g. recalcMaxFolderPairsVisible()

    //support for CTRL + C and DEL on grids
    m_gridMainL->getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onGridKeyEvent(event, *m_gridMainL,  true /*leftSide*/); });
    m_gridMainC->getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onGridKeyEvent(event, *m_gridMainC,  true /*leftSide*/); });
    m_gridMainR->getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onGridKeyEvent(event, *m_gridMainR, false /*leftSide*/); });

    m_gridOverview->getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onTreeKeyEvent(event); });

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onLocalKeyEvent(event); }); //enable dialog-specific key events

    //drag and drop .ffs_gui and .ffs_batch on main dialog
    setupFileDrop(*this);
    Bind(EVENT_DROP_FILE, [this](FileDropEvent& event) { onDialogFilesDropped(event); });

    //calculate witdh of folder pair manually (if scrollbars are visible)
    m_panelTopLeft->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { onResizeLeftFolderWidth(event); });

    m_panelTopLeft  ->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onTopFolderPairKeyEvent(event); });
    m_panelTopCenter->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onTopFolderPairKeyEvent(event); });
    m_panelTopRight ->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onTopFolderPairKeyEvent(event); });

    //dynamically change sizer direction depending on size
    m_panelTopButtons->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { onResizeTopButtonPanel(event); });
    m_panelConfig    ->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { onResizeConfigPanel   (event); });
    m_panelViewFilter->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { onResizeViewPanel     (event); });
    wxSizeEvent dummy3;
    onResizeTopButtonPanel(dummy3); //
    onResizeConfigPanel   (dummy3); //call once on window creation
    onResizeViewPanel     (dummy3); //

    const int scrollDelta = m_buttonSelectFolderLeft->GetSize().y; //more approriate than GetCharHeight() here
    m_scrolledWindowFolderPairs->SetScrollRate(scrollDelta, scrollDelta);

    //event handler for manual (un-)checking of rows and setting of sync direction
    m_gridMainC->Bind(EVENT_GRID_CHECK_ROWS,     [this](CheckRowsEvent&     event) { onCheckRows       (event); });
    m_gridMainC->Bind(EVENT_GRID_SYNC_DIRECTION, [this](SyncDirectionEvent& event) { onSetSyncDirection(event); });

    //mainly to update row label sizes...
    updateGui();

    //register regular check for update on next idle event
    Bind(wxEVT_IDLE, &MainDialog::onStartupUpdateCheck, this);

    //asynchronous call to wxWindow::Layout(): fix superfluous frame on right and bottom when FFS is started in fullscreen mode
    Bind(wxEVT_IDLE, &MainDialog::onLayoutWindowAsync, this);
    wxCommandEvent evtDummy;           //call once before onLayoutWindowAsync()
    onResizeLeftFolderWidth(evtDummy); //

    //scroll cfg history to last used position. We cannot do this earlier e.g. in setGlobalCfgOnInit()
    //1. setConfig() indirectly calls cfggrid::addAndSelect() which changes cfg history scroll position
    //2. Grid::makeRowVisible() requires final window height! => do this after window resizing is complete
    if (m_gridCfgHistory->getRowCount() > 0)
        m_gridCfgHistory->scrollTo(std::clamp<size_t>(globalSettings.mainDlg.cfgGridTopRowPos, //must be set *after* wxAuiManager::LoadPerspective() to have any effect
                                                      0,  m_gridCfgHistory->getRowCount() - 1));

    //first selected item should always be visible:
    const std::vector<size_t> selectedRows = m_gridCfgHistory->getSelectedRows();
    if (!selectedRows.empty())
        m_gridCfgHistory->makeRowVisible(selectedRows.front());


    m_buttonCompare->SetFocus();

    //----------------------------------------------------------------------------------------------------------------------------------------------------------------
    //some convenience: if FFS is started with a *.ffs_gui file as commandline parameter AND all directories contained exist, comparison shall be started right away
    if (startComparison)
    {
        const MainConfiguration currMainCfg = getConfig().mainCfg;

        //------------------------------------------------------------------------------------------
        //harmonize checks with comparison.cpp:: checkForIncompleteInput()
        //we're really doing two checks: 1. check directory existence 2. check config validity -> don't mix them!
        bool havePartialPair = false;
        bool haveFullPair    = false;

        std::vector<AbstractPath> folderPathsToCheck;

        auto addFolderCheck = [&](const LocalPairConfig& lpc)
        {
            const AbstractPath folderPathL = createAbstractPath(lpc.folderPathPhraseLeft);
            const AbstractPath folderPathR = createAbstractPath(lpc.folderPathPhraseRight);

            if (AFS::isNullPath(folderPathL) != AFS::isNullPath(folderPathR)) //only skip check if both sides are empty!
                havePartialPair = true;
            else if (!AFS::isNullPath(folderPathL))
                haveFullPair = true;

            if (!AFS::isNullPath(folderPathL))
                folderPathsToCheck.push_back(folderPathL); //noexcept
            if (!AFS::isNullPath(folderPathR))
                folderPathsToCheck.push_back(folderPathR); //noexcept
        };

        addFolderCheck(currMainCfg.firstPair);
        for (const LocalPairConfig& lpc : currMainCfg.additionalPairs)
            addFolderCheck(lpc);
        //------------------------------------------------------------------------------------------

        if (havePartialPair != haveFullPair) //either all pairs full or all half-filled -> validity check!
        {
            //check existence of all directories in parallel!
            AsyncFirstResult<std::false_type> firstMissingDir;
            for (const AbstractPath& folderPath : folderPathsToCheck)
                firstMissingDir.addJob([folderPath]() -> std::optional<std::false_type>
            {
                try
                {
                    if (AFS::getItemType(folderPath) != AFS::ItemType::file) //throw FileError
                        return {};
                }
                catch (FileError&) {}
                return std::false_type();
            });

            const bool startComparisonNow = !firstMissingDir.timedWait(std::chrono::milliseconds(500)) || //= no result yet => start comparison anyway!
                                            !firstMissingDir.get(); //= all directories exist

            if (startComparisonNow)
            {
                wxCommandEvent dummy2(wxEVT_COMMAND_BUTTON_CLICKED);
                //better!? => m_buttonCompare->Command(dummy2); //simulate click
                if (wxEvtHandler* evtHandler = m_buttonCompare->GetEventHandler())
                    evtHandler->AddPendingEvent(dummy2); //simulate button click on "compare"
            }
        }
    }
}


MainDialog::~MainDialog()
{
    std::optional<FileError> firstError;
    try //save "LastRun.ffs_gui"
    {
        writeConfig(getConfig(), lastRunConfigPath_); //throw FileError
    }
    catch (const FileError& e) { if (!firstError) firstError = e; }

    try //save "GlobalSettings.xml"
    {
        writeConfig(getGlobalCfgBeforeExit(), globalConfigFilePath_); //throw FileError
    }
    catch (const FileError& e) { if (!firstError) firstError = e; }

    //don't annoy users on read-only drives: it's enough to show a single error message
    if (firstError)
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(firstError->toString()));

    //this is pathetic: macOS 11.0 Big Sur crashes (for floating panels) unless we detach: https://freefilesync.org/forum/viewtopic.php?t=7939
    //  => wxBug: https://trac.wxwidgets.org/ticket/18991
    std::vector<wxWindow*> panesToDetach; //careful: wxAuiManager::DetachPane() changes paneArray!
    {
        wxAuiPaneInfoArray& paneArray = auiMgr_.GetAllPanes();
        for (size_t i = 0; i < paneArray.size(); ++i)
            panesToDetach.push_back(paneArray[i].window);
    }
    for (wxWindow* win : panesToDetach)
        if (!auiMgr_.DetachPane(win))
            assert(false);
    assert(utcToTimeT(getCompileTime()) < utcToTimeT(parseTime("%Y-%m-%d", "2021-03-01"))); //check again if bug is fixed in March

    //auiMgr_.UnInit(); - "since wxWidgets 3.1.4 [...] it will be called automatically when this window is destroyed, as well as when the manager itself is."

    for (wxMenuItem* item : detachedMenuItems_)
        delete item; //something's got to give

    //no need for wxEventHandler::Unbind(): event sources are components of this window and are destroyed, too
}

//-------------------------------------------------------------------------------------------------------------------------------------

void MainDialog::onQueryEndSession()
{
    //we try our best to do something useful in this extreme situation - no reason to notify or even log errors here!
    try { writeConfig(getGlobalCfgBeforeExit(), globalConfigFilePath_); }
    catch (FileError&) {}

    try { writeConfig(getConfig(), lastRunConfigPath_); }
    catch (FileError&) {}
}


void MainDialog::onClose(wxCloseEvent& event)
{
    //attention: system shutdown is handled in onQueryEndSession()!

    //regular destruction handling
    if (event.CanVeto())
    {
        //=> veto all attempts to close the main window while comparison or synchronization are running:
        if (operationInProgress_)
        {
            event.Veto();
            Raise();    //=what Windows does when vetoing a close (via middle mouse on taskbar preview) while showing a modal dialog
            SetFocus(); //
            return;
        }

        const bool cancelled = !saveOldConfig(); //notify user about changed settings
        if (cancelled)
        {
            event.Veto();
            return;
        }
    }

    Destroy();
}


void MainDialog::setGlobalCfgOnInit(const XmlGlobalSettings& globalSettings)
{
    globalCfg_ = globalSettings;

    //caveat: set/get language asymmmetry! setLanguage(globalSettings.programLanguage); //throw FileError
    //we need to set language before creating this class!

    setInitialWindowSize(*this,
                         globalSettings.mainDlg.dlgSize,
                         globalSettings.mainDlg.dlgPos,
                         globalSettings.mainDlg.isMaximized,
                         wxSize{fastFromDIP(900), fastFromDIP(600)} /*defaultSize*/);

    //set column attributes
    m_gridMainL   ->setColumnConfig(convertColAttributes(globalSettings.mainDlg.columnAttribLeft,  getFileGridDefaultColAttribsLeft()));
    m_gridMainR   ->setColumnConfig(convertColAttributes(globalSettings.mainDlg.columnAttribRight, getFileGridDefaultColAttribsLeft()));
    m_splitterMain->setSashOffset(globalSettings.mainDlg.sashOffset);

    m_gridOverview->setColumnConfig(convertColAttributes(globalSettings.mainDlg.treeGridColumnAttribs, getTreeGridDefaultColAttribs()));
    treegrid::setShowPercentage(*m_gridOverview, globalSettings.mainDlg.treeGridShowPercentBar);

    treegrid::getDataView(*m_gridOverview).setSortDirection(globalSettings.mainDlg.treeGridLastSortColumn, globalSettings.mainDlg.treeGridLastSortAscending);

    //--------------------------------------------------------------------------------
    //load list of configuration files
    cfggrid::getDataView(*m_gridCfgHistory).set(globalSettings.mainDlg.cfgFileHistory);

    //globalSettings.mainDlg.cfgGridTopRowPos => defer evaluation until later within MainDialog constructor
    m_gridCfgHistory->setColumnConfig(convertColAttributes(globalSettings.mainDlg.cfgGridColumnAttribs, getCfgGridDefaultColAttribs()));
    cfggrid::getDataView(*m_gridCfgHistory).setSortDirection(globalSettings.mainDlg.cfgGridLastSortColumn, globalSettings.mainDlg.cfgGridLastSortAscending);
    cfggrid::setSyncOverdueDays(*m_gridCfgHistory, globalSettings.mainDlg.cfgGridSyncOverdueDays);
    //m_gridCfgHistory->Refresh(); <- implicit in last call

    //remove non-existent items (we need this only on startup)
    std::vector<Zstring> cfgFilePaths;
    for (const ConfigFileItem& item : globalSettings.mainDlg.cfgFileHistory)
        cfgFilePaths.push_back(item.cfgFilePath);

    cfgHistoryRemoveObsolete(cfgFilePaths);
    //--------------------------------------------------------------------------------

    //load list of last used folders
    m_folderPathLeft ->setHistory(folderHistoryLeft_);
    m_folderPathRight->setHistory(folderHistoryRight_);

    //show/hide file icons
    filegrid::setupIcons(*m_gridMainL, *m_gridMainC, *m_gridMainR, globalSettings.mainDlg.showIcons, convert(globalSettings.mainDlg.iconSize));

    filegrid::setItemPathForm(*m_gridMainL, globalSettings.mainDlg.itemPathFormatLeftGrid);
    filegrid::setItemPathForm(*m_gridMainR, globalSettings.mainDlg.itemPathFormatRightGrid);

    //--------------------------------------------------------------------------------
    m_checkBoxMatchCase->SetValue(globalCfg_.mainDlg.textSearchRespectCase);

    //wxAuiManager erroneously loads panel captions, we don't want that
    std::vector<std::pair<wxAuiPaneInfo*, wxString>> paneCaptions;
    wxAuiPaneInfoArray& paneArray = auiMgr_.GetAllPanes();
    for (size_t i = 0; i < paneArray.size(); ++i)
        paneCaptions.emplace_back(&paneArray[i], paneArray[i].caption);

    //compare progress dialog minimum sizes are layout-dependent + can't be changed by user => don't load stale values from config
    std::vector<std::tuple<wxAuiPaneInfo*, wxSize /*min size*/, wxSize /*best size*/>> paneConstraints;
    auto preserveConstraint = [&paneConstraints](wxAuiPaneInfo& pane) { paneConstraints.emplace_back(&pane, pane.min_size, pane.best_size); };

    wxAuiPaneInfo& progPane = auiMgr_.GetPane(compareStatus_->getAsWindow());
    preserveConstraint(auiMgr_.GetPane(m_panelTopButtons));
    preserveConstraint(progPane);
    preserveConstraint(auiMgr_.GetPane(m_panelSearch));
    preserveConstraint(auiMgr_.GetPane(m_panelViewFilter));
    preserveConstraint(auiMgr_.GetPane(m_panelConfig));

    auiMgr_.LoadPerspective(globalSettings.mainDlg.guiPerspectiveLast, false /*update: don't call wxAuiManager::Update() yet*/);

    //restore original captions
    for (const auto& [paneInfo, caption] : paneCaptions)
        paneInfo->Caption(caption);

    //restore pane layout constraints
    for (auto& [pane, minSize, bestSize] : paneConstraints)
    {
        pane->min_size  = minSize;
        pane->best_size = bestSize;
    }
    //--------------------------------------------------------------------------------

    //if MainDialog::onQueryEndSession() is called while comparison is active, this panel is saved and restored as "visible"
    progPane.Hide();

    auiMgr_.GetPane(m_panelSearch).Hide(); //no need to show it on startup
    auiMgr_.GetPane(m_panelLog   ).Hide(); //

    m_menuItemCheckVersionAuto->Check(updateCheckActive(globalCfg_.lastUpdateCheck));

    auiMgr_.Update();
}


XmlGlobalSettings MainDialog::getGlobalCfgBeforeExit()
{
    Freeze(); //no need to Thaw() again!!
    recalcMaxFolderPairsVisible();
    //--------------------------------------------------------------------------------
    XmlGlobalSettings globalSettings = globalCfg_;

    globalSettings.programLanguage = getLanguage();

    //retrieve column attributes
    globalSettings.mainDlg.columnAttribLeft  = convertColAttributes<ColAttributesRim>(m_gridMainL->getColumnConfig());
    globalSettings.mainDlg.columnAttribRight = convertColAttributes<ColAttributesRim>(m_gridMainR->getColumnConfig());
    globalSettings.mainDlg.sashOffset        = m_splitterMain->getSashOffset();

    globalSettings.mainDlg.treeGridColumnAttribs  = convertColAttributes<ColAttributesTree>(m_gridOverview->getColumnConfig());
    globalSettings.mainDlg.treeGridShowPercentBar = treegrid::getShowPercentage(*m_gridOverview);

    const auto [sortCol, ascending] = treegrid::getDataView(*m_gridOverview).getSortConfig();
    globalSettings.mainDlg.treeGridLastSortColumn    = sortCol;
    globalSettings.mainDlg.treeGridLastSortAscending = ascending;

    //--------------------------------------------------------------------------------
    //write list of configuration files
    std::vector<ConfigFileItem> cfgHistory
    {
        //make sure [Last session] is always part of history list
        ConfigFileItem(lastRunConfigPath_, 0 /*syncTime*/, getNullPath() /*logPath*/, SyncResult::aborted,
                       wxColor(0xdd, 0xdd, 0xdd) /* light grey from onCfgGridContext()*/)
    };

    for (const ConfigFileItem& item : cfggrid::getDataView(*m_gridCfgHistory).get())
        if (equalNativePath(item.cfgFilePath, lastRunConfigPath_))
            cfgHistory[0] = item; //preserve users's background color choice
        else
            cfgHistory.push_back(item);

    //trim excess elements (oldest first)
    if (cfgHistory.size() > globalSettings.mainDlg.cfgHistItemsMax)
        cfgHistory.resize(globalSettings.mainDlg.cfgHistItemsMax);

    globalSettings.mainDlg.cfgFileHistory         = std::move(cfgHistory);
    globalSettings.mainDlg.cfgGridTopRowPos       = m_gridCfgHistory->getTopRow();
    globalSettings.mainDlg.cfgGridColumnAttribs   = convertColAttributes<ColAttributesCfg>(m_gridCfgHistory->getColumnConfig());
    globalSettings.mainDlg.cfgGridSyncOverdueDays = cfggrid::getSyncOverdueDays(*m_gridCfgHistory);

    std::tie(globalSettings.mainDlg.cfgGridLastSortColumn,
             globalSettings.mainDlg.cfgGridLastSortAscending) = cfggrid::getDataView(*m_gridCfgHistory).getSortDirection();
    //--------------------------------------------------------------------------------
    globalSettings.mainDlg.cfgFilesLastUsed = activeConfigFiles_;

    //write list of last used folders
    globalSettings.mainDlg.folderHistoryLeft  = folderHistoryLeft_ ->getList();
    globalSettings.mainDlg.folderHistoryRight = folderHistoryRight_->getList();

    globalSettings.mainDlg.textSearchRespectCase = m_checkBoxMatchCase->GetValue();

    wxAuiPaneInfo& logPane = auiMgr_.GetPane(m_panelLog);

    if (logPane.IsShown())
    {
        if (logPane.IsMaximized())
            auiMgr_.RestorePane(logPane); //!= wxAuiPaneInfo::Restore() which does not un-hide other panels (WTF!?)
        else //ensure current window sizes will be used when pane is shown again:
            logPane.best_size = logPane.rect.GetSize();
    }
    //else: logPane.best_size already contains non-maximized value

    //auiMgr_.Update(); //[!] not needed
    globalSettings.mainDlg.guiPerspectiveLast = auiMgr_.SavePerspective(); //does not need wxAuiManager::Update()!

    const auto& [size, pos, isMaximized] = getWindowSizeBeforeClose(*this); //call *after* wxAuiManager::SavePerspective()!
    if (size)
        globalSettings.mainDlg.dlgSize = *size;
    if (pos)
        globalSettings.mainDlg.dlgPos = *pos;
    globalSettings.mainDlg.isMaximized = isMaximized;

    return globalSettings;
}


namespace
{
//user expectations for partial sync:
// 1. selected folder implies also processing child items
// 2. to-be-moved item requires also processing target item
std::vector<FileSystemObject*> expandSelectionForPartialSync(const std::vector<FileSystemObject*>& selection)
{
    std::vector<FileSystemObject*> output;

    for (FileSystemObject* fsObj : selection)
        recursiveObjectVisitor(*fsObj, [&](FolderPair& folder) { output.push_back(&folder); },
    [&](FilePair& file)
    {
        output.push_back(&file);
        switch (file.getSyncOperation()) //evaluate comparison result and sync direction
        {
            case SO_MOVE_LEFT_FROM:
            case SO_MOVE_LEFT_TO:
            case SO_MOVE_RIGHT_FROM:
            case SO_MOVE_RIGHT_TO:
                if (FileSystemObject* moveRefObj = FileSystemObject::retrieve(file.getMoveRef()))
                    output.push_back(moveRefObj);
                assert(dynamic_cast<FilePair*>(output.back())->getMoveRef() == file.getId());
                break;

            case SO_CREATE_NEW_LEFT:
            case SO_CREATE_NEW_RIGHT:
            case SO_DELETE_LEFT:
            case SO_DELETE_RIGHT:
            case SO_OVERWRITE_LEFT:
            case SO_OVERWRITE_RIGHT:
            case SO_COPY_METADATA_TO_LEFT:
            case SO_COPY_METADATA_TO_RIGHT:
            case SO_UNRESOLVED_CONFLICT:
            case SO_DO_NOTHING:
            case SO_EQUAL:
                break;
        }
    },
    [&](SymlinkPair& symlink) { output.push_back(&symlink); });

    removeDuplicates(output);
    return output;
}


bool selectionIncludesNonEqualItem(const std::vector<FileSystemObject*>& selection)
{
    struct ItemFound {};
    try
    {
        for (FileSystemObject* fsObj : selection)
            recursiveObjectVisitor(*fsObj,
            [](FolderPair&   folder) { if (folder .getSyncOperation() != SO_EQUAL) throw ItemFound(); },
        /**/[](FilePair&       file) { if (file   .getSyncOperation() != SO_EQUAL) throw ItemFound(); },
        /**/[](SymlinkPair& symlink) { if (symlink.getSyncOperation() != SO_EQUAL) throw ItemFound(); });
        return false;
    }
    catch (ItemFound&) { return true;}
}
}


void MainDialog::setSyncDirManually(const std::vector<FileSystemObject*>& selection, SyncDirection direction)
{
    if (!selectionIncludesNonEqualItem(selection))
        return; //harmonize with onGridContextRim(): this function should be a no-op iff context menu option is disabled!

    for (FileSystemObject* fsObj : selection)
    {
        setSyncDirectionRec(direction, *fsObj); //set new direction (recursively)
        setActiveStatus(true, *fsObj); //works recursively for directories
    }
    updateGui();
}


void MainDialog::setIncludedManually(const std::vector<FileSystemObject*>& selection, bool setActive)
{
    //if hidefiltered is active, there should be no filtered elements on screen => current element was filtered out
    assert(m_bpButtonShowExcluded->isActive() || !setActive);

    if (selection.empty())
        return; //harmonize with onGridContextRim(): this function should be a no-op iff context menu option is disabled!

    for (FileSystemObject* fsObj : selection)
        setActiveStatus(setActive, *fsObj); //works recursively for directories

    updateGuiDelayedIf(!m_bpButtonShowExcluded->isActive()); //show update GUI before removing rows
}


void MainDialog::copySelectionToClipboard(const std::vector<const Grid*>& gridRefs)
{
    try
    {
        //perf: wxString doesn't model exponential growth and is unsuitable for large data sets
        std::wstring clipBuf;

        for (const Grid* grid : gridRefs)
            if (auto prov = grid->getDataProvider())
            {
                std::vector<Grid::ColAttributes> colAttr = grid->getColumnConfig();
                std::erase_if(colAttr, [](const Grid::ColAttributes& ca) { return !ca.visible; });
                if (!colAttr.empty())
                    for (size_t row : grid->getSelectedRows())
                    {
                        std::for_each(colAttr.begin(), colAttr.end() - 1, [&](const Grid::ColAttributes& ca)
                        {
                            clipBuf += prov->getValue(row, ca.type);
                            clipBuf += L'\t';
                        });
                        clipBuf += prov->getValue(row, colAttr.back().type);
                        clipBuf += L'\n';
                    }
            }

        if (!clipBuf.empty())
            if (wxClipboard::Get()->Open())
            {
                ZEN_ON_SCOPE_EXIT(wxClipboard::Get()->Close());
                wxClipboard::Get()->SetData(new wxTextDataObject(std::move(clipBuf))); //ownership passed
            }
    }
    catch (const std::bad_alloc& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setMainInstructions(_("Out of memory.") + L' ' + utfTo<std::wstring>(e.what())));
    }
}


std::vector<FileSystemObject*> MainDialog::getGridSelection(bool fromLeft, bool fromRight) const
{
    std::vector<size_t> selectedRows;

    if (fromLeft)
        append(selectedRows, m_gridMainL->getSelectedRows());

    if (fromRight)
        append(selectedRows, m_gridMainR->getSelectedRows());

    removeDuplicates(selectedRows);
    assert(std::is_sorted(selectedRows.begin(), selectedRows.end()));

    return filegrid::getDataView(*m_gridMainC).getAllFileRef(selectedRows);
}


std::vector<FileSystemObject*> MainDialog::getTreeSelection() const
{
    std::vector<FileSystemObject*> output;

    for (size_t row : m_gridOverview->getSelectedRows())
        if (std::unique_ptr<TreeView::Node> node = treegrid::getDataView(*m_gridOverview).getLine(row))
        {
            if (auto root = dynamic_cast<const TreeView::RootNode*>(node.get()))
            {
                //selecting root means "select everything", *ignoring* current view filter!
                for (FileSystemObject& fsObj : root->baseFolder.refSubFolders()) //no need to explicitly add child elements!
                    output.push_back(&fsObj);
                for (FileSystemObject& fsObj : root->baseFolder.refSubFiles())
                    output.push_back(&fsObj);
                for (FileSystemObject& fsObj : root->baseFolder.refSubLinks())
                    output.push_back(&fsObj);
            }
            else if (auto dir = dynamic_cast<const TreeView::DirNode*>(node.get()))
                output.push_back(&(dir->folder));
            else if (auto file = dynamic_cast<const TreeView::FilesNode*>(node.get()))
                append(output, file->filesAndLinks);
            else assert(false);
        }
    return output;
}


void MainDialog::copyToAlternateFolder(const std::vector<FileSystemObject*>& selectionLeft,
                                       const std::vector<FileSystemObject*>& selectionRight)
{
    if (std::all_of(selectionLeft .begin(), selectionLeft .end(), [](const FileSystemObject* fsObj) { return fsObj->isEmpty< LEFT_SIDE>(); }) &&
    /**/std::all_of(selectionRight.begin(), selectionRight.end(), [](const FileSystemObject* fsObj) { return fsObj->isEmpty<RIGHT_SIDE>(); }))
    /**/return; //harmonize with onGridContextRim(): this function should be a no-op iff context menu option is disabled!

    FocusPreserver fp;

    if (showCopyToDialog(this,
                         selectionLeft, selectionRight,
                         globalCfg_.mainDlg.copyToCfg.targetFolderPath,
                         globalCfg_.mainDlg.copyToCfg.targetFolderLastSelected,
                         globalCfg_.mainDlg.copyToCfg.folderHistory, globalCfg_.folderHistoryMax,
                         globalCfg_.sftpKeyFileLastSelected,
                         globalCfg_.mainDlg.copyToCfg.keepRelPaths,
                         globalCfg_.mainDlg.copyToCfg.overwriteIfExists) != ConfirmationButton::accept)
        return;

    disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
    auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
    ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

    const auto& guiCfg = getConfig();

    StatusHandlerTemporaryPanel statusHandler(*this, std::chrono::system_clock::now() /*startTime*/,
                                              false /*ignoreErrors*/,
                                              guiCfg.mainCfg.autoRetryCount,
                                              guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
    try
    {
        fff::copyToAlternateFolder(selectionLeft, selectionRight,
                                   globalCfg_.mainDlg.copyToCfg.targetFolderPath,
                                   globalCfg_.mainDlg.copyToCfg.keepRelPaths,
                                   globalCfg_.mainDlg.copyToCfg.overwriteIfExists,
                                   globalCfg_.warnDlgs,
                                   statusHandler); //throw AbortProcess

        //"clearSelection" not needed/desired
    }
    catch (AbortProcess&) {}

    const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
    setLastOperationLog(r.summary, r.errorLog);

    //updateGui(); -> not needed
}


void MainDialog::deleteSelectedFiles(const std::vector<FileSystemObject*>& selectionLeft,
                                     const std::vector<FileSystemObject*>& selectionRight, bool moveToRecycler)
{
    if (std::all_of(selectionLeft .begin(), selectionLeft .end(), [](const FileSystemObject* fsObj) { return fsObj->isEmpty< LEFT_SIDE>(); }) &&
    /**/std::all_of(selectionRight.begin(), selectionRight.end(), [](const FileSystemObject* fsObj) { return fsObj->isEmpty<RIGHT_SIDE>(); }))
    /**/return; //harmonize with onGridContextRim(): this function should be a no-op iff context menu option is disabled!

    FocusPreserver fp;

    const auto& [itemList, itemCount] = getSelectedItemsAsString(selectionLeft, selectionRight);

    if (showDeleteDialog(this, itemList, itemCount,
                         moveToRecycler) != ConfirmationButton::accept)
        return;

    disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
    auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
    ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

    //wxBusyCursor dummy; -> redundant: progress already shown in status bar!
    const auto& guiCfg = getConfig();

    StatusHandlerTemporaryPanel statusHandler(*this, std::chrono::system_clock::now() /*startTime*/,
                                              false /*ignoreErrors*/,
                                              guiCfg.mainCfg.autoRetryCount,
                                              guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
    try
    {
        deleteFromGridAndHD(selectionLeft, selectionRight,
                            extractDirectionCfg(folderCmp_, getConfig().mainCfg),
                            moveToRecycler,
                            globalCfg_.warnDlgs.warnRecyclerMissing,
                            statusHandler); //throw AbortProcess
    }
    catch (AbortProcess&) {}

    const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
    setLastOperationLog(r.summary, r.errorLog);

    //remove rows that are empty: just a beautification, invalid rows shouldn't cause issues
    filegrid::getDataView(*m_gridMainC).removeInvalidRows();

    updateGui();
}


namespace
{
template <SelectedSide side>
AbstractPath getExistingParentFolder(const FileSystemObject& fsObj)
{
    auto folder = dynamic_cast<const FolderPair*>(&fsObj);
    if (!folder)
        folder = dynamic_cast<const FolderPair*>(&fsObj.parent());

    while (folder)
    {
        if (!folder->isEmpty<side>())
            return folder->getAbstractPath<side>();

        folder = dynamic_cast<const FolderPair*>(&folder->parent());
    }
    return fsObj.base().getAbstractPath<side>();
}


template <SelectedSide side, class Function>
void extractFileDescriptor(const FileSystemObject& fsObj, Function onDescriptor)
{
    if (!fsObj.isEmpty<side>())
        visitFSObject(fsObj, [](const FolderPair& folder) {},
    [&](const FilePair& file)
    {
        const FileDescriptor descr = { file.getAbstractPath<side>(), file.getAttributes<side>() };
        onDescriptor(descr);
    },
    [](const SymlinkPair& symlink) {});
}


template <SelectedSide side>
void collectNonNativeFiles(const std::vector<FileSystemObject*>& selectedRows, const TempFileBuffer& tempFileBuf,
                           std::set<FileDescriptor>& workLoad)
{
    for (const FileSystemObject* fsObj : selectedRows)
        extractFileDescriptor<side>(*fsObj, [&](const FileDescriptor& descr)
    {
        if (!AFS::getNativeItemPath(descr.path))
            if (tempFileBuf.getTempPath(descr).empty()) //TempFileBuffer::createTempFiles() contract!
                workLoad.insert(descr);
    });
}


template <SelectedSide side>
void invokeCommandLine(const Zstring& commandLinePhrase, //throw FileError
                       bool openWithDefaultAppRequested,
                       const std::vector<FileSystemObject*>& selection,
                       const TempFileBuffer& tempFileBuf)
{
    constexpr SelectedSide side2 = OtherSide<side>::value;

    for (const FileSystemObject* fsObj : selection) //context menu calls this function only if selection is not empty!
    {
        const AbstractPath basePath  = fsObj->base().getAbstractPath<side >();
        const AbstractPath basePath2 = fsObj->base().getAbstractPath<side2>();

        //return paths, even if item is not (yet) existing:
        const Zstring itemPath    = AFS::isNullPath(basePath ) ? Zstr("") : utfTo<Zstring>(AFS::getDisplayPath(fsObj->         getAbstractPath<side >()));
        const Zstring itemPath2   = AFS::isNullPath(basePath2) ? Zstr("") : utfTo<Zstring>(AFS::getDisplayPath(fsObj->         getAbstractPath<side2>()));
        const Zstring itemName    = AFS::isNullPath(basePath ) ? Zstr("") :                AFS::getItemName   (fsObj->         getAbstractPath<side >());
        const Zstring itemName2   = AFS::isNullPath(basePath2) ? Zstr("") :                AFS::getItemName   (fsObj->         getAbstractPath<side2>());
        const Zstring folderPath  = AFS::isNullPath(basePath ) ? Zstr("") : utfTo<Zstring>(AFS::getDisplayPath(fsObj->parent().getAbstractPath<side >()));
        const Zstring folderPath2 = AFS::isNullPath(basePath2) ? Zstr("") : utfTo<Zstring>(AFS::getDisplayPath(fsObj->parent().getAbstractPath<side2>()));

        Zstring localPath;
        Zstring localPath2;

        if (AFS::getNativeItemPath(basePath))
            localPath = itemPath; //no matter if item exists or not
        else //returns empty if not available (item not existing, error during copy):
            extractFileDescriptor<side>(*fsObj, [&](const FileDescriptor& descr) { localPath = tempFileBuf.getTempPath(descr); });

        if (AFS::getNativeItemPath(basePath2))
            localPath2 = itemPath2;
        else
            extractFileDescriptor<side2>(*fsObj, [&](const FileDescriptor& descr) { localPath2 = tempFileBuf.getTempPath(descr); });

        if (localPath .empty()) localPath  = replaceCpy(utfTo<Zstring>(L"<" + _("Local path not available for %x.") + L">"), Zstr("%x"), itemPath );
        if (localPath2.empty()) localPath2 = replaceCpy(utfTo<Zstring>(L"<" + _("Local path not available for %x.") + L">"), Zstr("%x"), itemPath2);

        Zstring cmdLine = expandMacros(commandLinePhrase);
        replace(cmdLine, Zstr("%item_path%"),    itemPath);
        replace(cmdLine, Zstr("%item_path2%"),   itemPath2);
        replace(cmdLine, Zstr("%local_path%"),   localPath);
        replace(cmdLine, Zstr("%local_path2%"),  localPath2);
        replace(cmdLine, Zstr("%item_name%"),    itemName);
        replace(cmdLine, Zstr("%item_name2%"),   itemName2);
        replace(cmdLine, Zstr("%parent_path%"),  folderPath);
        replace(cmdLine, Zstr("%parent_path2%"), folderPath2);

        if (openWithDefaultAppRequested) //not strictly needed, but: 1. better error reporting (Windows) 2. not async => avoid zombies (Linux/macOS)
            openWithDefaultApp(localPath); //throw FileError
        else
            try
            {
                std::optional<int> timeoutMs;
                if (selection.size() <= EXT_APP_MASS_INVOKE_THRESHOLD)
                    timeoutMs = EXT_APP_MAX_TOTAL_WAIT_TIME_MS / selection.size(); //run async, but give consoleExecute() some "time to fail"
                //else: run synchronously

                if (const auto& [exitCode, output] = consoleExecute(cmdLine, timeoutMs); //throw SysError, SysErrorTimeOut
                    exitCode != 0)
                    throw SysError(formatSystemError(utfTo<std::string>(commandLinePhrase), replaceCpy(_("Exit code %x"), L"%x", numberTo<std::wstring>(exitCode)), output));
            }
            catch (SysErrorTimeOut&) {} //child process not failed yet => probably fine :>
            catch (const SysError& e) { throw FileError(replaceCpy(_("Command %x failed."), L"%x", fmtPath(cmdLine)), e.toString()); }
    }
}
}


void MainDialog::openExternalApplication(const Zstring& commandLinePhrase, bool leftSide,
                                         const std::vector<FileSystemObject*>& selectionLeft,
                                         const std::vector<FileSystemObject*>& selectionRight)
{
    const XmlGlobalSettings defaultCfg;
    assert(defaultCfg.externalApps.size() >= 2);
    const bool showInFileBrowserRequested  = commandLinePhrase == defaultCfg.externalApps[0].cmdLine;
    const bool openWithDefaultAppRequested = commandLinePhrase == defaultCfg.externalApps[1].cmdLine;

    auto openFolderInFileBrowser = [this](const AbstractPath& folderPath)
    {
        try
        {
                openWithDefaultApp(utfTo<Zstring>(AFS::getDisplayPath(folderPath))); //throw FileError
        }
        catch (const FileError& e) { showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString())); }
    };

    //support fallback instead of an error in this special case
    if (showInFileBrowserRequested)
    {
        if (selectionLeft.size() + selectionRight.size() > 1) //do not open more than one Explorer instance!
        {
            if ((leftSide && !selectionLeft .empty()) ||
                (!leftSide && selectionRight.empty()))
                return openExternalApplication(commandLinePhrase, leftSide, { selectionLeft[0] }, {});
            else
                return openExternalApplication(commandLinePhrase, leftSide, {}, { selectionRight[0] });
        }

        if (selectionLeft.empty() && selectionRight.empty())
            return openFolderInFileBrowser(leftSide ?
                                           createAbstractPath(firstFolderPair_->getValues().folderPathPhraseLeft) :
                                           createAbstractPath(firstFolderPair_->getValues().folderPathPhraseRight));
        //in this context either left or right selection is filled with exactly one item
        if (!selectionLeft.empty())
        {
            if (selectionLeft[0]->isEmpty<LEFT_SIDE>())
                return openFolderInFileBrowser(getExistingParentFolder<LEFT_SIDE>(*selectionLeft[0]));
        }
        else
        {
            if (selectionRight[0]->isEmpty<RIGHT_SIDE>())
                return openFolderInFileBrowser(getExistingParentFolder<RIGHT_SIDE>(*selectionRight[0]));
        }
    }

    //regular command evaluation:
    const size_t invokeCount = selectionLeft.size() + selectionRight.size();
    if (invokeCount > EXT_APP_MASS_INVOKE_THRESHOLD)
        if (globalCfg_.confirmDlgs.confirmCommandMassInvoke)
        {
            bool dontAskAgain = false;
            switch (showConfirmationDialog(this, DialogInfoType::warning, PopupDialogCfg().
                                           setTitle(_("Confirm")).
                                           setMainInstructions(replaceCpy(_P("Do you really want to execute the command %y for one item?",
                                                                             "Do you really want to execute the command %y for %x items?", invokeCount),
                                                                          L"%y", fmtPath(commandLinePhrase))).
                                           setCheckBox(dontAskAgain, _("&Don't show this warning again")),
                                           _("&Execute")))
            {
                case ConfirmationButton::accept:
                    globalCfg_.confirmDlgs.confirmCommandMassInvoke = !dontAskAgain;
                    break;
                case ConfirmationButton::cancel:
                    return;
            }
        }

    std::set<FileDescriptor> nonNativeFiles;
    if (contains(commandLinePhrase, Zstr("%local_path%")))
    {
        collectNonNativeFiles< LEFT_SIDE>(selectionLeft,  tempFileBuf_, nonNativeFiles);
        collectNonNativeFiles<RIGHT_SIDE>(selectionRight, tempFileBuf_, nonNativeFiles);
    }
    if (contains(commandLinePhrase, Zstr("%local_path2%")))
    {
        collectNonNativeFiles<RIGHT_SIDE>(selectionLeft,  tempFileBuf_, nonNativeFiles);
        collectNonNativeFiles< LEFT_SIDE>(selectionRight, tempFileBuf_, nonNativeFiles);
    }

    //##################### create temporary files for non-native paths ######################
    if (!nonNativeFiles.empty())
    {
        FocusPreserver fp;

        disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
        auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
        ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

        const auto& guiCfg = getConfig();

        StatusHandlerTemporaryPanel statusHandler(*this, std::chrono::system_clock::now() /*startTime*/,
                                                  false /*ignoreErrors*/,
                                                  guiCfg.mainCfg.autoRetryCount,
                                                  guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
        try
        {
            tempFileBuf_.createTempFiles(nonNativeFiles, statusHandler); //throw AbortProcess
            //"clearSelection" not needed/desired
        }
        catch (AbortProcess&) {}

        const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
        setLastOperationLog(r.summary, r.errorLog);

        if (r.summary.syncResult == SyncResult::aborted)
            return;

        //updateGui(); -> not needed
    }
    //########################################################################################
    try
    {
        invokeCommandLine< LEFT_SIDE>(commandLinePhrase, openWithDefaultAppRequested, selectionLeft,  tempFileBuf_); //throw FileError
        invokeCommandLine<RIGHT_SIDE>(commandLinePhrase, openWithDefaultAppRequested, selectionRight, tempFileBuf_); //
    }
    catch (const FileError& e) { showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString())); }
}



void MainDialog::flashStatusInformation(const wxString& text)
{
    oldStatusMsgs_.push_back(m_staticTextStatusCenter->GetLabel());

    m_staticTextStatusCenter->SetLabel(text);
    m_staticTextStatusCenter->SetForegroundColour(wxColor(31, 57, 226)); //highlight color: blue
    m_staticTextStatusCenter->SetFont(m_staticTextStatusCenter->GetFont().Bold());

    m_panelStatusBar->Layout();
    //if (needLayoutUpdate) auiMgr.Update(); -> not needed here, this is called anyway in updateGui()

    auto restoreStatusInformation = [this]
    {
        if (!oldStatusMsgs_.empty())
        {
            wxString oldMsg = oldStatusMsgs_.back();
            oldStatusMsgs_.pop_back();

            if (oldStatusMsgs_.empty()) //restore original status text
            {
                m_staticTextStatusCenter->SetLabel(oldMsg);
                m_staticTextStatusCenter->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT)); //reset color

                wxFont font = m_staticTextStatusCenter->GetFont();
                font.SetWeight(wxFONTWEIGHT_NORMAL);
                m_staticTextStatusCenter->SetFont(font);

                m_panelStatusBar->Layout();
            }
        }
    };
    guiQueue_.processAsync([] { std::this_thread::sleep_for(std::chrono::seconds(3)); }, restoreStatusInformation);
}


void MainDialog::disableGuiElements(bool enableAbort)
{
    assert(!operationInProgress_);

    //disables all elements (except abort button) that might receive user input during long-running processes:
    //when changing consider: comparison, synchronization, manual deletion

    //OS X: wxWidgets portability promise is again a mess: http://wxwidgets.10942.n7.nabble.com/Disable-panel-and-appropriate-children-windows-linux-macos-td35357.html

    EnableCloseButton(false); //closing main dialog is not allowed during synchronization! crash!
    //EnableCloseButton(false) just does not work reliably!
    //- Windows: dialog can still be closed by clicking the task bar preview window with the middle mouse button or by pressing ALT+F4!
    //- OS X: Quit/Preferences menu items still enabled during sync,
    //       ([[m_macWindow standardWindowButton:NSWindowCloseButton] setEnabled:enable]) does not stick after calling Maximize() ([m_macWindow zoom:nil])
    //- Linux: it just works! :)
    operationInProgress_ = true;

    localKeyEventsEnabled_ = false;

    for (size_t pos = 0; pos < m_menubar->GetMenuCount(); ++pos)
        m_menubar->EnableTop(pos, false);

    if (enableAbort)
    {
        m_buttonCancel->Enable();
        m_buttonCancel->Show();
        //if (m_buttonCancel->IsShownOnScreen()) -> needed?
        m_buttonCancel->SetFocus();
        m_buttonCompare->Disable();
        m_buttonCompare->Hide();
        m_panelTopButtons->Layout();

        m_bpButtonCmpConfig  ->Disable();
        m_bpButtonCmpContext ->Disable();
        m_bpButtonFilter     ->Disable();
        m_bpButtonFilterContext->Disable();
        m_bpButtonSyncConfig ->Disable();
        m_bpButtonSyncContext->Disable();
        m_buttonSync         ->Disable();
    }
    else
        m_panelTopButtons->Disable();

    m_panelDirectoryPairs->Disable();
    m_gridOverview       ->Disable();
    m_panelCenter        ->Disable();
    m_panelSearch        ->Disable();
    m_panelLog           ->Disable();
    m_panelConfig        ->Disable();
    m_panelViewFilter    ->Disable();

    Refresh(); //wxWidgets fails to do this automatically for child items of disabled windows
}


void MainDialog::enableGuiElements()
{
    assert(operationInProgress_ && !localKeyEventsEnabled_); //disableGuiElements() not called? => WTF!

    //wxGTK, yet another QOI issue: some stupid bug keeps moving main dialog to top!!

    EnableCloseButton(true);
    operationInProgress_ = false;

    localKeyEventsEnabled_ = true;

    for (size_t pos = 0; pos < m_menubar->GetMenuCount(); ++pos)
        m_menubar->EnableTop(pos, true);

    m_buttonCancel->Disable();
    m_buttonCancel->Hide();
    m_buttonCompare->Enable();
    m_buttonCompare->Show();
    m_panelTopButtons->Layout();

    m_bpButtonCmpConfig  ->Enable();
    m_bpButtonCmpContext ->Enable();
    m_bpButtonFilter     ->Enable();
    m_bpButtonFilterContext->Enable();
    m_bpButtonSyncConfig ->Enable();
    m_bpButtonSyncContext->Enable();
    m_buttonSync         ->Enable();

    m_panelTopButtons->Enable();

    m_panelDirectoryPairs->Enable();
    m_gridOverview       ->Enable();
    m_panelCenter        ->Enable();
    m_panelSearch        ->Enable();
    m_panelLog           ->Enable();
    m_panelConfig        ->Enable();
    m_panelViewFilter    ->Enable();

    Refresh();

    warn_static("test: needed on macOS?")
    //auiMgr_.Update(); //at least wxWidgets on macOS fails to do this after enabling
}


namespace
{
void updateSizerOrientation(wxBoxSizer& sizer, wxWindow& window, double horizontalWeight)
{
    const int newOrientation = window.GetSize().GetWidth() * horizontalWeight > window.GetSize().GetHeight() ? wxHORIZONTAL : wxVERTICAL; //check window NOT sizer width!
    if (sizer.GetOrientation() != newOrientation)
    {
        sizer.SetOrientation(newOrientation);
        window.Layout();
    }
}
}


void MainDialog::onResizeTopButtonPanel(wxEvent& event)
{
    updateSizerOrientation(*bSizerTopButtons, *m_panelTopButtons, 0.5);
    event.Skip();
}


void MainDialog::onResizeConfigPanel(wxEvent& event)
{
    updateSizerOrientation(*bSizerConfig, *m_panelConfig, 0.5);
    event.Skip();
}


void MainDialog::onResizeViewPanel(wxEvent& event)
{
    //we need something more fancy for the statistics:
    const int newOrientation = m_panelViewFilter->GetSize().GetWidth() > m_panelViewFilter->GetSize().GetHeight() ? wxHORIZONTAL : wxVERTICAL; //check window NOT sizer width!
    if (bSizerViewFilter->GetOrientation() != newOrientation)
    {
        //apply opposite orientation for child sizers
        const int childOrient = newOrientation == wxHORIZONTAL ? wxVERTICAL : wxHORIZONTAL;
        wxSizerItemList& sl = bSizerStatistics->GetChildren();
        for (auto it = sl.begin(); it != sl.end(); ++it) //yet another wxWidgets bug keeps us from using std::for_each
        {
            wxSizerItem& szItem = **it;
            if (auto sizerChild = dynamic_cast<wxBoxSizer*>(szItem.GetSizer()))
                if (sizerChild->GetOrientation() != childOrient)
                    sizerChild->SetOrientation(childOrient);
        }

        bSizerStatistics->SetOrientation(newOrientation);
        bSizerViewFilter->SetOrientation(newOrientation);
        m_panelViewFilter->Layout();
        m_panelStatistics->Layout();
    }

    event.Skip();
}


void MainDialog::onResizeLeftFolderWidth(wxEvent& event)
{
    //adapt left-shift display distortion caused by scrollbars for multiple folder pairs
    const int width = m_panelTopLeft->GetSize().GetWidth();
    for (FolderPairPanel* panel : additionalFolderPairs_)
        panel->m_panelLeft->SetMinSize({width, -1});

    event.Skip();
}


void MainDialog::onTreeKeyEvent(wxKeyEvent& event)
{
    const std::vector<FileSystemObject*> selection = getTreeSelection();

    int keyCode = event.GetKeyCode();
    if (m_gridOverview->GetLayoutDirection() == wxLayout_RightToLeft)
    {
        if (keyCode == WXK_LEFT || keyCode == WXK_NUMPAD_LEFT)
            keyCode = WXK_RIGHT;
        else if (keyCode == WXK_RIGHT || keyCode == WXK_NUMPAD_RIGHT)
            keyCode = WXK_LEFT;
    }

    if (event.ControlDown())
        switch (keyCode)
        {
            case 'C':
            case WXK_INSERT: //CTRL + C || CTRL + INS
                copySelectionToClipboard({ m_gridOverview });
                return;
        }

    else if (event.AltDown())
        switch (keyCode)
        {
            case WXK_NUMPAD_LEFT:
            case WXK_LEFT: //ALT + <left>
                setSyncDirManually(selection, SyncDirection::left);
                return;

            case WXK_NUMPAD_RIGHT:
            case WXK_RIGHT: //ALT + <right>
                setSyncDirManually(selection, SyncDirection::right);
                return;

            case WXK_NUMPAD_UP:
            case WXK_NUMPAD_DOWN:
            case WXK_UP:   //ALT + <up>
            case WXK_DOWN: //ALT + <down>
                setSyncDirManually(selection, SyncDirection::none);
                return;
        }

    else
        switch (keyCode)
        {
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
                startSyncForSelecction(selection);
                return;

            case WXK_SPACE:
            case WXK_NUMPAD_SPACE:
                if (!selection.empty())
                    setIncludedManually(selection, m_bpButtonShowExcluded->isActive() && !selection[0]->isActive());
                //always exclude items if "m_bpButtonShowExcluded is unchecked" => yes, it's possible to have already unchecked items in selection, so we need to overwrite:
                //e.g. select root node while the first item returned is not shown on grid!
                return;

            case WXK_DELETE:
            case WXK_NUMPAD_DELETE:
                deleteSelectedFiles(selection, selection, !event.ShiftDown() /*moveToRecycler*/);
                return;
        }

    event.Skip(); //unknown keypress: propagate
}


void MainDialog::onGridKeyEvent(wxKeyEvent& event, Grid& grid, bool leftSide)
{
    const std::vector<FileSystemObject*> selection      = getGridSelection();
    const std::vector<FileSystemObject*> selectionLeft  = getGridSelection(true, false);
    const std::vector<FileSystemObject*> selectionRight = getGridSelection(false, true);

    int keyCode = event.GetKeyCode();
    if (grid.GetLayoutDirection() == wxLayout_RightToLeft)
    {
        if (keyCode == WXK_LEFT || keyCode == WXK_NUMPAD_LEFT)
            keyCode = WXK_RIGHT;
        else if (keyCode == WXK_RIGHT || keyCode == WXK_NUMPAD_RIGHT)
            keyCode = WXK_LEFT;
    }

    if (event.ControlDown())
        switch (keyCode)
        {
            case 'C':
            case WXK_INSERT: //CTRL + C || CTRL + INS
                copySelectionToClipboard({ m_gridMainL, m_gridMainR} );
                return; // -> swallow event! don't allow default grid commands!

            case 'T': //CTRL + T
                copyToAlternateFolder(selectionLeft, selectionRight);
                return;
        }

    else if (event.AltDown())
        switch (keyCode)
        {
            case WXK_NUMPAD_LEFT:
            case WXK_LEFT: //ALT + <left>
                setSyncDirManually(selection, SyncDirection::left);
                return;

            case WXK_NUMPAD_RIGHT:
            case WXK_RIGHT: //ALT + <right>
                setSyncDirManually(selection, SyncDirection::right);
                return;

            case WXK_NUMPAD_UP:
            case WXK_NUMPAD_DOWN:
            case WXK_UP:   //ALT + <up>
            case WXK_DOWN: //ALT + <down>
                setSyncDirManually(selection, SyncDirection::none);
                return;
        }

    else
    {
        //0 ... 9
        const size_t extAppPos = [&]() -> size_t
        {
            if ('0' <= keyCode && keyCode <= '9')
                return keyCode - '0';
            if (WXK_NUMPAD0 <= keyCode && keyCode <= WXK_NUMPAD9)
                return keyCode - WXK_NUMPAD0;
            return static_cast<size_t>(-1);
        }();

        if (extAppPos < globalCfg_.externalApps.size())
        {
            openExternalApplication(globalCfg_.externalApps[extAppPos].cmdLine, leftSide, selectionLeft, selectionRight);
            return;
        }

        switch (keyCode)
        {
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
                startSyncForSelecction(selection);
                return;

            case WXK_SPACE:
            case WXK_NUMPAD_SPACE:
                if (!selection.empty())
                    setIncludedManually(selection, m_bpButtonShowExcluded->isActive() && !selection[0]->isActive());
                return;

            case WXK_DELETE:
            case WXK_NUMPAD_DELETE:
                deleteSelectedFiles(selectionLeft, selectionRight, !event.ShiftDown() /*moveToRecycler*/);
                return;
        }
    }

    event.Skip(); //unknown keypress: propagate
}


void MainDialog::onLocalKeyEvent(wxKeyEvent& event) //process key events without explicit menu entry :)
{
    if (!localKeyEventsEnabled_)
    {
        event.Skip();
        return;
    }
    localKeyEventsEnabled_ = false; //avoid recursion
    ZEN_ON_SCOPE_EXIT(localKeyEventsEnabled_ = true);


    const int keyCode = event.GetKeyCode();

    //CTRL + X
    if (event.ControlDown())
        switch (keyCode)
        {
            //        case 'F': //CTRL + F
            //            showFindPanel();
            //            return; //-> swallow event!

            case 'W': //CTRL + W
                //don't use F10: and avoid accidental clicks: https://freefilesync.org/forum/viewtopic.php?t=1663
                wxCommandEvent dummy(wxEVT_COMMAND_BUTTON_CLICKED);
                m_bpButtonSwapSides->Command(dummy); //simulate click
                return; //-> swallow event!
        }

    switch (keyCode)
    {
        case WXK_F3:
        case WXK_NUMPAD_F3:
            startFindNext(!event.ShiftDown() /*searchAscending*/);
            return; //-> swallow event!

        //case WXK_F6:
        //{
        //    wxCommandEvent dummy2(wxEVT_COMMAND_BUTTON_CLICKED);
        //    m_bpButtonCmpConfig->Command(dummy2); //simulate click
        //}
        //return; //-> swallow event!

        //case WXK_F7:
        //{
        //    wxCommandEvent dummy2(wxEVT_COMMAND_BUTTON_CLICKED);
        //    m_bpButtonFilter->Command(dummy2); //simulate click
        //}
        //return; //-> swallow event!

        //case WXK_F8:
        //{
        //    wxCommandEvent dummy2(wxEVT_COMMAND_BUTTON_CLICKED);
        //    m_bpButtonSyncConfig->Command(dummy2); //simulate click
        //}
        //return; //-> swallow event!

        case WXK_F11:
            setGridViewType(m_bpButtonViewType->isActive() ? GridViewType::difference : GridViewType::action);
            return; //-> swallow event!

        //redirect certain (unhandled) keys directly to grid!
        case WXK_UP:
        case WXK_DOWN:
        case WXK_LEFT:
        case WXK_RIGHT:
        case WXK_PAGEUP:
        case WXK_PAGEDOWN:
        case WXK_HOME:
        case WXK_END:

        case WXK_NUMPAD_UP:
        case WXK_NUMPAD_DOWN:
        case WXK_NUMPAD_LEFT:
        case WXK_NUMPAD_RIGHT:
        case WXK_NUMPAD_PAGEUP:
        case WXK_NUMPAD_PAGEDOWN:
        case WXK_NUMPAD_HOME:
        case WXK_NUMPAD_END:
        {
            const wxWindow* focus = wxWindow::FindFocus();
            if (!isComponentOf(focus, m_gridMainL     ) && //
                !isComponentOf(focus, m_gridMainC     ) && //don't propagate keyboard commands if grid is already in focus
                !isComponentOf(focus, m_gridMainR     ) && //
                !isComponentOf(focus, m_gridOverview  ) &&
                !isComponentOf(focus, m_gridCfgHistory) && //don't propagate if selecting config
                !isComponentOf(focus, m_panelSearch   ) &&
                !isComponentOf(focus, m_panelLog      ) &&
                !isComponentOf(focus, m_panelDirectoryPairs) && //don't propagate if changing directory fields
                m_gridMainL->IsEnabled())
                if (wxEvtHandler* evtHandler = m_gridMainL->getMainWin().GetEventHandler())
                {
                    m_gridMainL->SetFocus();

                    event.SetEventType(wxEVT_KEY_DOWN); //the grid event handler doesn't expect wxEVT_CHAR_HOOK!
                    evtHandler->ProcessEvent(event); //propagating event to child lead to recursion with old key_event.h handling => still an issue?
                    event.Skip(false); //definitively handled now!
                    return;
                }
        }
        break;

        case WXK_ESCAPE: //let's do something useful and hide the log panel
        {
            const wxWindow* focus = wxWindow::FindFocus();
            if (!isComponentOf(focus, m_panelSearch)  && //search panel also handles ESC!
                m_panelLog->IsEnabled())
            {
                if (auiMgr_.GetPane(m_panelLog).IsShown()) //else: let it "ding"
                    return showLogPanel(false /*show*/);
            }
        }
        break;
    }

    event.Skip();
}


void MainDialog::onTreeGridSelection(GridSelectEvent& event)
{
    //scroll m_gridMain to user's new selection on m_gridOverview
    ptrdiff_t leadRow = -1;
    if (event.positive_ && event.rowFirst_ != event.rowLast_)
        if (std::unique_ptr<TreeView::Node> node = treegrid::getDataView(*m_gridOverview).getLine(event.rowFirst_))
        {
            if (const TreeView::RootNode* root = dynamic_cast<const TreeView::RootNode*>(node.get()))
                leadRow = filegrid::getDataView(*m_gridMainC).findRowFirstChild(&(root->baseFolder));
            else if (const TreeView::DirNode* dir = dynamic_cast<const TreeView::DirNode*>(node.get()))
            {
                leadRow = filegrid::getDataView(*m_gridMainC).findRowDirect(&(dir->folder));
                if (leadRow < 0) //directory was filtered out! still on tree view (but NOT on grid view)
                    leadRow = filegrid::getDataView(*m_gridMainC).findRowFirstChild(&(dir->folder));
            }
            else if (const TreeView::FilesNode* files = dynamic_cast<const TreeView::FilesNode*>(node.get()))
            {
                assert(!files->filesAndLinks.empty());
                if (!files->filesAndLinks.empty())
                    leadRow = filegrid::getDataView(*m_gridMainC).findRowDirect(files->filesAndLinks[0]->getId());
            }
        }

    if (leadRow >= 0)
    {
        leadRow = std::max<ptrdiff_t>(0, leadRow - 1); //scroll one more row

        m_gridMainL->scrollTo(leadRow); //scroll all of them (including "scroll master")
        m_gridMainC->scrollTo(leadRow); //
        m_gridMainR->scrollTo(leadRow); //

        m_gridOverview->getMainWin().Update(); //draw cursor immediately rather than on next idle event (required for slow CPUs, netbook)
    }

    //get selection on overview panel and set corresponding markers on main grid
    std::unordered_set<const FileSystemObject*> markedFilesAndLinks; //mark files/symlinks directly
    std::unordered_set<const ContainerObject*> markedContainer;      //mark full container including child-objects

    for (size_t row : m_gridOverview->getSelectedRows())
        if (std::unique_ptr<TreeView::Node> node = treegrid::getDataView(*m_gridOverview).getLine(row))
        {
            if (const TreeView::RootNode* root = dynamic_cast<const TreeView::RootNode*>(node.get()))
                markedContainer.insert(&(root->baseFolder));
            else if (const TreeView::DirNode* dir = dynamic_cast<const TreeView::DirNode*>(node.get()))
                markedContainer.insert(&(dir->folder));
            else if (const TreeView::FilesNode* files = dynamic_cast<const TreeView::FilesNode*>(node.get()))
                markedFilesAndLinks.insert(files->filesAndLinks.begin(), files->filesAndLinks.end());
        }

    filegrid::setNavigationMarker(*m_gridMainL, *m_gridMainR,
                                  std::move(markedFilesAndLinks), std::move(markedContainer));

    event.Skip();
}


void MainDialog::onTreeGridContext(GridContextMenuEvent& event)
{
    const std::vector<FileSystemObject*>& selection = getTreeSelection(); //referenced by lambdas!
    ContextMenu menu;

    //----------------------------------------------------------------------------------------------------
    auto getImage = [&](SyncDirection dir, SyncOperation soDefault)
    {
        return mirrorIfRtl(getSyncOpImage(!selection.empty() && selection[0]->getSyncOperation() != SO_EQUAL ?
                                          selection[0]->testSyncOperation(dir) : soDefault));
    };
    const wxImage opRight = getImage(SyncDirection::right, SO_OVERWRITE_RIGHT);
    const wxImage opNone  = getImage(SyncDirection::none,  SO_DO_NOTHING     );
    const wxImage opLeft  = getImage(SyncDirection::left,  SO_OVERWRITE_LEFT );

    wxString shortcutLeft  = L"\tAlt+Left";
    wxString shortcutRight = L"\tAlt+Right";
    if (m_gridOverview->GetLayoutDirection() == wxLayout_RightToLeft)
        std::swap(shortcutLeft, shortcutRight);

    const bool nonEqualSelected = selectionIncludesNonEqualItem(selection);
    menu.addItem(_("Set direction:") + L" ->" + shortcutRight, [this, &selection] { setSyncDirManually(selection, SyncDirection::right); }, opRight, nonEqualSelected);
    menu.addItem(_("Set direction:") + L" -" L"\tAlt+Down",    [this, &selection] { setSyncDirManually(selection, SyncDirection::none);  }, opNone,  nonEqualSelected);
    menu.addItem(_("Set direction:") + L" <-" + shortcutLeft,  [this, &selection] { setSyncDirManually(selection, SyncDirection::left);  }, opLeft,  nonEqualSelected);
    //Gtk needs a direction, "<-", because it has no context menu icons!
    //Gtk requires "no spaces" for shortcut identifiers!
    menu.addSeparator();
    //----------------------------------------------------------------------------------------------------
    auto addFilterMenu = [&](const std::wstring& label, const char* iconName, bool include)
    {
        if (selection.size() == 1)
        {
            ContextMenu submenu;

            const bool isFolder = dynamic_cast<const FolderPair*>(selection[0]) != nullptr;

            //by short name
            Zstring labelShort = Zstring(Zstr("*")) + FILE_NAME_SEPARATOR + selection[0]->getItemNameAny();
            if (isFolder)
                labelShort += FILE_NAME_SEPARATOR;
            submenu.addItem(utfTo<wxString>(labelShort), [this, &selection, include] { filterShortname(*selection[0], include); });

            //by relative path
            Zstring labelRel = FILE_NAME_SEPARATOR + selection[0]->getRelativePathAny();
            if (isFolder)
                labelRel += FILE_NAME_SEPARATOR;
            submenu.addItem(utfTo<wxString>(labelRel), [this, &selection, include] { filterItems(selection, include); });

            menu.addSubmenu(label, submenu, loadImage(iconName));
        }
        else if (selection.size() > 1)
        {
            //by relative path
            menu.addItem(label + L" <" + _("multiple selection") + L">",
                         [this, &selection, include] { filterItems(selection, include); }, loadImage(iconName));
        }
    };
    addFilterMenu(_("&Include via filter:"), "filter_include_sicon", true);
    addFilterMenu(_("&Exclude via filter:"), "filter_exclude_sicon", false);
    //----------------------------------------------------------------------------------------------------
    if (m_bpButtonShowExcluded->isActive() && !selection.empty() && !selection[0]->isActive())
        menu.addItem(_("Include temporarily") + L"\tSpace", [this, &selection] { setIncludedManually(selection, true); }, loadImage("checkbox_true"));
    else
        menu.addItem(_("Exclude temporarily") + L"\tSpace", [this, &selection] { setIncludedManually(selection, false); }, loadImage("checkbox_false"), !selection.empty());
    //----------------------------------------------------------------------------------------------------
    const bool selectionContainsItemsToSync = [&]
    {
        for (FileSystemObject* fsObj : expandSelectionForPartialSync(selection))
            switch (fsObj->getSyncOperation())
            {
                case SO_CREATE_NEW_LEFT:
                case SO_CREATE_NEW_RIGHT:
                case SO_DELETE_LEFT:
                case SO_DELETE_RIGHT:
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_RIGHT_TO:
                case SO_OVERWRITE_LEFT:
                case SO_OVERWRITE_RIGHT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                    return true;

                case SO_UNRESOLVED_CONFLICT:
                case SO_DO_NOTHING:
                case SO_EQUAL:
                    break;
            }
        return false;
    }();
    menu.addSeparator();
    menu.addItem(_("&Synchronize selection") + L"\tEnter", [&] { startSyncForSelecction(selection); }, loadImage("start_sync_selection_sicon"), selectionContainsItemsToSync);
    //----------------------------------------------------------------------------------------------------
    const bool haveNonEmptyItems = std::any_of(selection.begin(), selection.end(), [](const FileSystemObject* fsObj) { return !fsObj->isEmpty<LEFT_SIDE>() || !fsObj->isEmpty<RIGHT_SIDE>(); });
    //menu.addSeparator();
    //menu.addItem(_("&Copy to...") + L"\tCtrl+T", [&] { copyToAlternateFolder(selection, selection); }, wxNullImage, haveNonEmptyItems);
    //----------------------------------------------------------------------------------------------------
    menu.addSeparator();
    menu.addItem(_("&Delete") + L"\t(Shift+)Del", [&] { deleteSelectedFiles(selection, selection, true /*moveToRecycler*/); }, imgTrashSmall_, haveNonEmptyItems);

    menu.popup(*m_gridOverview);
}


void MainDialog::onGridContextRim(GridContextMenuEvent& event, bool leftSide)
{
    const std::vector<FileSystemObject*> selection      = getGridSelection(); //referenced by lambdas!
    const std::vector<FileSystemObject*> selectionLeft  = getGridSelection(true, false);
    const std::vector<FileSystemObject*> selectionRight = getGridSelection(false, true);

    onGridContextRim(getGridSelection(),
                     getGridSelection(true, false),
                     getGridSelection(false, true), leftSide);
}


void MainDialog::onGridGroupContextRim(GridClickEvent& event, bool leftSide)
{
    if (static_cast<HoverAreaGroup>(event.hoverArea_) == HoverAreaGroup::groupName)
        if (const FileView::PathDrawInfo pdi = filegrid::getDataView(*m_gridMainC).getDrawInfo(event.row_);
            pdi.folderGroupObj)
        {
            m_gridMainL->clearSelection(GridEventPolicy::deny);
            m_gridMainC->clearSelection(GridEventPolicy::deny);
            m_gridMainR->clearSelection(GridEventPolicy::deny);

            std::vector<FileSystemObject*> selectionLeft;
            std::vector<FileSystemObject*> selectionRight;
            (leftSide ? selectionLeft : selectionRight).push_back(pdi.folderGroupObj);

            onGridContextRim({pdi.folderGroupObj},
                             selectionLeft,
                             selectionRight, leftSide);
            return; //"swallow" event => suppress default context menu handling
        }

    assert(static_cast<HoverAreaGroup>(event.hoverArea_) != HoverAreaGroup::groupName);
    event.Skip();
}


void MainDialog::onGridContextRim(const std::vector<FileSystemObject*>& selection,
                                  const std::vector<FileSystemObject*>& selectionLeft,
                                  const std::vector<FileSystemObject*>& selectionRight, bool leftSide)
{
    ContextMenu menu;

    auto getImage = [&](SyncDirection dir, SyncOperation soDefault)
    {
        return mirrorIfRtl(getSyncOpImage(!selection.empty() && selection[0]->getSyncOperation() != SO_EQUAL ?
                                          selection[0]->testSyncOperation(dir) : soDefault));
    };
    const wxImage opLeft  = getImage(SyncDirection::left,  SO_OVERWRITE_LEFT );
    const wxImage opRight = getImage(SyncDirection::right, SO_OVERWRITE_RIGHT);
    const wxImage opNone  = getImage(SyncDirection::none,  SO_DO_NOTHING     );

    wxString shortcutLeft  = L"\tAlt+Left";
    wxString shortcutRight = L"\tAlt+Right";
    if (m_gridMainL->GetLayoutDirection() == wxLayout_RightToLeft)
        std::swap(shortcutLeft, shortcutRight);

    const bool nonEqualSelected = selectionIncludesNonEqualItem(selection);
    menu.addItem(_("Set direction:") + L" ->" + shortcutRight, [this, &selection] { setSyncDirManually(selection, SyncDirection::right); }, opRight, nonEqualSelected);
    menu.addItem(_("Set direction:") + L" -" L"\tAlt+Down",    [this, &selection] { setSyncDirManually(selection, SyncDirection::none);  }, opNone,  nonEqualSelected);
    menu.addItem(_("Set direction:") + L" <-" + shortcutLeft,  [this, &selection] { setSyncDirManually(selection, SyncDirection::left);  }, opLeft,  nonEqualSelected);
    //GTK needs a direction, "<-", because it has no context menu icons!
    //GTK does not allow spaces in shortcut identifiers!
    menu.addSeparator();
    //----------------------------------------------------------------------------------------------------
    auto addFilterMenu = [&](const wxString& label, const char* iconName, bool include)
    {
        if (selection.size() == 1)
        {
            ContextMenu submenu;

            const bool isFolder = dynamic_cast<const FolderPair*>(selection[0]) != nullptr;

            //by extension
            if (!isFolder)
            {
                const Zstring extension = getFileExtension(selection[0]->getItemNameAny());
                if (!extension.empty())
                    submenu.addItem(L"*." + utfTo<wxString>(extension),
                                    [this, extension, include] { filterExtension(extension, include); });
            }

            //by short name
            Zstring labelShort = Zstring(Zstr("*")) + FILE_NAME_SEPARATOR + selection[0]->getItemNameAny();
            if (isFolder)
                labelShort += FILE_NAME_SEPARATOR;
            submenu.addItem(utfTo<wxString>(labelShort), [this, &selection, include] { filterShortname(*selection[0], include); });

            //by relative path
            Zstring labelRel = FILE_NAME_SEPARATOR + selection[0]->getRelativePathAny();
            if (isFolder)
                labelRel += FILE_NAME_SEPARATOR;
            submenu.addItem(utfTo<wxString>(labelRel), [this, &selection, include] { filterItems(selection, include); });

            menu.addSubmenu(label, submenu, loadImage(iconName));
        }
        else if (selection.size() > 1)
        {
            //by relative path
            menu.addItem(label + L" <" + _("multiple selection") + L">",
                         [this, &selection, include] { filterItems(selection, include); }, loadImage(iconName));
        }
    };
    addFilterMenu(_("&Include via filter:"), "filter_include_sicon", true);
    addFilterMenu(_("&Exclude via filter:"), "filter_exclude_sicon", false);
    //----------------------------------------------------------------------------------------------------
    if (m_bpButtonShowExcluded->isActive() && !selection.empty() && !selection[0]->isActive())
        menu.addItem(_("Include temporarily") + L"\tSpace", [this, &selection] { setIncludedManually(selection, true); }, loadImage("checkbox_true"));
    else
        menu.addItem(_("Exclude temporarily") + L"\tSpace", [this, &selection] { setIncludedManually(selection, false); }, loadImage("checkbox_false"), !selection.empty());
    //----------------------------------------------------------------------------------------------------
    const bool selectionContainsItemsToSync = [&]
    {
        for (FileSystemObject* fsObj : expandSelectionForPartialSync(selection))
            switch (fsObj->getSyncOperation())
            {
                case SO_CREATE_NEW_LEFT:
                case SO_CREATE_NEW_RIGHT:
                case SO_DELETE_LEFT:
                case SO_DELETE_RIGHT:
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_RIGHT_TO:
                case SO_OVERWRITE_LEFT:
                case SO_OVERWRITE_RIGHT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                    return true;

                case SO_UNRESOLVED_CONFLICT:
                case SO_DO_NOTHING:
                case SO_EQUAL:
                    break;
            }
        return false;
    }();
    menu.addSeparator();
    menu.addItem(_("&Synchronize selection") + L"\tEnter", [&] { startSyncForSelecction(selection); }, loadImage("start_sync_selection_sicon"), selectionContainsItemsToSync);
    //----------------------------------------------------------------------------------------------------
    if (!globalCfg_.externalApps.empty())
    {
        menu.addSeparator();

        for (auto it = globalCfg_.externalApps.begin();
             it != globalCfg_.externalApps.end();
             ++it)
        {
            //translate default external apps on the fly: 1. "Show in Explorer" 2. "Open with default application"
            wxString description = translate(it->description);
            if (description.empty())
                description = L' '; //wxWidgets doesn't like empty labels

            auto openApp = [this, command = it->cmdLine, leftSide, &selectionLeft, &selectionRight] { openExternalApplication(command, leftSide, selectionLeft, selectionRight); };

            const size_t pos = it - globalCfg_.externalApps.begin();

            if (pos == 0)
                description += L"\tD-Click, 0";
            else if (pos < 9)
                description += L"\t" + numberTo<std::wstring>(pos);

            menu.addItem(description, openApp, wxNullImage, !selectionLeft.empty() || !selectionRight.empty());
        }
    }
    //----------------------------------------------------------------------------------------------------
    const bool haveNonEmptyItemsL = std::any_of(selectionLeft .begin(), selectionLeft .end(), [](const FileSystemObject* fsObj) { return !fsObj->isEmpty<LEFT_SIDE >(); });
    const bool haveNonEmptyItemsR = std::any_of(selectionRight.begin(), selectionRight.end(), [](const FileSystemObject* fsObj) { return !fsObj->isEmpty<RIGHT_SIDE>(); });

    menu.addSeparator();
    menu.addItem(_("&Copy to...") + L"\tCtrl+T", [&] { copyToAlternateFolder(selectionLeft, selectionRight); }, wxNullImage, haveNonEmptyItemsL || haveNonEmptyItemsR);
    //----------------------------------------------------------------------------------------------------
    menu.addSeparator();
    menu.addItem(_("&Delete") + L"\t(Shift+)Del", [&] { deleteSelectedFiles(selectionLeft, selectionRight, true /*moveToRecycler*/); }, imgTrashSmall_, haveNonEmptyItemsL || haveNonEmptyItemsR);

    menu.popup(leftSide ? *m_gridMainL : *m_gridMainR);
}


void MainDialog::addFilterPhrase(const Zstring& phrase, bool include, bool requireNewLine)
{
    Zstring& filterString = [&]() -> Zstring&
    {
        if (include)
        {
            Zstring& includeFilter = currentCfg_.mainCfg.globalFilter.includeFilter;
            if (NameFilter::isNull(includeFilter, Zstring())) //fancy way of checking for "*" include
                includeFilter.clear();
            return includeFilter;
        }
        else
            return currentCfg_.mainCfg.globalFilter.excludeFilter;
    }();

    if (requireNewLine)
    {
        trim(filterString, false, true, [](Zchar c) { return c == FILTER_ITEM_SEPARATOR || c == Zstr('\n') || c == Zstr(' '); });
        if (!filterString.empty())
            filterString += Zstr('\n');
        filterString += phrase;
    }
    else
    {
        trim(filterString, false, true, [](Zchar c) { return c == Zstr('\n') || c == Zstr(' '); });

        if (filterString.empty())
            ;
        else if (endsWith(filterString, FILTER_ITEM_SEPARATOR))
            filterString += Zstr(' ');
        else
            filterString += Zstr('\n');

        filterString += phrase + Zstr(' ') + FILTER_ITEM_SEPARATOR; //append FILTER_ITEM_SEPARATOR to 'mark' that next extension exclude should write to same line
    }

    updateGlobalFilterButton();
    if (include)
        applyFilterConfig(); //user's temporary exclusions lost!
    else //do not fully apply filter, just exclude new items: preserve user's temporary exclusions
    {
        std::for_each(begin(folderCmp_), end(folderCmp_), [&](BaseFolderPair& baseFolder) { addHardFiltering(baseFolder, phrase); });
        updateGui();
    }
}


void MainDialog::filterExtension(const Zstring& extension, bool include)
{
    assert(!extension.empty());
    addFilterPhrase(Zstr("*.") + extension, include, false);
}


void MainDialog::filterShortname(const FileSystemObject& fsObj, bool include)
{
    Zstring phrase = Zstring(Zstr("*")) + FILE_NAME_SEPARATOR + fsObj.getItemNameAny();
    const bool isFolder = dynamic_cast<const FolderPair*>(&fsObj) != nullptr;
    if (isFolder)
        phrase += FILE_NAME_SEPARATOR;

    addFilterPhrase(phrase, include, true);
}


void MainDialog::filterItems(const std::vector<FileSystemObject*>& selection, bool include)
{
    if (!selection.empty())
    {
        Zstring phrase;
        for (auto it = selection.begin(); it != selection.end(); ++it)
        {
            FileSystemObject* fsObj = *it;

            if (it != selection.begin())
                phrase += Zstr('\n');

            //#pragma warning(suppress: 6011) -> fsObj bound in this context!
            phrase += FILE_NAME_SEPARATOR + fsObj->getRelativePathAny();

            const bool isFolder = dynamic_cast<const FolderPair*>(fsObj) != nullptr;
            if (isFolder)
                phrase += FILE_NAME_SEPARATOR;
        }
        addFilterPhrase(phrase, include, true);
    }
}


void MainDialog::onGridLabelContextC(GridLabelClickEvent& event)
{
    ContextMenu menu;

    const GridViewType viewType = m_bpButtonViewType->isActive() ? GridViewType::action : GridViewType::difference;
    menu.addItem(_("Difference") + (viewType != GridViewType::difference ? L"\tF11" : L""), [&] { setGridViewType(GridViewType::difference); }, greyScaleIfDisabled(loadImage("compare_sicon"   ), viewType == GridViewType::difference));
    menu.addItem(_("Action")     + (viewType != GridViewType::action     ? L"\tF11" : L""), [&] { setGridViewType(GridViewType::action    ); }, greyScaleIfDisabled(loadImage("start_sync_sicon"), viewType == GridViewType::action));
    menu.popup(*m_gridMainC, { m_gridMainC->GetSize().x, 0 });
}


void MainDialog::onGridLabelContextRim(GridLabelClickEvent& event, bool leftSide)
{
    ContextMenu menu;
    //--------------------------------------------------------------------------------------------------------
    Grid& grid = leftSide ? *m_gridMainL : *m_gridMainR;
    //const ColumnTypeRim colType = static_cast<ColumnTypeRim>(event.colType_);

    auto toggleColumn = [&](ColumnType ct)
    {
        auto colAttr = grid.getColumnConfig();

        Grid::ColAttributes* caItemPath = nullptr;
        Grid::ColAttributes* caToggle   = nullptr;

        for (Grid::ColAttributes& ca : colAttr)
            if (ca.type == static_cast<ColumnType>(ColumnTypeRim::path))
                caItemPath = &ca;
            else if (ca.type == ct)
                caToggle = &ca;

        assert(caItemPath && caItemPath->stretch > 0 && caItemPath->visible);
        assert(caToggle   && caToggle  ->stretch == 0);

        if (caItemPath && caToggle)
        {
            caToggle->visible = !caToggle->visible;

            //take width of newly visible column from stretched item path column
            caItemPath->offset -= caToggle->visible ? caToggle->offset : -caToggle->offset;

            grid.setColumnConfig(colAttr);
        }
    };

    if (const GridData* prov = grid.getDataProvider())
        for (const Grid::ColAttributes& ca : grid.getColumnConfig())
            menu.addCheckBox(prov->getColumnLabel(ca.type), [ct = ca.type, toggleColumn] { toggleColumn(ct); },
                             ca.visible, ca.type != static_cast<ColumnType>(ColumnTypeRim::path)); //do not allow user to hide this column!
    //----------------------------------------------------------------------------------------------
    menu.addSeparator();

    auto& itemPathFormat = leftSide ? globalCfg_.mainDlg.itemPathFormatLeftGrid : globalCfg_.mainDlg.itemPathFormatRightGrid;

    auto setItemPathFormat = [&](ItemPathFormat fmt)
    {
        itemPathFormat = fmt;
        filegrid::setItemPathForm(grid, fmt);
    };
    auto addFormatEntry = [&](const wxString& label, ItemPathFormat fmt)
    {
        menu.addRadio(label, [fmt, &setItemPathFormat] { setItemPathFormat(fmt); }, itemPathFormat == fmt);
    };
    addFormatEntry(_("Item name"    ), ItemPathFormat::name);
    addFormatEntry(_("Relative path"), ItemPathFormat::relative);
    addFormatEntry(_("Full path"    ), ItemPathFormat::full);

    //----------------------------------------------------------------------------------------------
    menu.addSeparator();

    auto setIconSize = [&](FileIconSize sz, bool showIcons)
    {
        globalCfg_.mainDlg.iconSize  = sz;
        globalCfg_.mainDlg.showIcons = showIcons;
        filegrid::setupIcons(*m_gridMainL, *m_gridMainC, *m_gridMainR, globalCfg_.mainDlg.showIcons, convert(globalCfg_.mainDlg.iconSize));
    };

    auto setDefault = [&]
    {
        const XmlGlobalSettings defaultCfg;

        grid.setColumnConfig(convertColAttributes(leftSide ? defaultCfg.mainDlg.columnAttribLeft : defaultCfg.mainDlg.columnAttribRight, defaultCfg.mainDlg.columnAttribLeft));

        setItemPathFormat(leftSide ? defaultCfg.mainDlg.itemPathFormatLeftGrid : defaultCfg.mainDlg.itemPathFormatRightGrid);

        setIconSize(defaultCfg.mainDlg.iconSize, defaultCfg.mainDlg.showIcons);
    };
    menu.addItem(_("&Default"), setDefault); //'&' -> reuse text from "default" buttons elsewhere
    //----------------------------------------------------------------------------------------------
    menu.addSeparator();
    menu.addCheckBox(_("Show icons:"), [&] { setIconSize(globalCfg_.mainDlg.iconSize, !globalCfg_.mainDlg.showIcons); }, globalCfg_.mainDlg.showIcons);

    auto addSizeEntry = [&](const wxString& label, FileIconSize sz)
    {
        menu.addRadio(label, [sz, &setIconSize] { setIconSize(sz, true /*showIcons*/); }, globalCfg_.mainDlg.iconSize == sz, globalCfg_.mainDlg.showIcons);
    };
    addSizeEntry(L"    " + _("Small" ), FileIconSize::small );
    addSizeEntry(L"    " + _("Medium"), FileIconSize::medium);
    addSizeEntry(L"    " + _("Large" ), FileIconSize::large );
    //----------------------------------------------------------------------------------------------
    //    if (type == ColumnTypeRim::date)
    {
        menu.addSeparator();

        auto selectTimeSpan = [&]
        {
            if (showSelectTimespanDlg(this, manualTimeSpanFrom_, manualTimeSpanTo_) == ConfirmationButton::accept)
            {
                applyTimeSpanFilter(folderCmp_, manualTimeSpanFrom_, manualTimeSpanTo_); //overwrite current active/inactive settings
                //updateGuiDelayedIf(!m_bpButtonShowExcluded->isActive()); //show update GUI before removing rows
                updateGui();
            }
        };
        menu.addItem(_("Select time span..."), selectTimeSpan);
    }
    //--------------------------------------------------------------------------------------------------------
    menu.popup(grid, { event.mousePos_.x, grid.getColumnLabelHeight() });
    //event.Skip();
}


void MainDialog::onOpenMenuTools(wxMenuEvent& event)
{
    //each layout menu item is either shown and owned by m_menuTools OR detached from m_menuTools and owned by detachedMenuItems_:
    auto filterLayoutItems = [&](wxMenuItem* menuItem, wxWindow* panelWindow)
    {
        wxAuiPaneInfo& paneInfo = this->auiMgr_.GetPane(panelWindow);
        if (paneInfo.IsShown())
        {
            if (!detachedMenuItems_.contains(menuItem))
                detachedMenuItems_.insert(m_menuTools->Remove(menuItem)); //pass ownership
        }
        else if (detachedMenuItems_.contains(menuItem))
        {
            detachedMenuItems_.erase(menuItem); //pass ownership
            m_menuTools->Append(menuItem);      //
        }
    };
    filterLayoutItems(m_menuItemShowMain,       m_panelTopButtons);
    filterLayoutItems(m_menuItemShowFolders,    m_panelDirectoryPairs);
    filterLayoutItems(m_menuItemShowViewFilter, m_panelViewFilter);
    filterLayoutItems(m_menuItemShowConfig,     m_panelConfig);
    filterLayoutItems(m_menuItemShowOverview,   m_gridOverview);
    event.Skip();
}


void MainDialog::resetLayout()
{
    m_splitterMain->setSashOffset(0);
    auiMgr_.LoadPerspective(defaultPerspective_, false /*don't call wxAuiManager::Update() => already done in updateGuiForFolderPair() */);
    updateGuiForFolderPair();
}


void MainDialog::onSetLayoutContext(wxMouseEvent& event)
{
    ContextMenu menu;

    menu.addItem(replaceCpy(_("&Reset layout"), L"&", L""), [&] { resetLayout(); }); //reuse translation from GUI builder
    //----------------------------------------------------------------------------------------

    bool addedSeparator = false;

    const wxAuiPaneInfoArray& paneArray = auiMgr_.GetAllPanes();
    for (size_t i = 0; i < paneArray.size(); ++i)
    {
        wxAuiPaneInfo& paneInfo = paneArray[i];
        if (!paneInfo.IsShown() &&
            paneInfo.window != compareStatus_->getAsWindow() &&
            paneInfo.window != m_panelLog                    &&
            paneInfo.window != m_panelSearch)
        {
            if (!addedSeparator)
            {
                menu.addSeparator();
                addedSeparator = true;
            }

            menu.addItem(replaceCpy(_("Show \"%x\""), L"%x", paneInfo.caption), [this, &paneInfo]
            {
                paneInfo.Show();
                this->auiMgr_.Update();
            });
        }
    }

    menu.popup(*this);
}


void MainDialog::onCompSettingsContext(wxEvent& event)
{
    ContextMenu menu;

    auto setVariant = [&](CompareVariant var)
    {
        currentCfg_.mainCfg.cmpCfg.compareVar = var;
        applyCompareConfig(true /*setDefaultViewType*/);
    };

    const CompareVariant activeCmpVar = getConfig().mainCfg.cmpCfg.compareVar;

    auto addVariantItem = [&](CompareVariant cmpVar, const char* iconName)
    {
        const wxImage imgSel = loadImage(iconName, -1 /*maxWidth*/, getDefaultMenuIconSize());

        menu.addItem(getVariantName(cmpVar), [&setVariant, cmpVar] { setVariant(cmpVar); }, greyScaleIfDisabled(imgSel, activeCmpVar == cmpVar));
    };
    addVariantItem(CompareVariant::timeSize, "cmp_time");
    addVariantItem(CompareVariant::content,  "cmp_content");
    addVariantItem(CompareVariant::size,     "cmp_size");

    menu.popup(*m_bpButtonCmpContext, { m_bpButtonCmpContext->GetSize().x, 0 });
}


void MainDialog::onSyncSettingsContext(wxEvent& event)
{
    ContextMenu menu;

    auto setVariant = [&](SyncVariant var)
    {
        currentCfg_.mainCfg.syncCfg.directionCfg.var = var;
        applySyncDirections();
    };

    const auto activeSyncVar = getConfig().mainCfg.syncCfg.directionCfg.var;

    auto addVariantItem = [&](SyncVariant syncVar, const char* iconName)
    {
        const wxImage imgSel = mirrorIfRtl(loadImage(iconName, -1 /*maxWidth*/, getDefaultMenuIconSize()));

        menu.addItem(getVariantName(syncVar), [&setVariant, syncVar] { setVariant(syncVar); }, greyScaleIfDisabled(imgSel, activeSyncVar == syncVar));
    };
    addVariantItem(SyncVariant::twoWay, "sync_twoway");
    addVariantItem(SyncVariant::mirror, "sync_mirror");
    addVariantItem(SyncVariant::update, "sync_update");
    addVariantItem(SyncVariant::custom, "sync_custom");

    menu.popup(*m_bpButtonSyncContext, { m_bpButtonSyncContext->GetSize().x, 0 });
}


void MainDialog::onDialogFilesDropped(FileDropEvent& event)
{
    assert(!event.itemPaths_.empty());
    loadConfiguration(event.itemPaths_);
    //event.Skip();
}


void MainDialog::onDirSelected(wxCommandEvent& event)
{
    //left and right directory text-control and folderSelector are synchronized by MainFolderDragDrop automatically
    clearGrid(); //disable the sync button
    event.Skip();
}


void MainDialog::onDirManualCorrection(wxCommandEvent& event)
{
    updateUnsavedCfgStatus();
    event.Skip();
}


void MainDialog::cfgHistoryRemoveObsolete(const std::vector<Zstring>& filePaths)
{
    auto getUnavailableCfgFilesAsync = [filePaths] //don't use wxString: NOT thread-safe! (e.g. non-atomic ref-count)
    {
        std::list<std::future<bool>> availableFiles; //check existence of all config files in parallel!

        for (const Zstring& filePath : filePaths)
            availableFiles.push_back(runAsync([=] { return fileAvailable(filePath); }));

        //potentially slow network access => limit maximum wait time!
        waitForAllTimed(availableFiles.begin(), availableFiles.end(), std::chrono::seconds(1));

        std::vector<Zstring> pathsToRemove;

        auto itFut = availableFiles.begin();
        for (auto it = filePaths.begin(); it != filePaths.end(); ++it, ++itFut)
            if (isReady(*itFut) && !itFut->get()) //remove only files that are confirmed to be non-existent
                pathsToRemove.push_back(*it); //file access error? probably not accessible network share or usb stick => remove cfg

        return pathsToRemove;
    };

    guiQueue_.processAsync(getUnavailableCfgFilesAsync, [this](const std::vector<Zstring>& filePaths2)
    {
        cfggrid::getDataView(*m_gridCfgHistory).removeItems(filePaths2);
        m_gridCfgHistory->Refresh();
    });
}


void MainDialog::updateUnsavedCfgStatus()
{
    const Zstring activeCfgFilePath = activeConfigFiles_.size() == 1 && !equalNativePath(activeConfigFiles_[0], lastRunConfigPath_) ? activeConfigFiles_[0] : Zstring();

    std::vector<std::wstring> jobNames;
    for (const Zstring& cfgFilePath : activeConfigFiles_)
        jobNames.push_back(equalNativePath(cfgFilePath, lastRunConfigPath_) ?
                           L'[' + _("Last session") + L']' :
                           extractJobName(cfgFilePath));

    const bool haveUnsavedCfg = lastSavedCfg_ != getConfig();

    //update save config button
    const bool allowSave = haveUnsavedCfg ||
                           activeConfigFiles_.size() > 1;

    auto makeBrightGrey = [](wxImage img)
    {
        img = img.ConvertToGreyscale(1.0/3, 1.0/3, 1.0/3); //treat all channels equally!
        brighten(img, 80);
        return img;
    };

    setImage(*m_bpButtonSave, allowSave ? loadImage("cfg_save") : makeBrightGrey(loadImage("cfg_save")));
    m_bpButtonSave->Enable(allowSave);
    m_menuItemSave->Enable(allowSave); //bitmap is automatically greyscaled on Win7 (introducing a crappy looking shift), but not on XP

    //set main dialog title
    wxString title;
    if (haveUnsavedCfg)
        title += L'*';

    if (!activeCfgFilePath.empty())
        title += utfTo<wxString>(activeCfgFilePath);
    else if (activeConfigFiles_.size() > 1)
    {
        title += jobNames[0];
        std::for_each(jobNames.begin() + 1, jobNames.end(), [&](const std::wstring& jobName)
        { title += L" + " + jobName; });
    }
    else
    {
        title += L"FreeFileSync " + utfTo<std::wstring>(ffsVersion);
        title += SPACED_DASH + _("Folder Comparison and Synchronization");
    }

    SetTitle(title);
}


void MainDialog::onConfigSave(wxCommandEvent& event)
{
    const Zstring activeCfgFilePath = activeConfigFiles_.size() == 1 && !equalNativePath(activeConfigFiles_[0], lastRunConfigPath_) ? activeConfigFiles_[0] : Zstring();

    //if we work on a single named configuration document: save directly if changed
    //else: always show file dialog
    if (activeCfgFilePath.empty())
        trySaveConfig(nullptr);
    else
        try
        {
            switch (getXmlType(activeCfgFilePath)) //throw FileError
            {
                case XmlType::gui:
                    trySaveConfig(&activeCfgFilePath);
                    break;
                case XmlType::batch:
                    trySaveBatchConfig(&activeCfgFilePath);
                    break;
                case XmlType::global:
                case XmlType::other:
                    showNotificationDialog(this, DialogInfoType::error,
                                           PopupDialogCfg().setDetailInstructions(replaceCpy(_("File %x does not contain a valid configuration."), L"%x", fmtPath(activeCfgFilePath))));
                    break;
            }
        }
        catch (const FileError& e)
        {
            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        }
}


bool MainDialog::trySaveConfig(const Zstring* guiCfgPath) //return true if saved successfully
{
    Zstring cfgFilePath;

    if (guiCfgPath)
    {
        cfgFilePath = *guiCfgPath;
        assert(endsWith(cfgFilePath, Zstr(".ffs_gui")));
    }
    else
    {
        const Zstring activeCfgFilePath = activeConfigFiles_.size() == 1 && !equalNativePath(activeConfigFiles_[0], lastRunConfigPath_) ? activeConfigFiles_[0] : Zstring();

        const std::optional<Zstring> defaultFolderPath = !activeCfgFilePath.empty() ?
                                                         getParentFolderPath(activeCfgFilePath) :
                                                         getParentFolderPath(globalCfg_.mainDlg.cfgFileLastSelected);

        Zstring defaultFileName = !activeCfgFilePath.empty() ?
                                  afterLast(activeCfgFilePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all) :
                                  Zstr("SyncSettings.ffs_gui");

        //attention: activeConfigFiles_ may be an imported ffs_batch file! We don't want to overwrite it with a GUI config!
        defaultFileName = beforeLast(defaultFileName, Zstr('.'), IfNotFoundReturn::all) + Zstr(".ffs_gui");

        wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), utfTo<wxString>(defaultFileName),
                                  wxString(L"FreeFileSync (*.ffs_gui)|*.ffs_gui") + L"|" +_("All files") + L" (*.*)|*",
                                  wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (fileSelector.ShowModal() != wxID_OK)
            return false;
        cfgFilePath = globalCfg_.mainDlg.cfgFileLastSelected = utfTo<Zstring>(fileSelector.GetPath());
    }

    const XmlGuiConfig guiCfg = getConfig();

    try
    {
        writeConfig(guiCfg, cfgFilePath); //throw FileError
        setLastUsedConfig(guiCfg, { cfgFilePath });

        flashStatusInformation(_("Configuration saved"));
        return true;
    }
    catch (const FileError& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        return false;
    }
}


bool MainDialog::trySaveBatchConfig(const Zstring* batchCfgPath)
{
    //essentially behave like trySaveConfig(): the collateral damage of not saving GUI-only settings "m_bpButtonViewType" is negligible

    const Zstring activeCfgFilePath = activeConfigFiles_.size() == 1 && !equalNativePath(activeConfigFiles_[0], lastRunConfigPath_) ? activeConfigFiles_[0] : Zstring();

    //prepare batch config: reuse existing batch-specific settings from file if available
    BatchExclusiveConfig batchExCfg;
    try
    {
        Zstring referenceBatchFile;
        if (batchCfgPath)
            referenceBatchFile = *batchCfgPath;
        else if (!activeCfgFilePath.empty())
            if (getXmlType(activeCfgFilePath) == XmlType::batch) //throw FileError
                referenceBatchFile = activeCfgFilePath;

        if (!referenceBatchFile.empty())
        {
            XmlBatchConfig referenceBatchCfg;

            std::wstring warningMsg;
            readConfig(referenceBatchFile, referenceBatchCfg, warningMsg); //throw FileError
            //=> ignore warnings altogether: user has seen them already when loading the config file!
            batchExCfg = referenceBatchCfg.batchExCfg;
        }
    }
    catch (const FileError& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        return false;
    }

    Zstring cfgFilePath;
    if (batchCfgPath)
    {
        cfgFilePath = *batchCfgPath;
        assert(endsWith(cfgFilePath, Zstr(".ffs_batch")));
    }
    else
    {
        //let user update batch config: this should change batch-exclusive settings only, else the "setLastUsedConfig" below would be somewhat of a lie
        if (showBatchConfigDialog(this,
                                  batchExCfg,
                                  currentCfg_.mainCfg.ignoreErrors) != ConfirmationButton::accept)
            return false;
        updateUnsavedCfgStatus(); //nothing else to update on GUI!

        const std::optional<Zstring> defaultFolderPath = !activeCfgFilePath.empty() ?
                                                         getParentFolderPath(activeCfgFilePath) :
                                                         getParentFolderPath(globalCfg_.mainDlg.cfgFileLastSelected);

        Zstring defaultFileName = !activeCfgFilePath.empty() ?
                                  afterLast(activeCfgFilePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all) :
                                  Zstr("BatchRun.ffs_batch");

        //attention: activeConfigFiles_ may be an ffs_gui file! We don't want to overwrite it with a BATCH config!
        defaultFileName = beforeLast(defaultFileName, Zstr('.'), IfNotFoundReturn::all) + Zstr(".ffs_batch");

        wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), utfTo<wxString>(defaultFileName),
                                  _("FreeFileSync batch") + L" (*.ffs_batch)|*.ffs_batch" + L"|" +_("All files") + L" (*.*)|*",
                                  wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (fileSelector.ShowModal() != wxID_OK)
            return false;
        cfgFilePath = globalCfg_.mainDlg.cfgFileLastSelected = utfTo<Zstring>(fileSelector.GetPath());
    }

    const XmlGuiConfig guiCfg = getConfig();
    const XmlBatchConfig batchCfg = convertGuiToBatch(guiCfg, batchExCfg);

    try
    {
        writeConfig(batchCfg, cfgFilePath); //throw FileError
        setLastUsedConfig(guiCfg, { cfgFilePath }); //[!] behave as if we had saved guiCfg

        flashStatusInformation(_("Configuration saved"));
        return true;
    }
    catch (const FileError& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        return false;
    }
}


bool MainDialog::saveOldConfig() //return false on user abort
{
    const XmlGuiConfig guiCfg = getConfig();

    if (lastSavedCfg_ != guiCfg)
    {
        const Zstring activeCfgFilePath = activeConfigFiles_.size() == 1 && !equalNativePath(activeConfigFiles_[0], lastRunConfigPath_) ? activeConfigFiles_[0] : Zstring();

        //notify user about changed settings
        if (globalCfg_.confirmDlgs.confirmSaveConfig)
            if (!activeCfgFilePath.empty())
                //only if check is active and non-default config file loaded
            {
                bool neverSaveChanges = false;
                switch (showQuestionDialog(this, DialogInfoType::info, PopupDialogCfg().
                                           setTitle(utfTo<wxString>(activeCfgFilePath)).
                                           setMainInstructions(replaceCpy(_("Do you want to save changes to %x?"), L"%x",
                                                                          fmtPath(afterLast(activeCfgFilePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all)))).
                                           setCheckBox(neverSaveChanges, _("Never save &changes"), static_cast<ConfirmationButton3>(QuestionButton2::yes)),
                                           _("&Save"), _("Do&n't save")))
                {
                    case QuestionButton2::yes: //save
                        try
                        {
                            switch (getXmlType(activeCfgFilePath)) //throw FileError
                            {
                                case XmlType::gui:
                                    return trySaveConfig(&activeCfgFilePath);
                                case XmlType::batch:
                                    return trySaveBatchConfig(&activeCfgFilePath);
                                case XmlType::global:
                                case XmlType::other:
                                    showNotificationDialog(this, DialogInfoType::error,
                                                           PopupDialogCfg().setDetailInstructions(replaceCpy(_("File %x does not contain a valid configuration."), L"%x", fmtPath(activeCfgFilePath))));
                                    return false;
                            }
                        }
                        catch (const FileError& e)
                        {
                            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
                            return false;
                        }
                        break;

                    case QuestionButton2::no: //don't save
                        globalCfg_.confirmDlgs.confirmSaveConfig = !neverSaveChanges;
                        break;

                    case QuestionButton2::cancel:
                        return false;
                }
            }
        //user doesn't save changes =>
        //discard current reference file(s), this ensures next app start will load [Last session] instead of the original non-modified config selection
        setLastUsedConfig(guiCfg, {} /*cfgFilePaths*/);
        //this seems to make theoretical sense also: the job of this function is to make sure, current (volatile) config and reference file name are in sync
        // => if user does not save cfg, it is not attached to a physical file anymore!
    }
    return true;
}


void MainDialog::onConfigLoad(wxCommandEvent& event)
{
    std::optional<Zstring> defaultFolderPath = getParentFolderPath(globalCfg_.mainDlg.cfgFileLastSelected);

    wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), wxString() /*default file name*/,
                              wxString(L"FreeFileSync (*.ffs_gui; *.ffs_batch)|*.ffs_gui;*.ffs_batch") + L"|" +_("All files") + L" (*.*)|*",
                              wxFD_OPEN | wxFD_MULTIPLE);
    if (fileSelector.ShowModal() != wxID_OK)
        return;

    wxArrayString tmp;
    fileSelector.GetPaths(tmp);

    std::vector<Zstring> filePaths;
    for (const wxString& path : tmp)
        filePaths.push_back(utfTo<Zstring>(path));

    if (!filePaths.empty())
        globalCfg_.mainDlg.cfgFileLastSelected = filePaths[0];

    assert(!filePaths.empty());
    loadConfiguration(filePaths);
}


void MainDialog::onCfgGridSelection(GridSelectEvent& event)
{
    std::vector<Zstring> filePaths;
    for (size_t row : m_gridCfgHistory->getSelectedRows())
        if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(row))
            filePaths.push_back(cfg->cfgItem.cfgFilePath);
        else
            assert(false);

    if (filePaths.empty() || //ignore accidental clicks in empty space of configuration panel
        !loadConfiguration(filePaths))
        //user changed m_gridCfgHistory selection so it's this method's responsibility to synchronize with activeConfigFiles:
        //- if user cancelled saving old config
        //- there's an error loading new config
        cfggrid::addAndSelect(*m_gridCfgHistory, activeConfigFiles_, false /*scrollToSelection*/);

    event.Skip();
}


void MainDialog::onCfgGridDoubleClick(GridClickEvent& event)
{
    if (!activeConfigFiles_.empty())
    {
        wxCommandEvent dummy(wxEVT_COMMAND_BUTTON_CLICKED);
        m_buttonCompare->Command(dummy); //simulate click
    }
}


void MainDialog::onConfigNew(wxCommandEvent& event)
{
    loadConfiguration({});
}


bool MainDialog::loadConfiguration(const std::vector<Zstring>& filePaths)
{
    if (!saveOldConfig())
        return false; //cancelled by user

    XmlGuiConfig newGuiCfg; //contains default values

    //add default exclusion filter: this is only ever relevant when creating new configurations!
    //a default XmlGuiConfig does not need these user-specific exclusions!
    Zstring& excludeFilter = newGuiCfg.mainCfg.globalFilter.excludeFilter;
    if (!excludeFilter.empty() && !endsWith(excludeFilter, Zstr('\n')))
        excludeFilter += Zstr('\n');
    excludeFilter += globalCfg_.defaultExclusionFilter;

    if (!filePaths.empty()) //empty cfg file list means "use default"
        try
        {
            //allow reading batch configurations also
            std::wstring warningMsg;
            readAnyConfig(filePaths, newGuiCfg, warningMsg); //throw FileError

            if (!warningMsg.empty())
            {
                showNotificationDialog(this, DialogInfoType::warning, PopupDialogCfg().setDetailInstructions(warningMsg));
                setConfig(newGuiCfg, filePaths);
                setLastUsedConfig(XmlGuiConfig(), filePaths); //simulate changed config due to parsing errors
                return true;
            }
        }
        catch (const FileError& e)
        {
            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
            return false;
        }

    setConfig(newGuiCfg, filePaths);
    //flashStatusInformation("Configuration loaded"); -> irrelevant!?
    return true;
}


void MainDialog::removeSelectedCfgHistoryItems(bool deleteFromDisk)
{
    const std::vector<size_t> selectedRows = m_gridCfgHistory->getSelectedRows();
    if (!selectedRows.empty())
    {
        std::vector<Zstring> filePaths;
        for (size_t row : selectedRows)
            if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(row))
                filePaths.push_back(cfg->cfgItem.cfgFilePath);
            else
                assert(false);

        if (deleteFromDisk)
        {
            std::wstring fileList;
            for (const Zstring& filePath : filePaths)
                fileList += utfTo<std::wstring>(filePath) + L'\n';

            FocusPreserver fp;

            bool moveToRecycler = true;
            if (showDeleteDialog(this, fileList, static_cast<int>(filePaths.size()),
                                 moveToRecycler) != ConfirmationButton::accept)
                return;

            std::vector<Zstring> deletedPaths;
            std::optional<FileError> firstError;

            for (const Zstring& filePath : filePaths)
                try
                {
                    AbstractPath cfgPath = createItemPathNative(filePath);

                    if (moveToRecycler)
                        AFS::recycleItemIfExists(cfgPath); //throw FileError
                    else
                        AFS::removeFileIfExists(cfgPath); //throw FileError

                    deletedPaths.push_back(filePath);
                }
                catch (const FileError& e) { if (!firstError) firstError = e; }

            if (firstError)
                showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(firstError->toString()));
            filePaths = deletedPaths;
        }

        //FIRST: discard unsaved changes (*before* removing cfg items) => no point in saving before removing, right?
        setLastUsedConfig(getConfig(), {} /*cfgFilePaths*/);

        cfggrid::getDataView(*m_gridCfgHistory).removeItems(filePaths);
        m_gridCfgHistory->Refresh(); //grid size changed => clears selection!

        //set active selection on next item to allow "batch-deletion" by holding down DEL key
        //user expects that selected config is also loaded: https://freefilesync.org/forum/viewtopic.php?t=5723
        //  => deleteFromDisk failed? still select selectedRows.front()!
        std::vector<Zstring> nextCfgPaths;
        if (m_gridCfgHistory->getRowCount() > 0)
        {
            const size_t nextRow = std::min(selectedRows.front(), m_gridCfgHistory->getRowCount() - 1);
            if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(nextRow))
                nextCfgPaths.push_back(cfg->cfgItem.cfgFilePath);
        }

        if (!loadConfiguration(nextCfgPaths))
            setLastUsedConfig(lastSavedCfg_, {}); //error/(cancel) => clear activeConfigFiles_ so that old configs don't reappear after restart
    }
}


void MainDialog::renameSelectedCfgHistoryItem()
{
    const std::vector<size_t> selectedRows = m_gridCfgHistory->getSelectedRows();
    if (!selectedRows.empty())
    {
        const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(selectedRows[0]);
        assert(cfg);
        if (!cfg)
            return;

        if (cfg->isLastRunCfg)
            return showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(
                                              replaceCpy(_("%x cannot be renamed."), L"%x", fmtPath(cfg->name))));

        const Zstring cfgPathOld = cfg->cfgItem.cfgFilePath;

        //FIRST: 1. consolidate unsaved changes using the *old* config file name, if any!
        //2. get rid of multiple-selection if exists 3. load cfg to allow non-failing(!) setLastUsedConfig() below
        if (!loadConfiguration({ cfgPathOld }))
            return; //error/cancel

        const Zstring fileName     =  afterLast(cfgPathOld, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);
        /**/  Zstring folderPathPf = beforeLast(cfgPathOld, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
        if (!folderPathPf.empty())
            folderPathPf += FILE_NAME_SEPARATOR;

        const Zstring cfgNameOld = beforeLast(fileName, Zstr('.'), IfNotFoundReturn::all);
        /**/  Zstring cfgDotExt  =  afterLast(fileName, Zstr('.'), IfNotFoundReturn::none);
        if (!cfgDotExt.empty())
            cfgDotExt = Zstr('.') + cfgDotExt;

        for (;;)
        {
            wxTextEntryDialog cfgRenameDlg(this, _("New name:"), _("Rename Configuration"), utfTo<wxString>(cfgNameOld));

            wxTextValidator inputValidator(wxFILTER_EXCLUDE_CHAR_LIST);
            inputValidator.SetCharExcludes(LR"(<>:"/\|?*)"); //forbidden chars for file names (at least on Windows)
            //https://docs.microsoft.com/de-de/windows/win32/fileio/naming-a-file#naming-conventions
            cfgRenameDlg.SetTextValidator(inputValidator);

            if (cfgRenameDlg.ShowModal() != wxID_OK)
                return;

            const Zstring cfgNameNew = utfTo<Zstring>(trimCpy(cfgRenameDlg.GetValue()));
            if (cfgNameNew == cfgNameOld)
                return;

            const Zstring cfgPathNew = folderPathPf + cfgNameNew + cfgDotExt;
            try
            {
                if (cfgNameNew.empty()) //better error message + check than wxFILTER_EMPTY, e.g. trimCpy()!
                    throw FileError(_("Configuration name must not be empty."));

                moveAndRenameItem(cfgPathOld, cfgPathNew, false /*replaceExisting*/); //throw FileError, (ErrorMoveUnsupported), ErrorTargetExisting
            }
            catch (const FileError& e)
            {
                showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
                continue;
            }

            cfggrid::getDataView(*m_gridCfgHistory).removeItems({ cfgPathOld });
            m_gridCfgHistory->Refresh(); //grid size changed => clears selection!

            //keep current cfg and just swap the file name: see previous "loadConfiguration({ cfgPathOld }"!
            setLastUsedConfig(lastSavedCfg_, { cfgPathNew });
            return;
        }
    }
}


void MainDialog::onCfgGridKeyEvent(wxKeyEvent& event)
{
    const int keyCode = event.GetKeyCode();
    switch (keyCode)
    {
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (!activeConfigFiles_.empty())
            {
                wxCommandEvent dummy(wxEVT_COMMAND_BUTTON_CLICKED);
                m_buttonCompare->Command(dummy); //simulate click
            }
            break;

        case WXK_DELETE:
        case WXK_NUMPAD_DELETE:
            removeSelectedCfgHistoryItems(event.ShiftDown() /*deleteFromDisk*/);
            return; //"swallow" event

        case WXK_F2:
        case WXK_NUMPAD_F2:
            renameSelectedCfgHistoryItem();
            return; //"swallow" event
    }
    event.Skip();
}


void MainDialog::onCfgGridContext(GridContextMenuEvent& event)
{
    ContextMenu menu;
    const std::vector<size_t> selectedRows = m_gridCfgHistory->getSelectedRows();

    //--------------------------------------------------------------------------------------------------------
    const bool renameEnabled = [&]
    {
        if (!selectedRows.empty())
            if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(selectedRows[0]))
                return !cfg->isLastRunCfg;
        return false;
    }();
    menu.addItem(_("&Rename...") + L"\tF2",  [this] { renameSelectedCfgHistoryItem (); }, wxNullImage, renameEnabled);
    //--------------------------------------------------------------------------------------------------------
    ContextMenu submenu;

    auto addColorOption = [&](const wxColor& col, const wxString& name)
    {
        auto applyBackColor = [this, col, &selectedRows]
        {
            std::vector<Zstring> filePaths;
            for (size_t row : selectedRows)
                if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(row))
                    filePaths.push_back(cfg->cfgItem.cfgFilePath);
                else
                    assert(false);

            cfggrid::getDataView(*m_gridCfgHistory).setBackColor(filePaths, col);

            //re-apply selection (after sorting by color tags):
            cfggrid::addAndSelect(*m_gridCfgHistory, activeConfigFiles_, false /*scrollToSelection*/);
            //m_gridCfgHistory->Refresh(); <- implicit in last call
        };

        wxBitmap bmpSquare(this->GetCharHeight(), this->GetCharHeight()); //seems we don't need to pass 24-bit depth here even for high-contrast color schemes
        {
            wxMemoryDC dc(bmpSquare);
            const wxColor borderCol(0xdd, 0xdd, 0xdd); //light grey
            const wxColor fillCol = col.Ok() ? col : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
            drawFilledRectangle(dc, wxRect(bmpSquare.GetSize()), fastFromDIP(1), borderCol, fillCol);
        }
        submenu.addItem(name, applyBackColor, bmpSquare.ConvertToImage(), !selectedRows.empty());
    };
    addColorOption(wxNullColour, L'(' + _("&Default") + L')'); //meta options should be enclosed in parentheses
    addColorOption({ 0xff, 0xd8, 0xcb }, _("Red"));
    addColorOption({ 0xff, 0xf9, 0x99 }, _("Yellow"));
    addColorOption({ 0xcc, 0xff, 0x99 }, _("Green"));
    addColorOption({ 0xcc, 0xff, 0xff }, _("Cyan"));
    addColorOption({ 0xcc, 0xcc, 0xff }, _("Blue"));
    addColorOption({ 0xf2, 0xcb, 0xff }, _("Purple"));
    addColorOption({ 0xdd, 0xdd, 0xdd }, _("Grey"));

    menu.addSubmenu(_("Background color"), submenu, loadImage("color_sicon"), !selectedRows.empty());
    menu.addSeparator();
    //--------------------------------------------------------------------------------------------------------
    const XmlGlobalSettings defaultCfg;
    assert(!defaultCfg.externalApps.empty());

    auto showInFileManager = [&]
    {
        if (!selectedRows.empty())
            if (const ConfigView::Details* cfg = cfggrid::getDataView(*m_gridCfgHistory).getItem(selectedRows[0]))
            {
                const Zstring commandLinePhrase = defaultCfg.externalApps[0].cmdLine;
                const Zstring cmdLine = replaceCpy(expandMacros(commandLinePhrase), Zstr("%local_path%"), cfg->cfgItem.cfgFilePath);
                try
                {
                    if (const auto& [exitCode, output] = consoleExecute(cmdLine, EXT_APP_MAX_TOTAL_WAIT_TIME_MS); //throw SysError, SysErrorTimeOut
                        exitCode != 0)
                        throw SysError(formatSystemError(utfTo<std::string>(commandLinePhrase), replaceCpy(_("Exit code %x"), L"%x", numberTo<std::wstring>(exitCode)), output));
                }
                catch (SysErrorTimeOut&) {} //child process not failed yet => probably fine :>
                catch (const SysError& e)
                {
                    const std::wstring errorMsg = replaceCpy(_("Command %x failed."), L"%x", fmtPath(cmdLine)) + L"\n\n" + e.toString();
                    showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(errorMsg));
                }
                return;
            }
        assert(false);
    };
    menu.addItem(translate(defaultCfg.externalApps[0].description), //translate default external apps on the fly: "Show in Explorer"
                 showInFileManager, wxNullImage, !selectedRows.empty());
    menu.addSeparator();
    //--------------------------------------------------------------------------------------------------------
    menu.addItem(_("Hide configuration") + L"\tDel", [this] { removeSelectedCfgHistoryItems(false /*deleteFromDisk*/); }, wxNullImage,    !selectedRows.empty());
    menu.addItem(_("&Delete") +      L"\tShift+Del", [this] { removeSelectedCfgHistoryItems(true  /*deleteFromDisk*/); }, imgTrashSmall_, !selectedRows.empty());
    //--------------------------------------------------------------------------------------------------------
    menu.popup(*m_gridCfgHistory);
    //event.Skip();
}


void MainDialog::onCfgGridLabelContext(GridLabelClickEvent& event)
{
    ContextMenu menu;
    //--------------------------------------------------------------------------------------------------------
    auto toggleColumn = [&](ColumnType ct)
    {
        auto colAttr = m_gridCfgHistory->getColumnConfig();

        Grid::ColAttributes* caName   = nullptr;
        Grid::ColAttributes* caToggle = nullptr;

        for (Grid::ColAttributes& ca : colAttr)
            if (ca.type == static_cast<ColumnType>(ColumnTypeCfg::name))
                caName = &ca;
            else if (ca.type == ct)
                caToggle = &ca;

        assert(caName && caName->stretch > 0 && caName->visible);
        assert(caToggle && caToggle->stretch == 0);

        if (caName && caToggle)
        {
            caToggle->visible = !caToggle->visible;

            //take width of newly visible column from stretched folder name column
            caName->offset -= caToggle->visible ? caToggle->offset : -caToggle->offset;

            m_gridCfgHistory->setColumnConfig(colAttr);
        }
    };

    if (auto prov = m_gridCfgHistory->getDataProvider())
        for (const Grid::ColAttributes& ca : m_gridCfgHistory->getColumnConfig())
            menu.addCheckBox(prov->getColumnLabel(ca.type), [ct = ca.type, toggleColumn] { toggleColumn(ct); },
                             ca.visible, ca.type != static_cast<ColumnType>(ColumnTypeCfg::name)); //do not allow user to hide name column!
    else assert(false);
    //--------------------------------------------------------------------------------------------------------
    menu.addSeparator();

    auto setDefault = [&]
    {
        const XmlGlobalSettings defaultCfg;
        m_gridCfgHistory->setColumnConfig(convertColAttributes(defaultCfg.mainDlg.cfgGridColumnAttribs, getCfgGridDefaultColAttribs()));
    };
    menu.addItem(_("&Default"), setDefault); //'&' -> reuse text from "default" buttons elsewhere
    //--------------------------------------------------------------------------------------------------------
    menu.addSeparator();

    auto setCfgHighlight = [&]
    {
        int cfgGridSyncOverdueDays = cfggrid::getSyncOverdueDays(*m_gridCfgHistory);

        if (showCfgHighlightDlg(this, cfgGridSyncOverdueDays) == ConfirmationButton::accept)
            cfggrid::setSyncOverdueDays(*m_gridCfgHistory, cfgGridSyncOverdueDays);
    };
    menu.addItem(_("Highlight..."), setCfgHighlight);
    //--------------------------------------------------------------------------------------------------------

    menu.popup(*m_gridCfgHistory, { event.mousePos_.x, m_gridCfgHistory->getColumnLabelHeight() });
    //event.Skip();
}


void MainDialog::onCfgGridLabelLeftClick(GridLabelClickEvent& event)
{
    const auto colType = static_cast<ColumnTypeCfg>(event.colType_);
    bool sortAscending = getDefaultSortDirection(colType);

    const auto [sortCol, ascending] = cfggrid::getDataView(*m_gridCfgHistory).getSortDirection();
    if (sortCol == colType)
        sortAscending = !ascending;

    cfggrid::getDataView(*m_gridCfgHistory).setSortDirection(colType, sortAscending);
    m_gridCfgHistory->Refresh();

    //re-apply selection:
    cfggrid::addAndSelect(*m_gridCfgHistory, activeConfigFiles_, false /*scrollToSelection*/);
}


void MainDialog::onCheckRows(CheckRowsEvent& event)
{
    std::vector<size_t> selectedRows;

    const size_t rowLast = std::min(event.rowLast_, filegrid::getDataView(*m_gridMainC).rowsOnView()); //consider dummy rows
    for (size_t i = event.rowFirst_; i < rowLast; ++i)
        selectedRows.push_back(i);

    if (!selectedRows.empty())
    {
        std::vector<FileSystemObject*> objects = filegrid::getDataView(*m_gridMainC).getAllFileRef(selectedRows);
        setIncludedManually(objects, event.setActive_);
    }
}


void MainDialog::onSetSyncDirection(SyncDirectionEvent& event)
{
    std::vector<size_t> selectedRows;

    const size_t rowLast = std::min(event.rowLast_, filegrid::getDataView(*m_gridMainC).rowsOnView()); //consider dummy rows
    for (size_t i = event.rowFirst_; i < rowLast; ++i)
        selectedRows.push_back(i);

    if (!selectedRows.empty())
    {
        std::vector<FileSystemObject*> objects = filegrid::getDataView(*m_gridMainC).getAllFileRef(selectedRows);
        setSyncDirManually(objects, event.direction_);
    }
}


void MainDialog::setLastUsedConfig(const XmlGuiConfig& guiConfig, const std::vector<Zstring>& cfgFilePaths)
{
    activeConfigFiles_ = cfgFilePaths;
    lastSavedCfg_ = guiConfig;

    cfggrid::addAndSelect(*m_gridCfgHistory, activeConfigFiles_, true /*scrollToSelection*/); //put filepath on list of last used config files

    updateUnsavedCfgStatus();
}


void MainDialog::setConfig(const XmlGuiConfig& newGuiCfg, const std::vector<Zstring>& referenceFiles)
{
    currentCfg_ = newGuiCfg;

    //evaluate new settings...

    //(re-)set view filter buttons
    setViewFilterDefault();

    updateGlobalFilterButton();

    //set first folder pair
    firstFolderPair_->setValues(currentCfg_.mainCfg.firstPair);

    //folderHistoryLeft->addItem(currentCfg.mainCfg.firstPair.leftDirectory);
    //folderHistoryRight->addItem(currentCfg.mainCfg.firstPair.rightDirectory);

    setAddFolderPairs(currentCfg_.mainCfg.additionalPairs);

    setGridViewType(currentCfg_.gridViewType);

    clearGrid(); //+ update GUI!

    setLastUsedConfig(newGuiCfg, referenceFiles);
}


XmlGuiConfig MainDialog::getConfig() const
{
    XmlGuiConfig guiCfg = currentCfg_;

    //load settings whose ownership lies not in currentCfg:

    //first folder pair
    guiCfg.mainCfg.firstPair = firstFolderPair_->getValues();

    //add additional pairs
    guiCfg.mainCfg.additionalPairs.clear();

    for (const FolderPairPanel* panel : additionalFolderPairs_)
        guiCfg.mainCfg.additionalPairs.push_back(panel->getValues());

    //sync preview
    guiCfg.gridViewType = m_bpButtonViewType->isActive() ? GridViewType::action : GridViewType::difference;

    return guiCfg;
}


void MainDialog::updateGuiDelayedIf(bool condition)
{
    if (condition)
    {
        filegrid::refresh(*m_gridMainL, *m_gridMainC, *m_gridMainR);
        m_gridMainL->Update();
        m_gridMainC->Update();
        m_gridMainR->Update();

        //some delay to show the changed GUI before removing rows from sight
        std::this_thread::sleep_for(FILE_GRID_POST_UPDATE_DELAY);
    }

    updateGui();
}


void MainDialog::showConfigDialog(SyncConfigPanel panelToShow, int localPairIndexToShow)
{
    GlobalPairConfig globalPairCfg;
    globalPairCfg.cmpCfg  = currentCfg_.mainCfg.cmpCfg;
    globalPairCfg.syncCfg = currentCfg_.mainCfg.syncCfg;
    globalPairCfg.filter  = currentCfg_.mainCfg.globalFilter;

    globalPairCfg.miscCfg.deviceParallelOps      = currentCfg_.mainCfg.deviceParallelOps;
    globalPairCfg.miscCfg.ignoreErrors           = currentCfg_.mainCfg.ignoreErrors;
    globalPairCfg.miscCfg.autoRetryCount    = currentCfg_.mainCfg.autoRetryCount;
    globalPairCfg.miscCfg.autoRetryDelay    = currentCfg_.mainCfg.autoRetryDelay;
    globalPairCfg.miscCfg.postSyncCommand        = currentCfg_.mainCfg.postSyncCommand;
    globalPairCfg.miscCfg.postSyncCondition      = currentCfg_.mainCfg.postSyncCondition;
    globalPairCfg.miscCfg.altLogFolderPathPhrase = currentCfg_.mainCfg.altLogFolderPathPhrase;
    globalPairCfg.miscCfg.emailNotifyAddress     = currentCfg_.mainCfg.emailNotifyAddress;
    globalPairCfg.miscCfg.emailNotifyCondition   = currentCfg_.mainCfg.emailNotifyCondition;

    //don't recalculate value but consider current screen status!!!
    //e.g. it's possible that the first folder pair local config is shown with all config initial if user just removed local config via mouse context menu!
    const bool showMultipleCfgs = m_bpButtonLocalCompCfg->IsShown();
    //harmonize with MainDialog::updateGuiForFolderPair()!

    assert(showMultipleCfgs || localPairIndexToShow == -1);
    assert(m_bpButtonLocalCompCfg->IsShown() == m_bpButtonLocalSyncCfg->IsShown() &&
           m_bpButtonLocalCompCfg->IsShown() == m_bpButtonLocalFilter ->IsShown());

    std::vector<LocalPairConfig> localCfgs; //showSyncConfigDlg() needs *all* folder pairs for deviceParallelOps update
    localCfgs.push_back(firstFolderPair_->getValues());

    for (const FolderPairPanel* panel : additionalFolderPairs_)
        localCfgs.push_back(panel->getValues());

    //------------------------------------------------------------------------------------
    const GlobalPairConfig             globalPairCfgOld = globalPairCfg;
    const std::vector<LocalPairConfig> localPairCfgOld  = localCfgs;

    if (showSyncConfigDlg(this,
                          panelToShow,
                          showMultipleCfgs ? localPairIndexToShow : -1,
                          showMultipleCfgs,
                          globalPairCfg,
                          localCfgs,
                          globalCfg_.versioningFolderHistory, globalCfg_.versioningFolderLastSelected,
                          globalCfg_.logFolderHistory,        globalCfg_.logFolderLastSelected,
                          globalCfg_.folderHistoryMax,
                          globalCfg_.sftpKeyFileLastSelected,
                          globalCfg_.emailHistory,   globalCfg_.emailHistoryMax,
                          globalCfg_.commandHistory, globalCfg_.commandHistoryMax) != ConfirmationButton::accept)
        return;

    assert(localCfgs.size() == localPairCfgOld.size());

    currentCfg_.mainCfg.cmpCfg       = globalPairCfg.cmpCfg;
    currentCfg_.mainCfg.syncCfg      = globalPairCfg.syncCfg;
    currentCfg_.mainCfg.globalFilter = globalPairCfg.filter;

    currentCfg_.mainCfg.deviceParallelOps      = globalPairCfg.miscCfg.deviceParallelOps;
    currentCfg_.mainCfg.ignoreErrors           = globalPairCfg.miscCfg.ignoreErrors;
    currentCfg_.mainCfg.autoRetryCount    = globalPairCfg.miscCfg.autoRetryCount;
    currentCfg_.mainCfg.autoRetryDelay    = globalPairCfg.miscCfg.autoRetryDelay;
    currentCfg_.mainCfg.postSyncCommand        = globalPairCfg.miscCfg.postSyncCommand;
    currentCfg_.mainCfg.postSyncCondition      = globalPairCfg.miscCfg.postSyncCondition;
    currentCfg_.mainCfg.altLogFolderPathPhrase = globalPairCfg.miscCfg.altLogFolderPathPhrase;
    currentCfg_.mainCfg.emailNotifyAddress     = globalPairCfg.miscCfg.emailNotifyAddress;
    currentCfg_.mainCfg.emailNotifyCondition   = globalPairCfg.miscCfg.emailNotifyCondition;

    firstFolderPair_->setValues(localCfgs[0]);

    for (size_t i = 1; i < localCfgs.size(); ++i)
        additionalFolderPairs_[i - 1]->setValues(localCfgs[i]);

    //------------------------------------------------------------------------------------

    const bool cmpConfigChanged = globalPairCfg.cmpCfg != globalPairCfgOld.cmpCfg || [&]
    {
        for (size_t i = 0; i < localCfgs.size(); ++i)
            if (localCfgs[i].localCmpCfg != localPairCfgOld[i].localCmpCfg)
                return true;
        return false;
    }();

    //[!] don't redetermine sync directions if only options for deletion handling or versioning are changed!!!
    const bool syncDirectionsChanged = globalPairCfg.syncCfg.directionCfg != globalPairCfgOld.syncCfg.directionCfg || [&]
    {
        for (size_t i = 0; i < localCfgs.size(); ++i)
            if (static_cast<bool>(localCfgs[i].localSyncCfg) != static_cast<bool>(localPairCfgOld[i].localSyncCfg) ||
                (localCfgs[i].localSyncCfg && localCfgs[i].localSyncCfg->directionCfg != localPairCfgOld[i].localSyncCfg->directionCfg))
                return true;
        return false;
    }();

    const bool filterConfigChanged = globalPairCfg.filter != globalPairCfgOld.filter || [&]
    {
        for (size_t i = 0; i < localCfgs.size(); ++i)
            if (localCfgs[i].localFilter != localPairCfgOld[i].localFilter)
                return true;
        return false;
    }();

    //const bool miscConfigChanged = globalPairCfg.miscCfg.deviceParallelOps      != globalPairCfgOld.miscCfg.deviceParallelOps   ||
    //                               globalPairCfg.miscCfg.ignoreErrors           != globalPairCfgOld.miscCfg.ignoreErrors        ||
    //                               globalPairCfg.miscCfg.autoRetryCount    != globalPairCfgOld.miscCfg.autoRetryCount ||
    //                               globalPairCfg.miscCfg.autoRetryDelay    != globalPairCfgOld.miscCfg.autoRetryDelay ||
    //                               globalPairCfg.miscCfg.postSyncCommand        != globalPairCfgOld.miscCfg.postSyncCommand     ||
    //                               globalPairCfg.miscCfg.postSyncCondition      != globalPairCfgOld.miscCfg.postSyncCondition   ||
    //                               globalPairCfg.miscCfg.altLogFolderPathPhrase != globalPairCfgOld.miscCfg.altLogFolderPathPhrase ||
    //                               globalPairCfg.miscCfg.emailNotifyAddress     != globalPairCfgOld.miscCfg.emailNotifyAddress  ||
    //                               globalPairCfg.miscCfg.emailNotifyCondition   != globalPairCfgOld.miscCfg.emailNotifyCondition;

    if (cmpConfigChanged)
        applyCompareConfig(globalPairCfg.cmpCfg.compareVar != globalPairCfgOld.cmpCfg.compareVar /*setDefaultViewType*/);

    if (syncDirectionsChanged)
        applySyncDirections();

    if (filterConfigChanged)
    {
        updateGlobalFilterButton(); //refresh global filter icon
        applyFilterConfig(); //re-apply filter
    }

    updateUnsavedCfgStatus(); //also included by updateGui();
}


void MainDialog::onGlobalFilterContext(wxEvent& event)
{
    auto clearFilter = [&]
    {
        currentCfg_.mainCfg.globalFilter = FilterConfig();
        updateGlobalFilterButton(); //refresh global filter icon
        applyFilterConfig(); //re-apply filter
    };
    auto copyFilter  = [&] { filterCfgOnClipboard_ = std::make_unique<FilterConfig>(currentCfg_.mainCfg.globalFilter); };
    auto pasteFilter = [&]
    {
        if (filterCfgOnClipboard_)
        {
            currentCfg_.mainCfg.globalFilter = *filterCfgOnClipboard_;
            updateGlobalFilterButton(); //refresh global filter icon
            applyFilterConfig(); //re-apply filter
        }
    };

    ContextMenu menu;
    menu.addItem( _("Clear filter"), clearFilter, wxNullImage, !isNullFilter(currentCfg_.mainCfg.globalFilter));
    menu.addSeparator();
    menu.addItem( _("Copy"),  copyFilter,  wxNullImage, !isNullFilter(currentCfg_.mainCfg.globalFilter));
    menu.addItem( _("Paste"), pasteFilter, wxNullImage, filterCfgOnClipboard_.get() != nullptr);

    menu.popup(*m_bpButtonFilterContext, { m_bpButtonFilterContext->GetSize().x, 0 });
}


void MainDialog::onToggleViewType(wxCommandEvent& event)
{
    setGridViewType(m_bpButtonViewType->isActive() ? GridViewType::difference : GridViewType::action);
}


void MainDialog::onToggleViewButton(wxCommandEvent& event)
{
    if (auto button = dynamic_cast<ToggleButton*>(event.GetEventObject()))
    {
        button->toggle();
        updateGui();
    }
    else
        assert(false);
}


void MainDialog::setViewFilterDefault()
{
    auto setButton = [](ToggleButton& tb, bool value) { tb.setActive(value); };

    const auto& def = globalCfg_.mainDlg.viewFilterDefault;
    setButton(*m_bpButtonShowExcluded, def.excluded);
    setButton(*m_bpButtonShowEqual,    def.equal);
    setButton(*m_bpButtonShowConflict, def.conflict);

    setButton(*m_bpButtonShowLeftOnly,   def.leftOnly);
    setButton(*m_bpButtonShowRightOnly,  def.rightOnly);
    setButton(*m_bpButtonShowLeftNewer,  def.leftNewer);
    setButton(*m_bpButtonShowRightNewer, def.rightNewer);
    setButton(*m_bpButtonShowDifferent,  def.different);

    setButton(*m_bpButtonShowCreateLeft,  def.createLeft);
    setButton(*m_bpButtonShowCreateRight, def.createRight);
    setButton(*m_bpButtonShowUpdateLeft,  def.updateLeft);
    setButton(*m_bpButtonShowUpdateRight, def.updateRight);
    setButton(*m_bpButtonShowDeleteLeft,  def.deleteLeft);
    setButton(*m_bpButtonShowDeleteRight, def.deleteRight);
    setButton(*m_bpButtonShowDoNothing,   def.doNothing);
}


void MainDialog::onViewTypeContextMouse(wxMouseEvent&   event)
{
    ContextMenu menu;

    const GridViewType viewType = m_bpButtonViewType->isActive() ? GridViewType::action : GridViewType::difference;
    menu.addItem(_("Difference") + (viewType != GridViewType::difference ? L"\tF11" : L""), [&] { setGridViewType(GridViewType::difference); }, greyScaleIfDisabled(loadImage("compare_sicon"   ), viewType == GridViewType::difference));
    menu.addItem(_("Action")     + (viewType != GridViewType::action     ? L"\tF11" : L""), [&] { setGridViewType(GridViewType::action    ); }, greyScaleIfDisabled(loadImage("start_sync_sicon"), viewType == GridViewType::action));

    menu.popup(*m_bpButtonViewType, { m_bpButtonViewType->GetSize().x, 0 });
}


void MainDialog::onViewFilterContext(wxEvent& event)
{
    ContextMenu menu;

    auto saveButtonDefault = [](const ToggleButton& tb, bool& defaultValue)
    {
        if (tb.IsShown())
            defaultValue = tb.isActive();
    };

    auto saveDefault = [&]
    {
        auto& def = globalCfg_.mainDlg.viewFilterDefault;
        saveButtonDefault(*m_bpButtonShowExcluded, def.excluded);
        saveButtonDefault(*m_bpButtonShowEqual,    def.equal);
        saveButtonDefault(*m_bpButtonShowConflict, def.conflict);

        saveButtonDefault(*m_bpButtonShowLeftOnly,   def.leftOnly);
        saveButtonDefault(*m_bpButtonShowRightOnly,  def.rightOnly);
        saveButtonDefault(*m_bpButtonShowLeftNewer,  def.leftNewer);
        saveButtonDefault(*m_bpButtonShowRightNewer, def.rightNewer);
        saveButtonDefault(*m_bpButtonShowDifferent,  def.different);

        saveButtonDefault(*m_bpButtonShowCreateLeft,  def.createLeft);
        saveButtonDefault(*m_bpButtonShowCreateRight, def.createRight);
        saveButtonDefault(*m_bpButtonShowDeleteLeft,  def.deleteLeft);
        saveButtonDefault(*m_bpButtonShowDeleteRight, def.deleteRight);
        saveButtonDefault(*m_bpButtonShowUpdateLeft,  def.updateLeft);
        saveButtonDefault(*m_bpButtonShowUpdateRight, def.updateRight);
        saveButtonDefault(*m_bpButtonShowDoNothing,   def.doNothing);

        flashStatusInformation(_("View settings saved"));
    };

    menu.addItem( _("Save as default"), saveDefault, loadImage("cfg_save_sicon"));
    menu.popup(*m_bpButtonViewFilterContext, { m_bpButtonViewFilterContext->GetSize().x, 0 });
}


void MainDialog::updateGlobalFilterButton()
{
    //global filter: test for Null-filter
    setImage(*m_bpButtonFilter, greyScaleIfDisabled(loadImage("options_filter"), !isNullFilter(currentCfg_.mainCfg.globalFilter)));

    const std::wstring status = !isNullFilter(currentCfg_.mainCfg.globalFilter) ? _("Active") : _("None");
    m_bpButtonFilter->SetToolTip(_("Filter") + L" (F7) (" + status + L')');
    m_bpButtonFilterContext->SetToolTip(m_bpButtonFilter->GetToolTipText());
}


void MainDialog::onCompare(wxCommandEvent& event)
{
    //wxBusyCursor dummy; -> redundant: progress already shown in progress dialog!

    FocusPreserver fp; //e.g. keep focus on config panel after pressing F5

    int scrollPosX = 0;
    int scrollPosY = 0;
    m_gridMainL->GetViewStart(&scrollPosX, &scrollPosY); //preserve current scroll position
    ZEN_ON_SCOPE_EXIT(
        m_gridMainL->Scroll(scrollPosX, scrollPosY); //
        m_gridMainR->Scroll(scrollPosX, scrollPosY); //restore
        m_gridMainC->Scroll(-1, scrollPosY); );      //

    disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
    auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
    ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

    clearGrid(); //avoid memory peak by clearing old data first

    const auto& guiCfg = getConfig();
    const std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();

    const std::vector<FolderPairCfg>& fpCfgList = extractCompareCfg(guiCfg.mainCfg);

    //handle status display and error messages
    StatusHandlerTemporaryPanel statusHandler(*this, startTime,
                                              guiCfg.mainCfg.ignoreErrors,
                                              guiCfg.mainCfg.autoRetryCount,
                                              guiCfg.mainCfg.autoRetryDelay);
    try
    {
        //GUI mode: place directory locks on directories isolated(!) during both comparison and synchronization
        std::unique_ptr<LockHolder> dirLocks;

        //COMPARE DIRECTORIES
        folderCmp_ = compare(globalCfg_.warnDlgs,
                             globalCfg_.fileTimeTolerance,
                             true, //allowUserInteraction
                             globalCfg_.runWithBackgroundPriority,
                             globalCfg_.createLockFile,
                             dirLocks,
                             fpCfgList,
                             statusHandler); //throw AbortProcess
    }
    catch (AbortProcess&) {}

    const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
    //---------------------------------------------------------------------------

    setLastOperationLog(r.summary, r.errorLog);

    if (r.summary.syncResult == SyncResult::aborted)
        return updateGui(); //refresh grid in ANY case! (also on abort)


    filegrid::setData(*m_gridMainC,    folderCmp_); //update view on data
    treegrid::setData(*m_gridOverview, folderCmp_); //
    updateGui();

    m_gridMainL->clearSelection(GridEventPolicy::allow);
    m_gridMainC->clearSelection(GridEventPolicy::allow);
    m_gridMainR->clearSelection(GridEventPolicy::allow);

    m_gridOverview->clearSelection(GridEventPolicy::allow);

    //play (optional) sound notification
    if (!globalCfg_.soundFileCompareFinished.empty())
    {
        //wxWidgets shows modal error dialog by default => NO!
        wxLog* oldLogTarget = wxLog::SetActiveTarget(new wxLogStderr); //transfer and receive ownership!
        ZEN_ON_SCOPE_EXIT(delete wxLog::SetActiveTarget(oldLogTarget));

        wxSound::Play(utfTo<wxString>(globalCfg_.soundFileCompareFinished), wxSOUND_ASYNC);
    }

    if (!IsActive())
        RequestUserAttention();

    //remember folder history (except when cancelled by user)
    for (const FolderPairCfg& fpCfg : fpCfgList)
    {
        folderHistoryLeft_ ->addItem(fpCfg.folderPathPhraseLeft_);
        folderHistoryRight_->addItem(fpCfg.folderPathPhraseRight_);
    }

    assert(m_buttonCompare->GetId() != wxID_ANY);
    if (fp.getFocusId() == m_buttonCompare->GetId())
        fp.setFocus(m_buttonSync);

    //mark selected cfg files as "in sync" when there is nothing to do: https://freefilesync.org/forum/viewtopic.php?t=4991
    if (r.summary.syncResult == SyncResult::finishedSuccess)
    {
        const SyncStatistics st(folderCmp_);
        if (st.createCount() +
            st.updateCount() +
            st.deleteCount() == 0)
        {
            flashStatusInformation(_("No files to synchronize"));
            updateConfigLastRunStats(std::chrono::system_clock::to_time_t(startTime), r.summary.syncResult, getNullPath() /*logFilePath*/);
        }
    }
}


void MainDialog::updateGui()
{
    updateGridViewData(); //update gridDataView and write status information

    updateStatistics();

    updateUnsavedCfgStatus();

    const auto& mainCfg = getConfig().mainCfg;
    const std::optional<CompareVariant>           cmpVar  = getCompVariant(mainCfg);
    const std::optional<SyncVariant> syncVar = getSyncVariant(mainCfg);

    const char* cmpVarIconName = nullptr;
    if (cmpVar)
        switch (*cmpVar)
        {
            //*INDENT-OFF*
            case CompareVariant::timeSize: cmpVarIconName = "cmp_time";    break;
            case CompareVariant::content:  cmpVarIconName = "cmp_content"; break;
            case CompareVariant::size:     cmpVarIconName = "cmp_size";    break;
            //*INDENT-ON*
        }
    const char* syncVarIconName = nullptr;
    if (syncVar)
        switch (*syncVar)
        {
            //*INDENT-OFF*
            case SyncVariant::twoWay: syncVarIconName = "sync_twoway"; break;
            case SyncVariant::mirror: syncVarIconName = "sync_mirror"; break;
            case SyncVariant::update: syncVarIconName = "sync_update"; break;
            case SyncVariant::custom: syncVarIconName = "sync_custom"; break;
            //*INDENT-ON*
        }

    updateTopButton(*m_buttonCompare, loadImage("compare"),   getVariantName(cmpVar),  cmpVarIconName, false /*makeGrey*/);
    updateTopButton(*m_buttonSync,    loadImage("start_sync"), getVariantName(syncVar), syncVarIconName, folderCmp_.empty());
    m_panelTopButtons->Layout();

    m_menuItemExportList->Enable(!folderCmp_.empty()); //a CSV without even folder names confuses users: https://freefilesync.org/forum/viewtopic.php?t=4787

    //auiMgr_.Update(); -> doesn't seem to be needed
}


void MainDialog::clearGrid(ptrdiff_t pos)
{
    if (!folderCmp_.empty())
    {
        assert(pos < makeSigned(folderCmp_.size()));
        if (pos < 0)
            folderCmp_.clear();
        else
            folderCmp_.erase(folderCmp_.begin() + pos);
    }

    filegrid::setData(*m_gridMainC,    folderCmp_);
    treegrid::setData(*m_gridOverview, folderCmp_);
    updateGui();
}


void MainDialog::updateStatistics()
{
    auto setValue = [](wxStaticText& txtControl, bool isZeroValue, const wxString& valueAsString, wxStaticBitmap& bmpControl, const char* imageName)
    {
        if (txtControl.GetLabel() != valueAsString)
        {
            wxFont fnt = txtControl.GetFont();
            fnt.SetWeight(isZeroValue ? wxFONTWEIGHT_NORMAL : wxFONTWEIGHT_BOLD);
            txtControl.SetFont(fnt);

            txtControl.SetLabel(valueAsString);
            bmpControl.SetBitmap(greyScaleIfDisabled(mirrorIfRtl(loadImage(imageName)), !isZeroValue));
        }
    };

    auto setIntValue = [&setValue](wxStaticText& txtControl, int value, wxStaticBitmap& bmpControl, const char* imageName)
    {
        setValue(txtControl, value == 0, formatNumber(value), bmpControl, imageName);
    };

    //update preview of item count and bytes to be transferred:
    const SyncStatistics st(folderCmp_);

    setValue(*m_staticTextData, st.getBytesToProcess() == 0, formatFilesizeShort(st.getBytesToProcess()), *m_bitmapData, "data");
    setIntValue(*m_staticTextCreateLeft,  st.createCount< LEFT_SIDE>(), *m_bitmapCreateLeft,  "so_create_left_sicon");
    setIntValue(*m_staticTextUpdateLeft,  st.updateCount< LEFT_SIDE>(), *m_bitmapUpdateLeft,  "so_update_left_sicon");
    setIntValue(*m_staticTextDeleteLeft,  st.deleteCount< LEFT_SIDE>(), *m_bitmapDeleteLeft,  "so_delete_left_sicon");
    setIntValue(*m_staticTextCreateRight, st.createCount<RIGHT_SIDE>(), *m_bitmapCreateRight, "so_create_right_sicon");
    setIntValue(*m_staticTextUpdateRight, st.updateCount<RIGHT_SIDE>(), *m_bitmapUpdateRight, "so_update_right_sicon");
    setIntValue(*m_staticTextDeleteRight, st.deleteCount<RIGHT_SIDE>(), *m_bitmapDeleteRight, "so_delete_right_sicon");

    m_panelStatistics->Layout();
    m_panelStatistics->Refresh(); //fix small mess up on RTL layout
}


void MainDialog::applyCompareConfig(bool setDefaultViewType)
{
    clearGrid(); //+ GUI update

    //convenience: change sync view
    if (setDefaultViewType)
        switch (currentCfg_.mainCfg.cmpCfg.compareVar)
        {
            case CompareVariant::timeSize:
            case CompareVariant::size:
                setGridViewType(GridViewType::action);
                break;

            case CompareVariant::content:
                setGridViewType(GridViewType::difference);
                break;
        }
}


void MainDialog::onStartSync(wxCommandEvent& event)
{
    if (folderCmp_.empty())
    {
        //quick sync: simulate button click on "compare"
        wxCommandEvent dummy(wxEVT_COMMAND_BUTTON_CLICKED);
        m_buttonCompare->Command(dummy); //simulate click

        if (folderCmp_.empty()) //check if user aborted or error occurred, etc...
            return;
    }

    const auto& guiCfg = getConfig();

    //show sync preview/confirmation dialog
    if (globalCfg_.confirmDlgs.confirmSyncStart)
    {
        bool dontShowAgain = false;

        if (showSyncConfirmationDlg(this, false /*syncSelection*/,
                                    getSyncVariant(guiCfg.mainCfg),
                                    SyncStatistics(folderCmp_),
                                    dontShowAgain) != ConfirmationButton::accept)
            return;
        globalCfg_.confirmDlgs.confirmSyncStart = !dontShowAgain;
    }

    std::set<AbstractPath> logFilePathsToKeep;
    for (const ConfigFileItem& item : cfggrid::getDataView(*m_gridCfgHistory).get())
        logFilePathsToKeep.insert(item.logFilePath);

    const std::chrono::system_clock::time_point syncStartTime = std::chrono::system_clock::now();

    std::vector<std::wstring> jobNames;
    for (const Zstring& cfgFilePath : activeConfigFiles_)
        jobNames.push_back(equalNativePath(cfgFilePath, lastRunConfigPath_) ?
                           L'[' + _("Last session") + L']' :
                           extractJobName(cfgFilePath));

    using FinalRequest = StatusHandlerFloatingDialog::FinalRequest;
    FinalRequest finalRequest = FinalRequest::none;
    {
        disableGuiElements(false /*enableAbort*/); //StatusHandlerFloatingDialog will internally process Window messages, so avoid unexpected callbacks!
        ZEN_ON_SCOPE_EXIT(enableGuiElements());
        //run this->enableGuiElements() BEFORE "finalRequest" buf AFTER StatusHandlerFloatingDialog::reportResults()

        //class handling status updates and error messages
        StatusHandlerFloatingDialog statusHandler(this, jobNames, syncStartTime,
                                                  guiCfg.mainCfg.ignoreErrors,
                                                  guiCfg.mainCfg.autoRetryCount,
                                                  guiCfg.mainCfg.autoRetryDelay,
                                                  globalCfg_.soundFileSyncFinished,
                                                  globalCfg_.progressDlg.dlgSize, globalCfg_.progressDlg.isMaximized,
                                                  globalCfg_.progressDlg.autoClose);
        try
        {
            //PERF_START;

            //let's report here rather than before comparison (user might have changed global settings in the meantime!)
            logNonDefaultSettings(globalCfg_, statusHandler); //throw AbortProcess

            //wxBusyCursor dummy; -> redundant: progress already shown in progress dialog!

            //GUI mode: end directory lock lifetime after comparion and start new locking right before sync
            std::unique_ptr<LockHolder> dirLocks;
            if (globalCfg_.createLockFile)
            {
                std::set<Zstring> folderPathsToLock;
                for (auto it = begin(folderCmp_); it != end(folderCmp_); ++it)
                {
                    if (it->isAvailable<LEFT_SIDE>()) //do NOT check directory existence again!
                        if (std::optional<Zstring> nativeFolderPath = AFS::getNativeItemPath(it->getAbstractPath<LEFT_SIDE>())) //restrict directory locking to native paths until further
                            folderPathsToLock.insert(*nativeFolderPath);

                    if (it->isAvailable<RIGHT_SIDE>())
                        if (std::optional<Zstring> nativeFolderPath = AFS::getNativeItemPath(it->getAbstractPath<RIGHT_SIDE>()))
                            folderPathsToLock.insert(*nativeFolderPath);
                }
                dirLocks = std::make_unique<LockHolder>(folderPathsToLock, globalCfg_.warnDlgs.warnDirectoryLockFailed, statusHandler); //throw AbortProcess
            }

            //START SYNCHRONIZATION
            synchronize(syncStartTime,
                        globalCfg_.verifyFileCopy,
                        globalCfg_.copyLockedFiles,
                        globalCfg_.copyFilePermissions,
                        globalCfg_.failSafeFileCopy,
                        globalCfg_.runWithBackgroundPriority,
                        extractSyncCfg(guiCfg.mainCfg),
                        folderCmp_,
                        globalCfg_.warnDlgs,
                        statusHandler); //throw AbortProcess
        }
        catch (AbortProcess&) {}

        StatusHandlerFloatingDialog::Result r = statusHandler.reportResults(guiCfg.mainCfg.postSyncCommand, guiCfg.mainCfg.postSyncCondition,
                                                                            guiCfg.mainCfg.altLogFolderPathPhrase, globalCfg_.logfilesMaxAgeDays, globalCfg_.logFormat, logFilePathsToKeep,
                                                                            guiCfg.mainCfg.emailNotifyAddress, guiCfg.mainCfg.emailNotifyCondition); //noexcept
        //---------------------------------------------------------------------------
        setLastOperationLog(r.summary, r.errorLog.ptr());

        globalCfg_.progressDlg.autoClose   = r.autoCloseDialog;
        globalCfg_.progressDlg.dlgSize     = r.dlgSize;
        globalCfg_.progressDlg.isMaximized = r.dlgIsMaximized;

        //update last sync stats for the selected cfg files
        updateConfigLastRunStats(std::chrono::system_clock::to_time_t(syncStartTime), r.summary.syncResult, r.logFilePath);

        //remove empty rows: just a beautification, invalid rows shouldn't cause issues
        filegrid::getDataView(*m_gridMainC).removeInvalidRows();

        updateGui();

        finalRequest = r.finalRequest;
    }

    //---------------------------------------------------------------------------
    switch (finalRequest)
    {
        case FinalRequest::none:
            break;
        case FinalRequest::exit:
            Destroy(); //don't use Close(): we don't want to show the prompt to save current config in onClose()
            break;
        case FinalRequest::shutdown: //run *after* last sync stats were updated and saved! https://freefilesync.org/forum/viewtopic.php?t=5761
            try
            {
                onQueryEndSession(); //(try to) save GlobalSettings.xml => don't block shutdown if failed!!!
                shutdownSystem(); //throw FileError
                terminateProcess(FFS_EXIT_SUCCESS); //no point in continuing and saving cfg again in ~MainDialog()/onQueryEndSession() while the OS will kill us anytime!
            }
            catch (const FileError& e) { showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString())); }
            //[!] ignores current error handling setting, BUT this is not a sync error!
            break;
    }
}


namespace
{
void appendInactive(ContainerObject& hierObj, std::vector<FileSystemObject*>& inactiveItems)
{
    for (FilePair& file : hierObj.refSubFiles())
        if (!file.isActive())
            inactiveItems.push_back(&file);
    for (SymlinkPair& link : hierObj.refSubLinks())
        if (!link.isActive())
            inactiveItems.push_back(&link);
    for (FolderPair& folder : hierObj.refSubFolders())
    {
        if (!folder.isActive())
            inactiveItems.push_back(&folder);
        appendInactive(folder, inactiveItems); //recurse
    }
}
}


void MainDialog::startSyncForSelecction(const std::vector<FileSystemObject*>& selection)
{
    //------------------ analyze selection ------------------
    std::unordered_set<const BaseFolderPair*> basePairsSelect;
    std::vector<FileSystemObject*> selectedActive;

    for (FileSystemObject* fsObj : expandSelectionForPartialSync(selection))
    {
        switch (fsObj->getSyncOperation())
        {
            case SO_CREATE_NEW_LEFT:
            case SO_CREATE_NEW_RIGHT:
            case SO_DELETE_LEFT:
            case SO_DELETE_RIGHT:
            case SO_MOVE_LEFT_FROM:
            case SO_MOVE_LEFT_TO:
            case SO_MOVE_RIGHT_FROM:
            case SO_MOVE_RIGHT_TO:
            case SO_OVERWRITE_LEFT:
            case SO_OVERWRITE_RIGHT:
            case SO_COPY_METADATA_TO_LEFT:
            case SO_COPY_METADATA_TO_RIGHT:
                basePairsSelect.insert(&fsObj->base());
                break;

            case SO_UNRESOLVED_CONFLICT:
            case SO_DO_NOTHING:
            case SO_EQUAL:
                break;
        }
        if (fsObj->isActive())
            selectedActive.push_back(fsObj);
    }

    if (basePairsSelect.empty())
        return; //harmonize with onGridContextRim(): this function should be a no-op iff context menu option is disabled!

    FocusPreserver fp;
    {
        //---------------------------------------------------------------
        //simulate partial sync by temporarily excluding all other items:
        std::vector<FileSystemObject*> inactiveItems; //remember inactive (assuming a smaller number than active items)
        std::for_each(begin(folderCmp_), end(folderCmp_), [&](BaseFolderPair& baseFolder) { appendInactive(baseFolder, inactiveItems); });

        setActiveStatus(false, folderCmp_); //limit to folderCmpSelect? => no, let's also activate non-participating folder pairs, if only to visually match user selection

        for (FileSystemObject* fsObj : selectedActive)
            fsObj->setActive(true);

        //don't run a full updateGui() (which would remove excluded rows) since we're only temporarily excluding:
        filegrid::refresh(*m_gridMainL, *m_gridMainC, *m_gridMainR);
        m_gridOverview->Refresh();

        ZEN_ON_SCOPE_EXIT(
            setActiveStatus(true, folderCmp_);

            //inactive items are expected to still exist after sync! => no need for FileSystemObject::ObjectId
            for (FileSystemObject* fsObj : inactiveItems)
            fsObj->setActive(false);

            filegrid::refresh(*m_gridMainL, *m_gridMainC, *m_gridMainR); //e.g. if user cancels confirmation popup
            m_gridOverview->Refresh();
        );
        //---------------------------------------------------------------
        const auto& guiCfg = getConfig();
        const std::vector<FolderPairSyncCfg> fpCfg = extractSyncCfg(guiCfg.mainCfg);

        //only apply partial sync to base pairs that contain at least one item to sync (e.g. avoid needless sync.ffs_db updates)
        std::vector<std::shared_ptr<BaseFolderPair>> folderCmpSelect;
        std::vector<FolderPairSyncCfg>               fpCfgSelect;

        for (size_t i = 0; i < folderCmp_.size(); ++i)
            if (basePairsSelect.contains(folderCmp_[i].get()))
            {
                folderCmpSelect.push_back(folderCmp_[i]);
                fpCfgSelect    .push_back(     fpCfg[i]);
            }

        //show sync preview/confirmation dialog
        if (globalCfg_.confirmDlgs.confirmSyncStart)
        {
            bool dontShowAgain = false;

            if (showSyncConfirmationDlg(this,
                                        true /*syncSelection*/,
                                        getSyncVariant(guiCfg.mainCfg),
                                        SyncStatistics(folderCmpSelect),
                                        dontShowAgain) != ConfirmationButton::accept)
                return;
            globalCfg_.confirmDlgs.confirmSyncStart = !dontShowAgain;
        }

        const std::chrono::system_clock::time_point syncStartTime = std::chrono::system_clock::now();

        //last sync log file? => let's go without; same behavior as manual deletion

        disableGuiElements(true /*enableAbort*/); //StatusHandlerFloatingDialog will internally process Window messages, so avoid unexpected callbacks!
        auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
        ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

        StatusHandlerTemporaryPanel statusHandler(*this, syncStartTime,
                                                  guiCfg.mainCfg.ignoreErrors,
                                                  guiCfg.mainCfg.autoRetryCount,
                                                  guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
        try
        {
            //let's report here rather than before comparison (user might have changed global settings in the meantime!)
            logNonDefaultSettings(globalCfg_, statusHandler); //throw AbortProcess

            //LockHolder? => let's go without; same behavior as manual deletion

            //START SYNCHRONIZATION
            synchronize(syncStartTime,
                        globalCfg_.verifyFileCopy,
                        globalCfg_.copyLockedFiles,
                        globalCfg_.copyFilePermissions,
                        globalCfg_.failSafeFileCopy,
                        globalCfg_.runWithBackgroundPriority,
                        fpCfgSelect,
                        folderCmpSelect,
                        globalCfg_.warnDlgs,
                        statusHandler); //throw AbortProcess
        }
        catch (AbortProcess&) {}

        const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept

        setLastOperationLog(r.summary, r.errorLog);
    } //run updateGui() *after* reverting our temporary exclusions

    //remove empty rows: just a beautification, invalid rows shouldn't cause issues
    filegrid::getDataView(*m_gridMainC).removeInvalidRows();

    updateGui();
}


void MainDialog::updateConfigLastRunStats(time_t lastRunTime, SyncResult result, const AbstractPath& logFilePath)
{
    cfggrid::getDataView(*m_gridCfgHistory).setLastRunStats(activeConfigFiles_, { lastRunTime, result, logFilePath });

    //re-apply selection: sort order changed if sorted by last sync time
    cfggrid::addAndSelect(*m_gridCfgHistory, activeConfigFiles_, false /*scrollToSelection*/);
    //m_gridCfgHistory->Refresh(); <- implicit in last call
}


void MainDialog::setLastOperationLog(const ProcessSummary& summary, const std::shared_ptr<const ErrorLog>& errorLog)
{
    const wxImage syncResultImage = [&]
    {
        switch (summary.syncResult)
        {
            case SyncResult::finishedSuccess:
                return loadImage("result_success");
            case SyncResult::finishedWarning:
                return loadImage("result_warning");
            case SyncResult::finishedError:
            case SyncResult::aborted:
                return loadImage("result_error");
        }
        assert(false);
        return wxNullImage;
    }();

    const wxImage logOverlayImage = [&]
    {
        //don't use "syncResult": There may be errors after sync, e.g. failure to save log file/send email!
        if (errorLog)
        {
            const ErrorLog::Stats logCount = errorLog->getStats();
            if (logCount.error > 0)
                return loadImage("msg_error", getDefaultMenuIconSize());
            if (logCount.warning > 0)
                return loadImage("msg_warning", getDefaultMenuIconSize());
        }
        return wxNullImage;
    }();

    m_bitmapSyncResult->SetBitmap(syncResultImage);
    m_staticTextSyncResult->SetLabel(getSyncResultLabel(summary.syncResult));


    m_staticTextItemsProcessed->SetLabel(formatNumber(summary.statsProcessed.items));
    m_staticTextBytesProcessed->SetLabel(L'(' + formatFilesizeShort(summary.statsProcessed.bytes) + L')');

    const bool hideRemainingStats = (summary.statsTotal.items < 0 && summary.statsTotal.bytes < 0) || //no total items/bytes: e.g. for pure folder comparison
                                    summary.statsProcessed == summary.statsTotal;  //...if everything was processed successfully

    m_staticTextProcessed     ->Show(!hideRemainingStats);
    m_staticTextRemaining     ->Show(!hideRemainingStats);
    m_staticTextItemsRemaining->Show(!hideRemainingStats);
    m_staticTextBytesRemaining->Show(!hideRemainingStats);

    if (!hideRemainingStats)
    {
        m_staticTextItemsRemaining->SetLabel(              formatNumber(summary.statsTotal.items - summary.statsProcessed.items));
        m_staticTextBytesRemaining->SetLabel(L'(' + formatFilesizeShort(summary.statsTotal.bytes - summary.statsProcessed.bytes) + L')');
    }

    const int64_t totalTimeSec = std::chrono::duration_cast<std::chrono::seconds>(summary.totalTime).count();

    m_staticTextTimeElapsed->SetLabel(wxTimeSpan::Seconds(totalTimeSec).Format(L"%H:%M:%S"));
    //totalTimeSec < 3600 ? wxTimeSpan::Seconds(totalTimeSec).Format(L"%M:%S") -> let's use full precision for max. clarity: https://freefilesync.org/forum/viewtopic.php?t=6308

    logPanel_->setLog(errorLog);

    m_panelLog->Layout();
    //m_panelItemStats->Layout(); //needed?
    //m_panelTimeStats->Layout(); //

    setImage(*m_bpButtonToggleLog, layOver(loadImage("log_file"), logOverlayImage, wxALIGN_BOTTOM | wxALIGN_RIGHT));
    m_bpButtonToggleLog->Show(static_cast<bool>(errorLog));
}


void MainDialog::onToggleLog(wxCommandEvent& event)
{
    const bool show = !auiMgr_.GetPane(m_panelLog).IsShown();
    showLogPanel(show);
    if (show)
        logPanel_->SetFocus();
}


void MainDialog::showLogPanel(bool show)
{
    wxAuiPaneInfo& logPane = auiMgr_.GetPane(m_panelLog);
    if (show != logPane.IsShown())
    {
        if (!show)
        {
            if (logPane.IsMaximized())
                auiMgr_.RestorePane(logPane); //!= wxAuiPaneInfo::Restore() which does not un-hide other panels (WTF!?)
            else //ensure current window sizes will be used when pane is shown again:
                logPane.best_size = logPane.rect.GetSize();
        }

        logPane.Show(show);
        auiMgr_.Update();
        m_panelLog->Refresh(); //macOS: fix background corruption for the statistics boxes (call *after* wxAuiManager::Update()
    }
}


void MainDialog::onGridDoubleClickRim(GridClickEvent& event, bool leftSide)
{
    if (!globalCfg_.externalApps.empty())
    {
        std::vector<FileSystemObject*> selectionLeft;
        std::vector<FileSystemObject*> selectionRight;
        if (FileSystemObject* fsObj = filegrid::getDataView(*m_gridMainC).getFsObject(event.row_)) //selection must be a list of BOUND pointers!
            (leftSide ? selectionLeft : selectionRight) = { fsObj };

        openExternalApplication(globalCfg_.externalApps[0].cmdLine, leftSide, selectionLeft, selectionRight);
    }
}


void MainDialog::onGridLabelLeftClickRim(GridLabelClickEvent& event, bool leftSide)
{
    const ColumnTypeRim colType = static_cast<ColumnTypeRim>(event.colType_);

    bool sortAscending = getDefaultSortDirection(colType);

    if (auto sortInfo = filegrid::getDataView(*m_gridMainC).getSortConfig())
        if (const ColumnTypeRim* sortType = std::get_if<ColumnTypeRim>(&sortInfo->sortCol))
            if (*sortType == colType && sortInfo->onLeft == leftSide)
                sortAscending = !sortInfo->ascending;

    const ItemPathFormat itemPathFormat = leftSide ? globalCfg_.mainDlg.itemPathFormatLeftGrid : globalCfg_.mainDlg.itemPathFormatRightGrid;

    filegrid::getDataView(*m_gridMainC).sortView(colType, itemPathFormat, leftSide, sortAscending);

    m_gridMainL->clearSelection(GridEventPolicy::allow);
    m_gridMainC->clearSelection(GridEventPolicy::allow);
    m_gridMainR->clearSelection(GridEventPolicy::allow);

    updateGui(); //refresh gridDataView
}


void MainDialog::onGridLabelLeftClickC(GridLabelClickEvent& event)
{
    const ColumnTypeCenter colType = static_cast<ColumnTypeCenter>(event.colType_);
    if (colType != ColumnTypeCenter::checkbox)
    {
        bool sortAscending = getDefaultSortDirection(colType);

        if (auto sortInfo = filegrid::getDataView(*m_gridMainC).getSortConfig())
            if (const ColumnTypeCenter* sortType = std::get_if<ColumnTypeCenter>(&sortInfo->sortCol))
                if (*sortType == colType)
                    sortAscending = !sortInfo->ascending;

        filegrid::getDataView(*m_gridMainC).sortView(colType, sortAscending);

        m_gridMainL->clearSelection(GridEventPolicy::allow);
        m_gridMainC->clearSelection(GridEventPolicy::allow);
        m_gridMainR->clearSelection(GridEventPolicy::allow);

        updateGui(); //refresh gridDataView
    }
}

void MainDialog::onSwapSides(wxCommandEvent& event)
{
    if (globalCfg_.confirmDlgs.confirmSwapSides)
    {
        bool dontWarnAgain = false;
        switch (showConfirmationDialog(this, DialogInfoType::info,
                                       PopupDialogCfg().setMainInstructions(_("Please confirm you want to swap sides.")).
                                       setCheckBox(dontWarnAgain, _("&Don't show this dialog again")),
                                       _("&Swap")))
        {
            case ConfirmationButton::accept: //swap
                globalCfg_.confirmDlgs.confirmSwapSides = !dontWarnAgain;
                break;
            case ConfirmationButton::cancel:
                return;
        }
    }
    //------------------------------------------------------

    //swap directory names:
    LocalPairConfig lpc1st = firstFolderPair_->getValues();
    std::swap(lpc1st.folderPathPhraseLeft, lpc1st.folderPathPhraseRight);
    firstFolderPair_->setValues(lpc1st);

    for (FolderPairPanel* panel : additionalFolderPairs_)
    {
        LocalPairConfig lpc = panel->getValues();
        std::swap(lpc.folderPathPhraseLeft, lpc.folderPathPhraseRight);
        panel->setValues(lpc);
    }

    //swap view filter
    bool tmp = m_bpButtonShowLeftOnly->isActive();
    m_bpButtonShowLeftOnly->setActive(m_bpButtonShowRightOnly->isActive());
    m_bpButtonShowRightOnly->setActive(tmp);

    tmp = m_bpButtonShowLeftNewer->isActive();
    m_bpButtonShowLeftNewer->setActive(m_bpButtonShowRightNewer->isActive());
    m_bpButtonShowRightNewer->setActive(tmp);

    /* for sync preview and "mirror" variant swapping may create strange effect:
    tmp = m_bpButtonShowCreateLeft->isActive();
    m_bpButtonShowCreateLeft->setActive(m_bpButtonShowCreateRight->isActive());
    m_bpButtonShowCreateRight->setActive(tmp);

    tmp = m_bpButtonShowDeleteLeft->isActive();
    m_bpButtonShowDeleteLeft->setActive(m_bpButtonShowDeleteRight->isActive());
    m_bpButtonShowDeleteRight->setActive(tmp);

    tmp = m_bpButtonShowUpdateLeft->isActive();
    m_bpButtonShowUpdateLeft->setActive(m_bpButtonShowUpdateRight->isActive());
    m_bpButtonShowUpdateRight->setActive(tmp);
    */
    //----------------------------------------------------------------------

    if (!folderCmp_.empty())
    {
        FocusPreserver fp;

        disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
        auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
        ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

        const auto& guiCfg = getConfig();

        StatusHandlerTemporaryPanel statusHandler(*this, std::chrono::system_clock::now() /*startTime*/,
                                                  false /*ignoreErrors*/,
                                                  guiCfg.mainCfg.autoRetryCount,
                                                  guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
        try
        {
            statusHandler.initNewPhase(-1, -1, ProcessPhase::none);
            swapGrids(getConfig().mainCfg, folderCmp_,
                      statusHandler); //throw AbortProcess
        }
        catch (AbortProcess&) {}

        const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
        setLastOperationLog(r.summary, r.errorLog);
    }

    updateGui(); //e.g. unsaved changes
}


void MainDialog::updateGridViewData()
{
    auto updateFilterButton = [&](ToggleButton& btn, const char* imgName, int itemCount)
    {
        const bool show = itemCount > 0;
        if (show)
        {
            int& itemCountDrawn = buttonLabelItemCount_[&btn];
            assert(itemCount != 0); //itemCountDrawn defaults to 0!
            if (itemCountDrawn != itemCount) //perf: only regenerate button labels when needed!
            {
                itemCountDrawn = itemCount;

                //accessibility: always set both foreground AND background colors!
                wxImage imgCountPressed  = mirrorIfRtl(createImageFromText(formatNumber(itemCount), btn.GetFont().Bold(), wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT)));
                wxImage imgCountReleased = mirrorIfRtl(createImageFromText(formatNumber(itemCount), btn.GetFont(),        wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
                imgCountReleased = resizeCanvas(imgCountReleased, imgCountPressed.GetSize(), wxALIGN_CENTER); //match with imgCountPressed's bold font

                //add bottom/right border space
                imgCountPressed  = resizeCanvas(imgCountPressed,  imgCountPressed .GetSize() + wxSize(fastFromDIP(5), fastFromDIP(5)), wxALIGN_TOP | wxALIGN_LEFT);
                imgCountReleased = resizeCanvas(imgCountReleased, imgCountReleased.GetSize() + wxSize(fastFromDIP(5), fastFromDIP(5)), wxALIGN_TOP | wxALIGN_LEFT);

                wxImage imgCategory = loadImage(imgName);
                imgCategory = resizeCanvas(imgCategory, imgCategory.GetSize() + wxSize(fastFromDIP(5), fastFromDIP(2)), wxALIGN_CENTER);

                wxImage imgIconReleased = imgCategory.ConvertToGreyscale(1.0/3, 1.0/3, 1.0/3); //treat all channels equally!
                brighten(imgIconReleased, 80);

                wxImage imgButtonPressed  = stackImages(imgCategory,     imgCountPressed,  ImageStackLayout::horizontal, ImageStackAlignment::bottom);
                wxImage imgButtonReleased = stackImages(imgIconReleased, imgCountReleased, ImageStackLayout::horizontal, ImageStackAlignment::bottom);

                //draw rectangle border with gradient as background
                wxImage imgBorderRect = renderPressedButton(imgButtonPressed.GetSize()).ConvertToImage();
                convertToVanillaImage(imgBorderRect);
                imgButtonPressed = layOver(imgBorderRect, imgButtonPressed);

                btn.init(mirrorIfRtl(imgButtonPressed), mirrorIfRtl(imgButtonReleased));
            }
        }

        if (btn.IsShown() != show)
            btn.Show(show);
    };

    FileView::FileStats fileStatsLeft;
    FileView::FileStats fileStatsRight;

    if (m_bpButtonViewType->isActive())
    {
        const FileView::ActionViewStats viewStats = filegrid::getDataView(*m_gridMainC).applyActionFilter(m_bpButtonShowExcluded->isActive(),
                                                    m_bpButtonShowCreateLeft ->isActive(),
                                                    m_bpButtonShowCreateRight->isActive(),
                                                    m_bpButtonShowDeleteLeft ->isActive(),
                                                    m_bpButtonShowDeleteRight->isActive(),
                                                    m_bpButtonShowUpdateLeft ->isActive(),
                                                    m_bpButtonShowUpdateRight->isActive(),
                                                    m_bpButtonShowDoNothing  ->isActive(),
                                                    m_bpButtonShowEqual      ->isActive(),
                                                    m_bpButtonShowConflict   ->isActive());
        fileStatsLeft  = viewStats.fileStatsLeft;
        fileStatsRight = viewStats.fileStatsRight;

        //sync preview buttons
        updateFilterButton(*m_bpButtonShowExcluded, "cat_excluded", viewStats.excluded);
        updateFilterButton(*m_bpButtonShowEqual,    "cat_equal",    viewStats.equal);
        updateFilterButton(*m_bpButtonShowConflict, "cat_conflict", viewStats.conflict);

        updateFilterButton(*m_bpButtonShowCreateLeft,  "so_create_left",  viewStats.createLeft);
        updateFilterButton(*m_bpButtonShowCreateRight, "so_create_right", viewStats.createRight);
        updateFilterButton(*m_bpButtonShowDeleteLeft,  "so_delete_left",  viewStats.deleteLeft);
        updateFilterButton(*m_bpButtonShowDeleteRight, "so_delete_right", viewStats.deleteRight);
        updateFilterButton(*m_bpButtonShowUpdateLeft,  "so_update_left",  viewStats.updateLeft);
        updateFilterButton(*m_bpButtonShowUpdateRight, "so_update_right", viewStats.updateRight);
        updateFilterButton(*m_bpButtonShowDoNothing,   "so_none",         viewStats.updateNone);

        m_bpButtonShowLeftOnly  ->Hide();
        m_bpButtonShowRightOnly ->Hide();
        m_bpButtonShowLeftNewer ->Hide();
        m_bpButtonShowRightNewer->Hide();
        m_bpButtonShowDifferent ->Hide();
    }
    else
    {
        const FileView::DifferenceViewStats viewStats = filegrid::getDataView(*m_gridMainC).applyDifferenceFilter(m_bpButtonShowExcluded->isActive(),
                                                        m_bpButtonShowLeftOnly  ->isActive(),
                                                        m_bpButtonShowRightOnly ->isActive(),
                                                        m_bpButtonShowLeftNewer ->isActive(),
                                                        m_bpButtonShowRightNewer->isActive(),
                                                        m_bpButtonShowDifferent ->isActive(),
                                                        m_bpButtonShowEqual     ->isActive(),
                                                        m_bpButtonShowConflict  ->isActive());
        fileStatsLeft  = viewStats.fileStatsLeft;
        fileStatsRight = viewStats.fileStatsRight;

        //comparison result view buttons
        updateFilterButton(*m_bpButtonShowExcluded, "cat_excluded", viewStats.excluded);
        updateFilterButton(*m_bpButtonShowEqual,    "cat_equal",    viewStats.equal);
        updateFilterButton(*m_bpButtonShowConflict, "cat_conflict", viewStats.conflict);

        m_bpButtonShowCreateLeft ->Hide();
        m_bpButtonShowCreateRight->Hide();
        m_bpButtonShowDeleteLeft ->Hide();
        m_bpButtonShowDeleteRight->Hide();
        m_bpButtonShowUpdateLeft ->Hide();
        m_bpButtonShowUpdateRight->Hide();
        m_bpButtonShowDoNothing  ->Hide();

        updateFilterButton(*m_bpButtonShowLeftOnly,   "cat_left_only",   viewStats.leftOnly);
        updateFilterButton(*m_bpButtonShowRightOnly,  "cat_right_only",  viewStats.rightOnly);
        updateFilterButton(*m_bpButtonShowLeftNewer,  "cat_left_newer",  viewStats.leftNewer);
        updateFilterButton(*m_bpButtonShowRightNewer, "cat_right_newer", viewStats.rightNewer);
        updateFilterButton(*m_bpButtonShowDifferent,  "cat_different",   viewStats.different);
    }

    const bool anyViewButtonShown = m_bpButtonShowExcluded   ->IsShown() ||
                                    m_bpButtonShowEqual      ->IsShown() ||
                                    m_bpButtonShowConflict   ->IsShown() ||

                                    m_bpButtonShowCreateLeft ->IsShown() ||
                                    m_bpButtonShowCreateRight->IsShown() ||
                                    m_bpButtonShowDeleteLeft ->IsShown() ||
                                    m_bpButtonShowDeleteRight->IsShown() ||
                                    m_bpButtonShowUpdateLeft ->IsShown() ||
                                    m_bpButtonShowUpdateRight->IsShown() ||
                                    m_bpButtonShowDoNothing  ->IsShown() ||

                                    m_bpButtonShowLeftOnly  ->IsShown() ||
                                    m_bpButtonShowRightOnly ->IsShown() ||
                                    m_bpButtonShowLeftNewer ->IsShown() ||
                                    m_bpButtonShowRightNewer->IsShown() ||
                                    m_bpButtonShowDifferent ->IsShown();

    m_bpButtonViewType         ->Show(anyViewButtonShown);
    m_bpButtonViewFilterContext->Show(anyViewButtonShown);

    m_panelViewFilter->Layout();

    //all three grids retrieve their data directly via gridDataView
    filegrid::refresh(*m_gridMainL, *m_gridMainC, *m_gridMainR);

    //overview panel
    if (m_bpButtonViewType->isActive())
        treegrid::getDataView(*m_gridOverview).applyActionFilter(m_bpButtonShowExcluded   ->isActive(),
                                                                 m_bpButtonShowCreateLeft ->isActive(),
                                                                 m_bpButtonShowCreateRight->isActive(),
                                                                 m_bpButtonShowDeleteLeft ->isActive(),
                                                                 m_bpButtonShowDeleteRight->isActive(),
                                                                 m_bpButtonShowUpdateLeft ->isActive(),
                                                                 m_bpButtonShowUpdateRight->isActive(),
                                                                 m_bpButtonShowDoNothing  ->isActive(),
                                                                 m_bpButtonShowEqual      ->isActive(),
                                                                 m_bpButtonShowConflict   ->isActive());
    else
        treegrid::getDataView(*m_gridOverview).applyDifferenceFilter(m_bpButtonShowExcluded  ->isActive(),
                                                                     m_bpButtonShowLeftOnly  ->isActive(),
                                                                     m_bpButtonShowRightOnly ->isActive(),
                                                                     m_bpButtonShowLeftNewer ->isActive(),
                                                                     m_bpButtonShowRightNewer->isActive(),
                                                                     m_bpButtonShowDifferent ->isActive(),
                                                                     m_bpButtonShowEqual     ->isActive(),
                                                                     m_bpButtonShowConflict  ->isActive());
    m_gridOverview->Refresh();

    //update status bar information
    setStatusBarFileStats(fileStatsLeft, fileStatsRight);
}


void MainDialog::setStatusBarFileStats(FileView::FileStats statsLeft,
                                       FileView::FileStats statsRight)
{
    //select state
    bSizerFileStatus->Show(true);
    m_staticTextFullStatus->Hide();

    //update status information
    bSizerStatusLeftDirectories->Show(statsLeft.folderCount > 0);
    bSizerStatusLeftFiles      ->Show(statsLeft.fileCount   > 0);

    setText(*m_staticTextStatusLeftDirs,  _P("1 directory", "%x directories", statsLeft.folderCount));
    setText(*m_staticTextStatusLeftFiles, _P("1 file", "%x files", statsLeft.fileCount));
    setText(*m_staticTextStatusLeftBytes, L'(' + formatFilesizeShort(statsLeft.bytes) + L')');
    //------------------------------------------------------------------------------
    bSizerStatusRightDirectories->Show(statsRight.folderCount > 0);
    bSizerStatusRightFiles      ->Show(statsRight.fileCount   > 0);

    setText(*m_staticTextStatusRightDirs,  _P("1 directory", "%x directories", statsRight.folderCount));
    setText(*m_staticTextStatusRightFiles, _P("1 file", "%x files", statsRight.fileCount));
    setText(*m_staticTextStatusRightBytes, L'(' + formatFilesizeShort(statsRight.bytes) + L')');
    //------------------------------------------------------------------------------
    wxString statusCenterNew;
    if (filegrid::getDataView(*m_gridMainC).rowsTotal() > 0)
    {
        statusCenterNew = _P("Showing %y of 1 item", "Showing %y of %x items", filegrid::getDataView(*m_gridMainC).rowsTotal());
        replace(statusCenterNew, L"%y", formatNumber(filegrid::getDataView(*m_gridMainC).rowsOnView())); //%x used as plural form placeholder!
    }

    //fill middle text (considering flashStatusInformation())
    if (oldStatusMsgs_.empty())
        setText(*m_staticTextStatusCenter, statusCenterNew);
    else
        oldStatusMsgs_.front() = statusCenterNew;

    m_panelStatusBar->Layout();
}


void MainDialog::applyFilterConfig()
{
    applyFiltering(folderCmp_, getConfig().mainCfg);
    updateGui();
    //updateGuiDelayedIf(currentCfg.hideExcludedItems); //show update GUI before removing rows
}


void MainDialog::applySyncDirections()
{
    if (!folderCmp_.empty())
    {
        FocusPreserver fp;

        disableGuiElements(true /*enableAbort*/); //StatusHandlerTemporaryPanel will internally process Window messages, so avoid unexpected callbacks!
        auto app = wxTheApp; //fix lambda/wxWigets/VC fuck up
        ZEN_ON_SCOPE_EXIT(app->Yield(); enableGuiElements()); //ui update before enabling buttons again: prevent strange behaviour of delayed button clicks

        const auto& guiCfg = getConfig();
        const auto& directCfgs = extractDirectionCfg(folderCmp_, getConfig().mainCfg);

        StatusHandlerTemporaryPanel statusHandler(*this, std::chrono::system_clock::now() /*startTime*/,
                                                  false /*ignoreErrors*/,
                                                  guiCfg.mainCfg.autoRetryCount,
                                                  guiCfg.mainCfg.autoRetryDelay); //handle status display and error messages
        try
        {
            statusHandler.initNewPhase(-1, -1, ProcessPhase::none);
            redetermineSyncDirection(directCfgs,
                                     statusHandler); //throw AbortProcess
        }
        catch (AbortProcess&) {}

        const StatusHandlerTemporaryPanel::Result r = statusHandler.reportResults(); //noexcept
        setLastOperationLog(r.summary, r.errorLog);
    }

    updateGui(); //e.g. unsaved changes
}


void MainDialog::onSearchGridEnter(wxCommandEvent& event)
{
    startFindNext(true /*searchAscending*/);
}


void MainDialog::onHideSearchPanel(wxCommandEvent& event)
{
    hideFindPanel();
}


void MainDialog::onSearchPanelKeyPressed(wxKeyEvent& event)
{
    switch (event.GetKeyCode())
    {
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER: //catches ENTER keys while focus is on *any* part of m_panelSearch! Seems to obsolete onSearchGridEnter()!
            startFindNext(true /*searchAscending*/);
            return;
        case WXK_ESCAPE:
            hideFindPanel();
            return;
    }
    event.Skip();
}


void MainDialog::showFindPanel() //CTRL + F or F3 with empty search phrase
{
    if (!auiMgr_.GetPane(m_panelSearch).IsShown())
    {
        auiMgr_.GetPane(m_panelSearch).Show();
        auiMgr_.Update();
    }

    m_textCtrlSearchTxt->SelectAll();

    if (wxWindow* focus = wxWindow::FindFocus()) //restore when closing panel!
        if (!isComponentOf(focus, m_panelSearch))
            focusIdAfterSearch_ = focus->GetId();
    //don't save wxWindow* to arbitrary window: it might not exist anymore when hideFindPanel() uses it!!! (e.g. some folder pair panel)

    m_textCtrlSearchTxt->SetFocus();
}


void MainDialog::hideFindPanel()
{
    if (auiMgr_.GetPane(m_panelSearch).IsShown())
    {
        auiMgr_.GetPane(m_panelSearch).Hide();
        auiMgr_.Update();
    }

    if (wxWindow* oldFocusWin = wxWindow::FindWindowById(focusIdAfterSearch_))
        oldFocusWin->SetFocus();
    focusIdAfterSearch_ = wxID_ANY;
}


void MainDialog::startFindNext(bool searchAscending) //F3 or ENTER in m_textCtrlSearchTxt
{
    const std::wstring& searchString = utfTo<std::wstring>(trimCpy(m_textCtrlSearchTxt->GetValue()));

    if (searchString.empty())
        showFindPanel();
    else
    {
        Grid* grid1 = m_gridMainL;
        Grid* grid2 = m_gridMainR;

        wxWindow* focus = wxWindow::FindFocus();
        if ((isComponentOf(focus, m_panelSearch) ? focusIdAfterSearch_ : focus->GetId()) == m_gridMainR->getMainWin().GetId())
            std::swap(grid1, grid2); //select side to start search at grid cursor position

        wxBeginBusyCursor(wxHOURGLASS_CURSOR);
        const std::pair<const Grid*, ptrdiff_t> result = findGridMatch(*grid1, *grid2, utfTo<std::wstring>(searchString),
                                                                       m_checkBoxMatchCase->GetValue(), searchAscending); //parameter owned by GUI, *not* globalCfg structure! => we should better implement a getGlocalCfg()!
        wxEndBusyCursor();

        if (Grid* grid = const_cast<Grid*>(result.first)) //grid wasn't const when passing to findAndSelectNext(), so this is safe
        {
            assert(result.second >= 0);

            filegrid::setScrollMaster(*grid);
            grid->setGridCursor(result.second, GridEventPolicy::allow);

            focusIdAfterSearch_ = grid->getMainWin().GetId();

            if (!isComponentOf(wxWindow::FindFocus(), m_panelSearch))
                grid->getMainWin().SetFocus();
        }
        else
        {
            showFindPanel();
            showNotificationDialog(this, DialogInfoType::info, PopupDialogCfg().
                                   setTitle(_("Find")).
                                   setMainInstructions(replaceCpy(_("Cannot find %x"), L"%x", fmtPath(searchString))));
        }
    }
}


void MainDialog::onTopFolderPairAdd(wxCommandEvent& event)
{
    insertAddFolderPair({ LocalPairConfig() }, 0);
    moveAddFolderPairUp(0);
}


void MainDialog::onTopFolderPairRemove(wxCommandEvent& event)
{
    assert(!additionalFolderPairs_.empty());
    if (!additionalFolderPairs_.empty())
    {
        moveAddFolderPairUp(0);
        removeAddFolderPair(0);
    }
}


void MainDialog::onLocalCompCfg(wxCommandEvent& event)
{
    const wxObject* const eventObj = event.GetEventObject(); //find folder pair originating the event
    for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
        if (eventObj == (*it)->m_bpButtonLocalCompCfg)
        {
            showConfigDialog(SyncConfigPanel::compare, (it - additionalFolderPairs_.begin()) + 1);
            break;
        }
}


void MainDialog::onLocalSyncCfg(wxCommandEvent& event)
{
    const wxObject* const eventObj = event.GetEventObject(); //find folder pair originating the event
    for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
        if (eventObj == (*it)->m_bpButtonLocalSyncCfg)
        {
            showConfigDialog(SyncConfigPanel::sync, (it - additionalFolderPairs_.begin()) + 1);
            break;
        }
}


void MainDialog::onLocalFilterCfg(wxCommandEvent& event)
{
    const wxObject* const eventObj = event.GetEventObject(); //find folder pair originating the event
    for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
        if (eventObj == (*it)->m_bpButtonLocalFilter)
        {
            showConfigDialog(SyncConfigPanel::filter, (it - additionalFolderPairs_.begin()) + 1);
            break;
        }
}


void MainDialog::onRemoveFolderPair(wxCommandEvent& event)
{
    const wxObject* const eventObj = event.GetEventObject(); //find folder pair originating the event
    for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
        if (eventObj == (*it)->m_bpButtonRemovePair)
        {
            removeAddFolderPair(it - additionalFolderPairs_.begin());
            break;
        }
}


void MainDialog::onShowFolderPairOptions(wxEvent& event)
{
    const wxObject* const eventObj = event.GetEventObject(); //find folder pair originating the event
    for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
        if (eventObj == (*it)->m_bpButtonFolderPairOptions)
        {
            const ptrdiff_t pos = it - additionalFolderPairs_.begin();

            ContextMenu menu;
            menu.addItem(_("Add folder pair"), [this, pos] { insertAddFolderPair({ LocalPairConfig() },  pos); }, loadImage("item_add_sicon"));
            menu.addSeparator();
            menu.addItem(_("Move up"  ) + L"\tAlt+Page Up",   [this, pos] { moveAddFolderPairUp(pos);     }, loadImage("move_up_sicon"));
            menu.addItem(_("Move down") + L"\tAlt+Page Down", [this, pos] { moveAddFolderPairUp(pos + 1); }, loadImage("move_down_sicon"), pos + 1 < makeSigned(additionalFolderPairs_.size()));

            menu.popup(*(*it)->m_bpButtonFolderPairOptions, { (*it)->m_bpButtonFolderPairOptions->GetSize().x, 0 });
            break;
        }
}


void MainDialog::onTopFolderPairKeyEvent(wxKeyEvent& event)
{
    const int keyCode = event.GetKeyCode();

    if (event.AltDown())
        switch (keyCode)
        {
            case WXK_PAGEDOWN: //Alt + Page Down
            case WXK_NUMPAD_PAGEDOWN:
                if (!additionalFolderPairs_.empty())
                {
                    moveAddFolderPairUp(0);
                    additionalFolderPairs_[0]->m_folderPathLeft->SetFocus();
                }
                return;
        }

    event.Skip();
}


void MainDialog::onAddFolderPairKeyEvent(wxKeyEvent& event)
{
    const int keyCode = event.GetKeyCode();

    auto getAddFolderPairPos = [&]() -> ptrdiff_t //find folder pair originating the event
    {
        if (auto eventObj = dynamic_cast<const wxWindow*>(event.GetEventObject()))
            for (auto it = additionalFolderPairs_.begin(); it != additionalFolderPairs_.end(); ++it)
                if (isComponentOf(eventObj, *it))
                    return it - additionalFolderPairs_.begin();
        return -1;
    };

    if (event.AltDown())
        switch (keyCode)
        {
            case WXK_PAGEUP: //Alt + Page Up
            case WXK_NUMPAD_PAGEUP:
            {
                const ptrdiff_t pos = getAddFolderPairPos();
                if (pos >= 0)
                {
                    moveAddFolderPairUp(pos);
                    (pos == 0 ? m_folderPathLeft : additionalFolderPairs_[pos - 1]->m_folderPathLeft)->SetFocus();
                }
            }
            return;

            case WXK_PAGEDOWN: //Alt + Page Down
            case WXK_NUMPAD_PAGEDOWN:
            {
                const ptrdiff_t pos = getAddFolderPairPos();
                if (0 <= pos && pos + 1 < makeSigned(additionalFolderPairs_.size()))
                {
                    moveAddFolderPairUp(pos + 1);
                    additionalFolderPairs_[pos + 1]->m_folderPathLeft->SetFocus();
                }
            }
            return;
        }

    event.Skip();
}


void MainDialog::updateGuiForFolderPair()
{
    recalcMaxFolderPairsVisible();

    //adapt delete top folder pair button
    m_bpButtonRemovePair->Show(!additionalFolderPairs_.empty());
    m_panelTopLeft->Layout();

    //adapt local filter and sync cfg for first folder pair
    const bool showLocalCfgFirstPair = !additionalFolderPairs_.empty()   ||
                                       firstFolderPair_->getCompConfig() ||
                                       firstFolderPair_->getSyncConfig() ||
                                       !isNullFilter(firstFolderPair_->getFilterConfig());
    //harmonize with MainDialog::showConfigDialog()!

    m_bpButtonLocalCompCfg->Show(showLocalCfgFirstPair);
    m_bpButtonLocalSyncCfg->Show(showLocalCfgFirstPair);
    m_bpButtonLocalFilter ->Show(showLocalCfgFirstPair);
    setImage(*m_bpButtonSwapSides, loadImage(showLocalCfgFirstPair ? "swap_slim" : "swap"));

    //update sub-panel sizes for calculations below!!!
    m_panelTopCenter->GetSizer()->SetSizeHints(m_panelTopCenter); //~=Fit() + SetMinSize()

    const int firstPairHeight = std::max(m_panelDirectoryPairs->ClientToWindowSize(m_panelTopLeft  ->GetSize()).y,  //include m_panelDirectoryPairs window borders!
                                         m_panelDirectoryPairs->ClientToWindowSize(m_panelTopCenter->GetSize()).y); //
    const int addPairHeight = !additionalFolderPairs_.empty() ? additionalFolderPairs_[0]->GetSize().y : 0;

    const double addPairCountMax = std::max(globalCfg_.mainDlg.folderPairsVisibleMax - 1 + 0.5, 1.5);

    const double addPairCountMin = std::min<double>(1.5,             additionalFolderPairs_.size()); //add 0.5 to indicate additional folders
    const double addPairCountOpt = std::min<double>(addPairCountMax, additionalFolderPairs_.size()); //
    addPairCountLast_ = addPairCountOpt;

    wxAuiPaneInfo& dirPane = auiMgr_.GetPane(m_panelDirectoryPairs);

    //make sure user cannot fully shrink additional folder pairs
    dirPane.MinSize (-1, firstPairHeight + addPairCountMin * addPairHeight);
    dirPane.BestSize(-1, firstPairHeight + addPairCountOpt * addPairHeight);

    //########################################################################################################################
    //wxAUI hack: call wxAuiPaneInfo::Fixed() to apply best size:
    dirPane.Fixed();
    auiMgr_.Update();

    //now make resizable again
    dirPane.Resizable();
    auiMgr_.Update();
    //alternative: dirPane.Hide() + .Show() seems to work equally well

    //########################################################################################################################

    //it seems there is no GetSizer()->SetSizeHints(this)/Fit() required due to wxAui "magic"
    //=> *massive* perf improvement on OS X!
}


void MainDialog::recalcMaxFolderPairsVisible()
{
    const int firstPairHeight = std::max(m_panelDirectoryPairs->ClientToWindowSize(m_panelTopLeft  ->GetSize()).y,  //include m_panelDirectoryPairs window borders!
                                         m_panelDirectoryPairs->ClientToWindowSize(m_panelTopCenter->GetSize()).y); //
    const int addPairHeight = !additionalFolderPairs_.empty() ? additionalFolderPairs_[0]->GetSize().y :
                              m_bpButtonAddPair->GetSize().y; //an educated guess

    //assert(firstPairHeight > 0 && addPairHeight > 0); -> wxWindows::GetSize() returns 0 if main window is minimized during sync! Test with "When finished: Exit"

    if (addPairCountLast_ && firstPairHeight > 0 && addPairHeight > 0)
    {
        const double addPairCountCurrent = (m_panelDirectoryPairs->GetSize().y - firstPairHeight) / (1.0 * addPairHeight); //include m_panelDirectoryPairs window borders!

        if (numeric::dist(addPairCountCurrent, *addPairCountLast_) > 0.4) //=> presumely changed by user!
        {
            globalCfg_.mainDlg.folderPairsVisibleMax = numeric::round(addPairCountCurrent) + 1;
        }
    }
}


void MainDialog::insertAddFolderPair(const std::vector<LocalPairConfig>& newPairs, size_t pos)
{
    assert(pos <= additionalFolderPairs_.size() && additionalFolderPairs_.size() == bSizerAddFolderPairs->GetItemCount());
    pos = std::min(pos, additionalFolderPairs_.size());

    for (size_t i = 0; i < newPairs.size(); ++i)
    {
        FolderPairPanel* newPair = nullptr;
        if (!folderPairScrapyard_.empty()) //construct cheaply from "spare parts"
        {
            newPair = folderPairScrapyard_.back().release(); //transfer ownership
            folderPairScrapyard_.pop_back();
            newPair->Show();
        }
        else
        {
            newPair = new FolderPairPanel(m_scrolledWindowFolderPairs, *this,
                                          globalCfg_.mainDlg.folderLastSelectedLeft,
                                          globalCfg_.mainDlg.folderLastSelectedRight,
                                          globalCfg_.sftpKeyFileLastSelected);

            //setHistory dropdown history
            newPair->m_folderPathLeft ->setHistory(folderHistoryLeft_ );
            newPair->m_folderPathRight->setHistory(folderHistoryRight_);

            newPair->m_bpButtonFolderPairOptions->SetBitmapLabel(loadImage("button_arrow_down"));

            //set width of left folder panel
            const int width = m_panelTopLeft->GetSize().GetWidth();
            newPair->m_panelLeft->SetMinSize({width, -1});

            //register events
            newPair->m_bpButtonFolderPairOptions->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onShowFolderPairOptions(event); });
            newPair->m_bpButtonFolderPairOptions->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent&   event) { onShowFolderPairOptions(event); });
            newPair->m_bpButtonRemovePair       ->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onRemoveFolderPair     (event); });

            static_cast<FolderPairPanelGenerated*>(newPair)->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onAddFolderPairKeyEvent(event); });

            newPair->m_bpButtonLocalCompCfg->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onLocalCompCfg  (event); });
            newPair->m_bpButtonLocalSyncCfg->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onLocalSyncCfg  (event); });
            newPair->m_bpButtonLocalFilter ->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onLocalFilterCfg(event); });

            //important: make sure panel has proper default height!
            newPair->GetSizer()->SetSizeHints(newPair); //~=Fit() + SetMinSize()
        }

        bSizerAddFolderPairs->Insert(pos + i, newPair, 0, wxEXPAND);
        additionalFolderPairs_.insert(additionalFolderPairs_.begin() + pos + i, newPair);

        //wxComboBox screws up miserably if width/height is smaller than the magic number 4! Problem occurs when trying to set tooltip
        //so we have to update window sizes before setting configuration:
        newPair->setValues(newPairs[i]);
    }

    updateGuiForFolderPair();

    clearGrid(); //+ GUI update
}


void MainDialog::moveAddFolderPairUp(size_t pos)
{
    assert(pos < additionalFolderPairs_.size());
    if (pos < additionalFolderPairs_.size())
    {
        const LocalPairConfig cfgTmp = additionalFolderPairs_[pos]->getValues();
        if (pos == 0)
        {
            additionalFolderPairs_[pos]->setValues(firstFolderPair_->getValues());
            firstFolderPair_->setValues(cfgTmp);
        }
        else
        {
            additionalFolderPairs_[pos]->setValues(additionalFolderPairs_[pos - 1]->getValues());
            additionalFolderPairs_[pos - 1]->setValues(cfgTmp);
        }

        //move comparison results, too!
        if (!folderCmp_.empty())
            std::swap(folderCmp_[pos], folderCmp_[pos + 1]); //invariant: folderCmp is empty or matches number of all folder pairs

        filegrid::setData(*m_gridMainC,    folderCmp_);
        treegrid::setData(*m_gridOverview, folderCmp_);
        updateGui();
    }
}


void MainDialog::removeAddFolderPair(size_t pos)
{
    assert(pos < additionalFolderPairs_.size());
    if (pos < additionalFolderPairs_.size())
    {
        FolderPairPanel* panel = additionalFolderPairs_[pos];

        additionalFolderPairs_.erase(additionalFolderPairs_.begin() + pos);
        bSizerAddFolderPairs->Detach(panel); //Remove() does not work on wxWindow*, so do it manually
        //more (non-portable) wxWidgets bullshit: on OS X wxWindow::Destroy() screws up and calls "operator delete" directly rather than
        //the deferred deletion it is expected to do (and which is implemented correctly on Windows and Linux)
        //http://bb10.com/python-wxpython-devel/2012-09/msg00004.html
        //=> since we're in a mouse button callback of a sub-component of "panel" we need to delay deletion ourselves:
        panel->Hide();
        folderPairScrapyard_.emplace_back(panel); //transfer ownership

        updateGuiForFolderPair();
        clearGrid(pos + 1); //+ GUI update
    }
}


void MainDialog::setAddFolderPairs(const std::vector<LocalPairConfig>& newPairs)
{
    //FolderPairPanel are too expensive to casually throw away and recreate!
    for (FolderPairPanel* panel : additionalFolderPairs_)
    {
        panel->Hide();
        folderPairScrapyard_.emplace_back(panel); //transfer ownership
    }
    additionalFolderPairs_.clear();
    bSizerAddFolderPairs->Clear(false /*delete_windows*/); //release ownership

    insertAddFolderPair(newPairs, 0);
}


//########################################################################################################


void MainDialog::onMenuOptions(wxCommandEvent& event)
{
    showOptionsDlg(this, globalCfg_);
}


void MainDialog::onMenuExportFileList(wxCommandEvent& event)
{
    std::optional<Zstring> defaultFolderPath = getParentFolderPath(globalCfg_.csvFileLastSelected);

    const Zstring defaultFileName = !globalCfg_.csvFileLastSelected.empty() ?
                                    afterLast(globalCfg_.csvFileLastSelected, FILE_NAME_SEPARATOR, IfNotFoundReturn::all) :
                                    Zstr("FileList.csv");

    wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), utfTo<wxString>(defaultFileName),
                              _("Comma-separated values") + L" (*.csv)|*.csv" + L"|" +_("All files") + L" (*.*)|*",
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fileSelector.ShowModal() != wxID_OK)
        return;
    const Zstring csvFilePath = globalCfg_.csvFileLastSelected = utfTo<Zstring>(fileSelector.GetPath());
    //---------------------------------------------------------------------

    wxBusyCursor dummy;

    //https://en.wikipedia.org/wiki/Comma-separated_values
    const lconv* localInfo = ::localeconv(); //always bound according to doc
    const bool haveCommaAsDecimalSep = std::string(localInfo->decimal_point) == ",";

    const char CSV_SEP = haveCommaAsDecimalSep ? ';' : ',';

    auto fmtValue = [&](const std::wstring& val) -> std::string
    {
        std::string&& tmp = utfTo<std::string>(val);

        if (contains(tmp, CSV_SEP))
            return '"' + tmp + '"';
        else
            return std::move(tmp);
    };

    std::string header; //perf: wxString doesn't model exponential growth and so is out, std::string doesn't give performance guarantee!
    header += BYTE_ORDER_MARK_UTF8;

    //base folders
    header += fmtValue(_("Folder Pairs")) + LINE_BREAK;
    std::for_each(begin(folderCmp_), end(folderCmp_), [&](BaseFolderPair& baseFolder)
    {
        header += fmtValue(AFS::getDisplayPath(baseFolder.getAbstractPath< LEFT_SIDE>())) + CSV_SEP;
        header += fmtValue(AFS::getDisplayPath(baseFolder.getAbstractPath<RIGHT_SIDE>())) + LINE_BREAK;
    });
    header += LINE_BREAK;

    //write header
    auto provLeft   = m_gridMainL->getDataProvider();
    auto provCenter = m_gridMainC->getDataProvider();
    auto provRight  = m_gridMainR->getDataProvider();

    auto colAttrLeft   = m_gridMainL->getColumnConfig();
    auto colAttrCenter = m_gridMainC->getColumnConfig();
    auto colAttrRight  = m_gridMainR->getColumnConfig();

    std::erase_if(colAttrLeft,   [](const Grid::ColAttributes& ca) { return !ca.visible; });
    std::erase_if(colAttrCenter, [](const Grid::ColAttributes& ca) { return !ca.visible || static_cast<ColumnTypeCenter>(ca.type) == ColumnTypeCenter::checkbox; });
    std::erase_if(colAttrRight,  [](const Grid::ColAttributes& ca) { return !ca.visible; });

    if (provLeft && provCenter && provRight)
    {
        for (const Grid::ColAttributes& ca : colAttrLeft)
        {
            header += fmtValue(provLeft->getColumnLabel(ca.type));
            header += CSV_SEP;
        }
        for (const Grid::ColAttributes& ca : colAttrCenter)
        {
            header += fmtValue(provCenter->getColumnLabel(ca.type));
            header += CSV_SEP;
        }
        if (!colAttrRight.empty())
        {
            std::for_each(colAttrRight.begin(), colAttrRight.end() - 1,
                          [&](const Grid::ColAttributes& ca)
            {
                header += fmtValue(provRight->getColumnLabel(ca.type));
                header += CSV_SEP;
            });
            header += fmtValue(provRight->getColumnLabel(colAttrRight.back().type));
        }
        header += LINE_BREAK;

        try
        {
            TempFileOutput fileOut(csvFilePath, nullptr /*notifyUnbufferedIO*/); //throw FileError

            fileOut.write(&header[0], header.size()); //throw FileError, (X)
            //main grid: write rows one after the other instead of creating one big string: memory allocation might fail; think 1 million rows!
            /*
                performance test case "export 600.000 rows" to CSV:
                aproach 1. assemble single temporary string, then write file:   4.6s
                aproach 2. write to buffered file output directly for each row: 6.4s
            */
            std::string buffer;
            const size_t rowCount = m_gridMainL->getRowCount();
            for (size_t row = 0; row < rowCount; ++row)
            {
                for (const Grid::ColAttributes& ca : colAttrLeft)
                {
                    buffer += fmtValue(provLeft->getValue(row, ca.type));
                    buffer += CSV_SEP;
                }

                for (const Grid::ColAttributes& ca : colAttrCenter)
                {
                    buffer += fmtValue(provCenter->getValue(row, ca.type));
                    buffer += CSV_SEP;
                }

                for (const Grid::ColAttributes& ca : colAttrRight)
                {
                    buffer += fmtValue(provRight->getValue(row, ca.type));
                    buffer += CSV_SEP;
                }
                buffer += LINE_BREAK;

                fileOut.write(&buffer[0], buffer.size()); //throw FileError, (X)
                buffer.clear();
            }
            fileOut.commit(); //throw FileError, (X)

            flashStatusInformation(_("File list exported"));
        }
        catch (const FileError& e)
        {
            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        }
    }
}


void MainDialog::onMenuCheckVersion(wxCommandEvent& event)
{
    checkForUpdateNow(*this, globalCfg_.lastOnlineVersion);
}


void MainDialog::onMenuCheckVersionAutomatically(wxCommandEvent& event)
{
    if (updateCheckActive(globalCfg_.lastUpdateCheck))
        disableUpdateCheck(globalCfg_.lastUpdateCheck);
    else
        globalCfg_.lastUpdateCheck = 0; //reset to GlobalSettings.xml default value!

    m_menuItemCheckVersionAuto->Check(updateCheckActive(globalCfg_.lastUpdateCheck));

    if (shouldRunAutomaticUpdateCheck(globalCfg_.lastUpdateCheck))
    {
        flashStatusInformation(_("Searching for program updates..."));
        //synchronous update check is sufficient here:
        automaticUpdateCheckEval(this, globalCfg_.lastUpdateCheck, globalCfg_.lastOnlineVersion,
                                 automaticUpdateCheckRunAsync(automaticUpdateCheckPrepare(*this).get()).get());
    }
}


void MainDialog::onStartupUpdateCheck(wxIdleEvent& event)
{
    //execute just once per startup!
    [[maybe_unused]] bool ubOk = Unbind(wxEVT_IDLE, &MainDialog::onStartupUpdateCheck, this);
    assert(ubOk);

    auto showNewVersionReminder = [this]
    {
        if (!globalCfg_.lastOnlineVersion.empty() && haveNewerVersionOnline(globalCfg_.lastOnlineVersion))
        {
            auto menu = new wxMenu();
            wxMenuItem* newItem = new wxMenuItem(menu, wxID_ANY, _("&Show details"));
            Bind(wxEVT_COMMAND_MENU_SELECTED, [this](wxCommandEvent&) { checkForUpdateNow(*this, globalCfg_.lastOnlineVersion); }, newItem->GetId());
            //show changelog + handle Donation Edition auto-updater (including expiration)
            menu->Append(newItem); //pass ownership

            const std::wstring& blackStar = utfTo<std::wstring>("★");
            m_menubar->Append(menu, blackStar + L' ' + replaceCpy(_("FreeFileSync %x is available!"), L"%x", utfTo<std::wstring>(globalCfg_.lastOnlineVersion)) + L' ' + blackStar);
        }
    };

    if (shouldRunAutomaticUpdateCheck(globalCfg_.lastUpdateCheck))
    {
        flashStatusInformation(_("Searching for program updates..."));

        std::shared_ptr<const UpdateCheckResultPrep> resultPrep = automaticUpdateCheckPrepare(*this); //run on main thread:

        guiQueue_.processAsync([resultPrep] { return automaticUpdateCheckRunAsync(resultPrep.get()); }, //run on worker thread: (long-running part of the check)
                               [this, showNewVersionReminder] (std::shared_ptr<const UpdateCheckResult>&& resultAsync)
        {
            automaticUpdateCheckEval(this, globalCfg_.lastUpdateCheck, globalCfg_.lastOnlineVersion,
                                     resultAsync.get()); //run on main thread:
            showNewVersionReminder();
        });
    }
    else
        showNewVersionReminder();
}


void MainDialog::onLayoutWindowAsync(wxIdleEvent& event)
{
    //execute just once per startup!
    [[maybe_unused]] bool ubOk = Unbind(wxEVT_IDLE, &MainDialog::onLayoutWindowAsync, this);
    assert(ubOk);

    //adjust folder pair distortion on startup
    for (FolderPairPanel* panel : additionalFolderPairs_)
        panel->Layout();

    Layout(); //strangely this layout call works if called in next idle event only
    m_panelTopButtons->Layout();

    warn_static("test: is this ancient problem still existing?")
    //auiMgr_.Update(); //fix view filter distortion
}


void MainDialog::onMenuAbout(wxCommandEvent& event)
{
    showAboutDialog(this);
}


void MainDialog::switchProgramLanguage(wxLanguage langId)
{
    //create new dialog with respect to new language
    XmlGlobalSettings newGlobalCfg = getGlobalCfgBeforeExit();
    newGlobalCfg.programLanguage = langId;

    //show new dialog, then delete old one
    MainDialog::create(globalConfigFilePath_, &newGlobalCfg, getConfig(), activeConfigFiles_, false);

    //we don't use Close():
    //1. we don't want to show the prompt to save current config in onClose()
    //2. after getGlobalCfgBeforeExit() the old main dialog is invalid so we want to force deletion
    Destroy(); //alternative: Close(true /*force*/)
}


void MainDialog::setGridViewType(GridViewType vt)
{
    //if (m_bpButtonViewType->isActive() == value) return; support polling -> what about initialization?

    m_bpButtonViewType->setActive(vt == GridViewType::action);
    m_bpButtonViewType->SetToolTip((vt == GridViewType::action ? _("Action") : _("Difference")) + L" (F11)");

    //toggle display of sync preview in middle grid
    filegrid::setViewType(*m_gridMainC, vt);

    updateGui();
}
