// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "main_dlg.h"
#include <wx/wupdlock.h>
#include <wx/filedlg.h>
#include <wx+/bitmap_button.h>
#include <wx+/font_size.h>
#include <wx+/popup_dlg.h>
#include <wx+/image_resources.h>
#include <zen/file_access.h>
#include <zen/build_info.h>
#include <zen/time.h>
#include "config.h"
#include "tray_menu.h"
#include "app_icon.h"
#include "../icon_buffer.h"
#include "../ffs_paths.h"
#include "../version/version.h"

    #include <gtk/gtk.h>

using namespace zen;
using namespace rts;


namespace
{
    static const size_t MAX_ADD_FOLDERS = 6;


std::wstring extractJobName(const Zstring& cfgFilePath)
{
    const Zstring fileName = afterLast(cfgFilePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);
    const Zstring jobName  = beforeLast(fileName, Zstr('.'), IfNotFoundReturn::all);
    return utfTo<std::wstring>(jobName);
}


}


class rts::DirectoryPanel : public FolderGenerated
{
public:
    DirectoryPanel(wxWindow* parent, Zstring& folderLastSelected) :
        FolderGenerated(parent),
        folderSelector_(parent, *this, *m_buttonSelectFolder, *m_txtCtrlDirectory, folderLastSelected, nullptr /*staticText*/)
    {
        m_bpButtonRemoveFolder->SetBitmapLabel(loadImage("item_remove"));
    }

    void setPath(const Zstring& dirpath) { folderSelector_.setPath(dirpath); }
    Zstring getPath() const { return folderSelector_.getPath(); }

private:
    FolderSelector2 folderSelector_;
};


void MainDialog::create(const Zstring& cfgFile)
{
    /*MainDialog* frame = */ new MainDialog(cfgFile);
}


MainDialog::MainDialog(const Zstring& cfgFileName) :
    MainDlgGenerated(nullptr),
    lastRunConfigPath_(fff::getConfigDirPathPf() + Zstr("LastRun.ffs_real"))
{
    SetIcon(getRtsIcon()); //set application icon

    setRelativeFontSize(*m_buttonStart, 1.5);

    const int scrollDelta = m_buttonSelectFolderMain->GetSize().y; //more approriate than GetCharHeight() here
    m_scrolledWinFolders->SetScrollRate(scrollDelta, scrollDelta);

    m_txtCtrlDirectoryMain->SetMinSize({fastFromDIP(300), -1});
    m_spinCtrlDelay       ->SetMinSize({fastFromDIP( 70), -1}); //Hack: set size (why does wxWindow::Size() not work?)

    m_bpButtonRemoveTopFolder->Hide();
    m_panelMainFolder->Layout();

    m_bitmapBatch  ->SetBitmap(loadImage("cfg_batch_sicon"));
    m_bitmapFolders->SetBitmap(fff::IconBuffer::genericDirIcon(fff::IconBuffer::SIZE_SMALL));
    m_bitmapConsole->SetBitmap(loadImage("command_line", fastFromDIP(20)));

    m_bpButtonAddFolder      ->SetBitmapLabel(loadImage("item_add"));
    m_bpButtonRemoveTopFolder->SetBitmapLabel(loadImage("item_remove"));
    setBitmapTextLabel(*m_buttonStart, loadImage("startRts"), m_buttonStart->GetLabel(), fastFromDIP(5), fastFromDIP(8));

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) { onLocalKeyEvent(event); });


    //prepare drag & drop
    firstFolderPanel_ = std::make_unique<FolderSelector2>(this, *m_panelMainFolder, *m_buttonSelectFolderMain, *m_txtCtrlDirectoryMain, folderLastSelected_, m_staticTextFinalPath);

    //--------------------------- load config values ------------------------------------
    XmlRealConfig newConfig;

    Zstring currentConfigFile = cfgFileName;
    if (currentConfigFile.empty())
        try
        {
            if (itemStillExists(lastRunConfigPath_)) //throw FileError
                currentConfigFile = lastRunConfigPath_;
        }
        catch (FileError&) { currentConfigFile = lastRunConfigPath_; } //access error? => user should be informed

    bool loadCfgSuccess = false;
    if (!currentConfigFile.empty())
        try
        {
            std::wstring warningMsg;
            readRealOrBatchConfig(currentConfigFile, newConfig, warningMsg); //throw FileError

            if (!warningMsg.empty())
                showNotificationDialog(this, DialogInfoType::warning, PopupDialogCfg().setDetailInstructions(warningMsg));

            loadCfgSuccess = warningMsg.empty();
        }
        catch (const FileError& e)
        {
            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        }

    const bool startWatchingImmediately = loadCfgSuccess && !cfgFileName.empty();

    setConfiguration(newConfig);
    setLastUsedConfig(currentConfigFile);
    //-----------------------------------------------------------------------------------------

    Center(); //needs to be re-applied after a dialog size change! (see addFolder() within setConfiguration())

    if (startWatchingImmediately) //start watch mode directly
    {
        wxCommandEvent dummy2(wxEVT_COMMAND_BUTTON_CLICKED);
        this->onStart(dummy2);
        //don't Show()!
    }
    else
    {
        Show();
        m_buttonStart->SetFocus(); //don't "steal" focus if program is running from sys-tray"
    }

    //drag and drop .ffs_real and .ffs_batch on main dialog
    setupFileDrop(*this);
    Bind(EVENT_DROP_FILE, [this](FileDropEvent& event) { onFilesDropped(event); });
}


MainDialog::~MainDialog()
{
    //save current configuration
    const XmlRealConfig currentCfg = getConfiguration();

    try //write config to XML
    {
        writeConfig(currentCfg, lastRunConfigPath_); //throw FileError
    }
    catch (const FileError& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
    }
}


void MainDialog::onQueryEndSession()
{
    try { writeConfig(getConfiguration(), lastRunConfigPath_); } //throw FileError
    catch (FileError&) {} //we try our best do to something useful in this extreme situation - no reason to notify or even log errors here!
}


void MainDialog::onMenuAbout(wxCommandEvent& event)
{
    wxString build = utfTo<wxString>(fff::ffsVersion);
#ifndef wxUSE_UNICODE
#error what is going on?
#endif

    const wchar_t* const SPACED_BULLET = L" \u2022 ";
    build += SPACED_BULLET;

    build += LTR_MARK; //fix Arabic
#ifndef ZEN_BUILD_ARCH
#error include <zen/build_info.h>
#endif
#if ZEN_BUILD_ARCH == ZEN_ARCH_32BIT
    build += L"32 Bit";
#else
    build += L"64 Bit";
#endif

    build += SPACED_BULLET;
    build += utfTo<wxString>(formatTime(formatDateTag, getCompileTime()));

    showNotificationDialog(this, DialogInfoType::info, PopupDialogCfg().
                           setTitle(_("About")).
                           setMainInstructions(L"RealTimeSync" L"\n\n" + replaceCpy(_("Version: %x"), L"%x", build)));
}


void MainDialog::onLocalKeyEvent(wxKeyEvent& event)
{
    const int keyCode = event.GetKeyCode();
    if (keyCode == WXK_ESCAPE)
    {
        Close();
        return;
    }
    event.Skip();
}


void MainDialog::onStart(wxCommandEvent& event)
{
    Hide();

    XmlRealConfig currentCfg = getConfiguration();
    const Zstring activeCfgFilePath = !equalNativePath(activeConfigFile_, lastRunConfigPath_) ? activeConfigFile_ : Zstring();

    switch (runFolderMonitor(currentCfg, ::extractJobName(activeCfgFilePath)))
    {
        case AbortReason::REQUEST_EXIT:
            Close();
            return;

        case AbortReason::REQUEST_GUI:
            break;
    }

    Show(); //don't show for AbortReason::REQUEST_EXIT
    Raise();
    m_buttonStart->SetFocus();
}


void MainDialog::onConfigSave(wxCommandEvent& event)
{
    const Zstring activeCfgFilePath = !equalNativePath(activeConfigFile_, lastRunConfigPath_) ? activeConfigFile_ : Zstring();

    std::optional<Zstring> defaultFolderPath = getParentFolderPath(activeCfgFilePath);

    Zstring defaultFileName = !activeCfgFilePath.empty() ?
                              afterLast(activeCfgFilePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all) :
                              Zstr("RealTime.ffs_real");

    //attention: activeConfigFile_ may be an imported *.ffs_batch file! We don't want to overwrite it with a RTS config!
    defaultFileName = beforeLast(defaultFileName, Zstr('.'), IfNotFoundReturn::all) + Zstr(".ffs_real");

    wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), utfTo<wxString>(defaultFileName),
                              wxString(L"RealTimeSync (*.ffs_real)|*.ffs_real") + L"|" +_("All files") + L" (*.*)|*",
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fileSelector.ShowModal() != wxID_OK)
        return;
    const Zstring targetFilePath = utfTo<Zstring>(fileSelector.GetPath());

    const XmlRealConfig currentCfg = getConfiguration();
    try
    {
        writeConfig(currentCfg, targetFilePath); //throw FileError
        setLastUsedConfig(targetFilePath);
    }
    catch (const FileError& e)
    {
        showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
    }
}


void MainDialog::loadConfig(const Zstring& filepath)
{
    XmlRealConfig newConfig;

    if (!filepath.empty())
        try
        {
            std::wstring warningMsg;
            readRealOrBatchConfig(filepath, newConfig, warningMsg); //throw FileError

            if (!warningMsg.empty())
                showNotificationDialog(this, DialogInfoType::warning, PopupDialogCfg().setDetailInstructions(warningMsg));
        }
        catch (const FileError& e)
        {
            showNotificationDialog(this, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
            return;
        }

    setConfiguration(newConfig);
    setLastUsedConfig(filepath);
}


void MainDialog::setLastUsedConfig(const Zstring& filepath)
{
    activeConfigFile_ = filepath;

    const Zstring activeCfgFilePath = !equalNativePath(activeConfigFile_, lastRunConfigPath_) ? activeConfigFile_ : Zstring();

    if (!activeCfgFilePath.empty())
        SetTitle(utfTo<wxString>(activeCfgFilePath));
    else
        SetTitle(L"RealTimeSync " + utfTo<std::wstring>(fff::ffsVersion) + SPACED_DASH + _("Automated Synchronization"));

}


void MainDialog::onConfigLoad(wxCommandEvent& event)
{
    const Zstring activeCfgFilePath = !equalNativePath(activeConfigFile_, lastRunConfigPath_) ? activeConfigFile_ : Zstring();
    //better: use last user-selected config path instead!

    std::optional<Zstring> defaultFolderPath = getParentFolderPath(activeCfgFilePath);

    wxFileDialog fileSelector(this, wxString() /*message*/,  utfTo<wxString>(defaultFolderPath ? *defaultFolderPath : Zstr("")), wxString() /*default file name*/,
                              wxString(L"RealTimeSync (*.ffs_real; *.ffs_batch)|*.ffs_real;*.ffs_batch") + L"|" +_("All files") + L" (*.*)|*",
                              wxFD_OPEN);
    if (fileSelector.ShowModal() != wxID_OK)
        return;

    loadConfig(utfTo<Zstring>(fileSelector.GetPath()));
}


void MainDialog::onFilesDropped(FileDropEvent& event)
{
    if (!event.itemPaths_.empty())
        loadConfig(event.itemPaths_[0]);
}


void MainDialog::setConfiguration(const XmlRealConfig& cfg)
{
    const Zstring& firstFolderPath = cfg.directories.empty() ? Zstring() : cfg.directories[0];
    const std::vector<Zstring> addFolderPaths = cfg.directories.empty() ? std::vector<Zstring>() :
                                                std::vector<Zstring>(cfg.directories.begin() + 1, cfg.directories.end());

    firstFolderPanel_->setPath(firstFolderPath);

    bSizerFolders->Clear(true);
    additionalFolderPanels_.clear();

    insertAddFolder(addFolderPaths, 0);

    m_textCtrlCommand->SetValue(utfTo<wxString>(cfg.commandline));
    m_spinCtrlDelay  ->SetValue(static_cast<int>(cfg.delay));
}


XmlRealConfig MainDialog::getConfiguration()
{
    XmlRealConfig output;

    output.directories.push_back(firstFolderPanel_->getPath());

    for (const DirectoryPanel* dp : additionalFolderPanels_)
        output.directories.push_back(dp->getPath());

    output.commandline = utfTo<Zstring>(m_textCtrlCommand->GetValue());
    output.delay       = m_spinCtrlDelay->GetValue();

    return output;
}


void MainDialog::onAddFolder(wxCommandEvent& event)
{
    const Zstring topFolder = firstFolderPanel_->getPath();

    //clear existing top folder first
    firstFolderPanel_->setPath(Zstring());

    insertAddFolder({ topFolder }, 0);
}


void MainDialog::onRemoveFolder(wxCommandEvent& event)
{
    //find folder pair originating the event
    const wxObject* const eventObj = event.GetEventObject();
    for (auto it = additionalFolderPanels_.begin(); it != additionalFolderPanels_.end(); ++it)
        if (eventObj == static_cast<wxObject*>((*it)->m_bpButtonRemoveFolder))
        {
            removeAddFolder(it - additionalFolderPanels_.begin());
            return;
        }
}


void MainDialog::onRemoveTopFolder(wxCommandEvent& event)
{
    if (!additionalFolderPanels_.empty())
    {
        firstFolderPanel_->setPath(additionalFolderPanels_[0]->getPath());
        removeAddFolder(0); //remove first of additional folders
    }
}


void MainDialog::insertAddFolder(const std::vector<Zstring>& newFolders, size_t pos)
{
    assert(pos <= additionalFolderPanels_.size() && additionalFolderPanels_.size() == bSizerFolders->GetItemCount());
    pos = std::min(pos, additionalFolderPanels_.size());

    for (size_t i = 0; i < newFolders.size(); ++i)
    {
        //add new folder pair
        DirectoryPanel* newFolder = new DirectoryPanel(m_scrolledWinFolders, folderLastSelected_);

        bSizerFolders->Insert(pos + i, newFolder, 0, wxEXPAND);
        additionalFolderPanels_.insert(additionalFolderPanels_.begin() + pos + i, newFolder);

        //register events
        newFolder->m_bpButtonRemoveFolder->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onRemoveFolder(event); });

        //make sure panel has proper default height
        newFolder->GetSizer()->SetSizeHints(newFolder); //~=Fit() + SetMinSize()

        newFolder->setPath(newFolders[i]);
    }

    //set size of scrolled window
    const int folderHeight = additionalFolderPanels_.empty() ? 0 : additionalFolderPanels_[0]->GetSize().GetHeight();
    const size_t visibleRows = std::min(additionalFolderPanels_.size(), MAX_ADD_FOLDERS); //up to MAX_ADD_FOLDERS additional folders shall be shown

    m_scrolledWinFolders->SetMinSize({-1, folderHeight * static_cast<int>(visibleRows)});

    //adapt delete top folder pair button
    m_bpButtonRemoveTopFolder->Show(!additionalFolderPanels_.empty());

    GetSizer()->SetSizeHints(this); //~=Fit() + SetMinSize()

    m_scrolledWinFolders->Layout(); //fix GUI distortion after .ffs_batch drag & drop (Linux)

    Refresh(); //remove a little flicker near the start button
}


void MainDialog::removeAddFolder(size_t pos)
{
    if (pos < additionalFolderPanels_.size())
    {
        //remove folder pairs from window
        DirectoryPanel* pairToDelete = additionalFolderPanels_[pos];

        bSizerFolders->Detach(pairToDelete); //Remove() does not work on Window*, so do it manually
        additionalFolderPanels_.erase(additionalFolderPanels_.begin() + pos); //remove last element in vector
        //more (non-portable) wxWidgets bullshit: on OS X wxWindow::Destroy() screws up and calls "operator delete" directly rather than
        //the deferred deletion it is expected to do (and which is implemented correctly on Windows and Linux)
        //http://bb10.com/python-wxpython-devel/2012-09/msg00004.html
        //=> since we're in a mouse button callback of a sub-component of "pairToDelete" we need to delay deletion ourselves:
        guiQueue_.processAsync([] {}, [pairToDelete] { pairToDelete->Destroy(); });

        //set size of scrolled window
        const int folderHeight = additionalFolderPanels_.empty() ? 0 : additionalFolderPanels_[0]->GetSize().GetHeight();
        const size_t visibleRows = std::min(additionalFolderPanels_.size(), MAX_ADD_FOLDERS); //up to MAX_ADD_FOLDERS additional folders shall be shown

        m_scrolledWinFolders->SetMinSize({-1, folderHeight * static_cast<int>(visibleRows)});
        m_scrolledWinFolders->Layout(); //[!] needed when scrollbars are shown

        //adapt delete top folder pair button
        m_bpButtonRemoveTopFolder->Show(!additionalFolderPanels_.empty());

        GetSizer()->SetSizeHints(this); //~=Fit() + SetMinSize()

        Refresh(); //remove a little flicker near the start button
    }
}
