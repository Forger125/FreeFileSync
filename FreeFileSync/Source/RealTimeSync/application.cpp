// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "application.h"
#include "main_dlg.h"
#include <zen/file_access.h>
#include <zen/thread.h>
#include <zen/shutdown.h>
#include <wx/event.h>
#include <wx/log.h>
#include <wx/tooltip.h>
#include <wx+/popup_dlg.h>
#include <wx+/image_resources.h>
#include "config.h"
#include "../base/resolve_path.h"
#include "../localization.h"
#include "../ffs_paths.h"
#include "../return_codes.h"
#include "../fatal_error.h"

    #include <gtk/gtk.h>

using namespace zen;
using namespace rts;


IMPLEMENT_APP(Application)


namespace
{
wxDEFINE_EVENT(EVENT_ENTER_EVENT_LOOP, wxCommandEvent);
}


bool Application::OnInit()
{
    //do not call wxApp::OnInit() to avoid using wxWidgets command line parser

    try { imageResourcesInit(fff::getResourceDirPf() + Zstr("Icons.zip")); }
    catch (FileError&) { assert(false); }
    //errors are not really critical in this context

    //GTK should already have been initialized by wxWidgets (see \src\gtk\app.cpp:wxApp::Initialize)
#if GTK_MAJOR_VERSION == 2
    ::gtk_rc_parse((fff::getResourceDirPf() + "Gtk2Styles.rc").c_str());

    //fix hang on Ubuntu 19.10 (see FFS's application.cpp)
    g_vfs_get_default(); //returns unowned GVfs*

#elif GTK_MAJOR_VERSION == 3
    auto loadCSS = [&](const char* fileName)
    {
        GtkCssProvider* provider = ::gtk_css_provider_new();
        ZEN_ON_SCOPE_EXIT(::g_object_unref(provider));

        GError* error = nullptr;
        ZEN_ON_SCOPE_EXIT(if (error) ::g_error_free(error));

        ::gtk_css_provider_load_from_path(provider, //GtkCssProvider* css_provider,
                                          (fff::getResourceDirPf() + fileName).c_str(), //const gchar* path,
                                          &error); //GError** error
        if (error)
            throw SysError(formatGlibError("gtk_css_provider_load_from_path", error));

        ::gtk_style_context_add_provider_for_screen(::gdk_screen_get_default(),               //GdkScreen* screen,
                                                    GTK_STYLE_PROVIDER(provider),             //GtkStyleProvider* provider,
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); //guint priority
    };
    try
    {
        loadCSS("Gtk3Styles.css"); //throw SysError
    }
    catch (const SysError& e)
    {
        std::cerr << utfTo<std::string>(e.toString()) << "\n" "Loading GTK3\'s old CSS format instead...\n";
        try
        {
            loadCSS("Gtk3Styles.old.css"); //throw SysError
        }
        catch (const SysError& e2) { std::cerr << utfTo<std::string>(e2.toString()) << '\n'; }
    }
#else
#error unknown GTK version!
#endif


    //Windows User Experience Interaction Guidelines: tool tips should have 5s timeout, info tips no timeout => compromise:
    wxToolTip::Enable(true); //yawn, a wxWidgets screw-up: wxToolTip::SetAutoPop is no-op if global tooltip window is not yet constructed: wxToolTip::Enable creates it
    wxToolTip::SetAutoPop(10'000); //https://docs.microsoft.com/en-us/windows/win32/uxguide/ctrl-tooltips-and-infotips

    SetAppName(L"RealTimeSync");

    try
    {
        fff::setLanguage(getProgramLanguage()); //throw FileError
    }
    catch (const FileError& e)
    {
        //following dialog does NOT trigger "exit on frame delete" while we are in OnInit(): https://docs.wxwidgets.org/trunk/overview_app.html#overview_app_shutdown
        showNotificationDialog(nullptr, DialogInfoType::error, PopupDialogCfg().setDetailInstructions(e.toString()));
        //continue!
    }


    Bind(wxEVT_QUERY_END_SESSION, [this](wxCloseEvent& event) { onQueryEndSession(event); }); //can veto
    Bind(wxEVT_END_SESSION,       [this](wxCloseEvent& event) { onQueryEndSession(event); }); //can *not* veto

    //Note: app start is deferred:  -> see FreeFileSync
    Bind(EVENT_ENTER_EVENT_LOOP, &Application::onEnterEventLoop, this);
    wxCommandEvent scrollEvent(EVENT_ENTER_EVENT_LOOP);
    AddPendingEvent(scrollEvent);
    return true; //true: continue processing; false: exit immediately.
}


int Application::OnExit()
{
    fff::releaseWxLocale();
    imageResourcesCleanup();
    return wxApp::OnExit();
}


wxLayoutDirection Application::GetLayoutDirection() const { return fff::getLayoutDirection(); }


void Application::onEnterEventLoop(wxEvent& event)
{
    [[maybe_unused]] bool ubOk = Unbind(EVENT_ENTER_EVENT_LOOP, &Application::onEnterEventLoop, this);
    assert(ubOk);

    //try to set config/batch- filepath set by %1 parameter
    std::vector<Zstring> commandArgs;
    for (int i = 1; i < argc; ++i)
    {
        Zstring filePath = fff::getResolvedFilePath(utfTo<Zstring>(argv[i]));

        if (!fileAvailable(filePath)) //be a little tolerant
        {
            if (fileAvailable(filePath + Zstr(".ffs_real")))
                filePath += Zstr(".ffs_real");
            else if (fileAvailable(filePath + Zstr(".ffs_batch")))
                filePath += Zstr(".ffs_batch");
            else
            {
                showNotificationDialog(nullptr, DialogInfoType::error, PopupDialogCfg().setMainInstructions(replaceCpy(_("Cannot find file %x."), L"%x", fmtPath(filePath))));
                return;
            }
        }
        commandArgs.push_back(filePath);
    }

    Zstring cfgFilename;
    if (!commandArgs.empty())
        cfgFilename = commandArgs[0];

    MainDialog::create(cfgFilename);
}


int Application::OnRun()
{
    [[maybe_unused]] const int rc = wxApp::OnRun();
    return fff::FFS_EXIT_SUCCESS; //process exit code
}


void Application::OnUnhandledException() //handles both wxApp::OnInit() + wxApp::OnRun()
{
    try
    {
        throw; //just re-throw
    }
    catch (const std::bad_alloc& e) //the only kind of exception we don't want crash dumps for
    {
        fff::logFatalError(e.what()); //it's not always possible to display a message box, e.g. corrupted stack, however low-level file output works!

        const auto titleFmt = copyStringTo<std::wstring>(wxTheApp->GetAppDisplayName()) + SPACED_DASH + _("An exception occurred");
        std::cerr << utfTo<std::string>(titleFmt + SPACED_DASH) << e.what() << '\n';
        terminateProcess(fff::FFS_EXIT_EXCEPTION);
    }
    //catch (...) -> Windows: let it crash and create mini dump!!! Linux/macOS: std::exception::what() logged to console
}


void Application::onQueryEndSession(wxEvent& event)
{
    if (auto mainWin = dynamic_cast<MainDialog*>(GetTopWindow()))
        mainWin->onQueryEndSession();
    //it's futile to try and clean up while the process is in full swing (CRASH!) => just terminate!
    terminateProcess(fff::FFS_EXIT_ABORTED);
}
