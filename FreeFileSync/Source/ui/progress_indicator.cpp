// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "progress_indicator.h"
#include <memory>
#include <wx/imaglist.h>
#include <wx/wupdlock.h>
#include <wx/app.h>
#include <zen/basic_math.h>
#include <zen/format_unit.h>
#include <zen/scope_guard.h>
#include <wx+/toggle_button.h>
#include <wx+/image_tools.h>
#include <wx+/graph.h>
#include <wx+/no_flicker.h>
#include <wx+/font_size.h>
#include <wx+/std_button_layout.h>
#include <zen/file_access.h>
#include <zen/thread.h>
#include <zen/perf.h>
#include <wx+/choice_enum.h>
#include "wx+/taskbar.h"
#include "wx+/window_tools.h"
#include "gui_generated.h"
#include "tray_icon.h"
#include "log_panel.h"
#include "app_icon.h"
#include "../ffs_paths.h"
#include "../perf_check.h"
#include "../icon_buffer.h"


using namespace zen;
using namespace fff;


namespace
{
constexpr std::chrono::seconds WINDOW_BYTES_PER_SEC  (5); //window size used for statistics
constexpr std::chrono::seconds WINDOW_REMAINING_TIME(60); //USB memory stick can have 40-second-hangs
constexpr std::chrono::seconds      SPEED_ESTIMATE_SAMPLE_SKIP(1);
constexpr std::chrono::milliseconds SPEED_ESTIMATE_UPDATE_INTERVAL(500);
constexpr std::chrono::seconds      GRAPH_TOTAL_TIME_UPDATE_INTERVAL(2);

const size_t PROGRESS_GRAPH_SAMPLE_SIZE_MAX = 2'500'000; //sizeof(single node) worst case ~ 3 * 8 byte ptr + 16 byte key/value = 40 byte

inline wxColor getColorBytes() { return { 111, 255,  99 }; } //light green
inline wxColor getColorItems() { return { 127, 147, 255 }; } //light blue

inline wxColor getColorBytesRim() { return { 20, 200,   0 }; } //medium green
inline wxColor getColorItemsRim() { return { 90, 120, 255 }; } //medium blue

//inline wxColor getColorBytesFaint() { return { 205, 255, 202 }; } //faint green
//inline wxColor getColorItemsFaint() { return { 198, 206, 255 }; } //faint blue

inline wxColor getColorBytesDark() { return { 12, 128,   0 }; } //dark green
inline wxColor getColorItemsDark() { return { 53,  25, 255 }; } //dark blue

inline wxColor getColorLightGrey() { return {0xf2, 0xf2, 0xf2}; }
inline wxColor getColorDarkGrey () { return {0x8f, 0x8f, 0x8f}; }


std::wstring getDialogPhaseText(const Statistics& syncStat, bool paused)
{
    if (paused)
        return _("Paused");

    if (syncStat.getAbortStatus())
        return _("Stop requested...");

    switch (syncStat.currentPhase())
    {
        case ProcessPhase::none:
            return _("Initializing..."); //dialog is shown *before* sync starts, so this text may be visible!
        case ProcessPhase::scanning:
            return _("Scanning...");
        case ProcessPhase::comparingContent:
            return _("Comparing content...");
        case ProcessPhase::synchronizing:
            return _("Synchronizing...");
    }
    assert(false);
    return std::wstring();
}


class CurveDataProgressBar : public CurveData
{
public:
    CurveDataProgressBar(bool drawTop) : drawTop_(drawTop) {}

    void setFraction(double fraction) { fraction_ = fraction; } //value between [0, 1]

private:
    std::pair<double, double> getRangeX() const override { return { 0, 1 }; }

    std::vector<CurvePoint> getPoints(double minX, double maxX, const wxSize& areaSizePx) const override
    {
        const double yLow  = drawTop_ ? 1 : -1; //draw partially out of vertical bounds to not render top/bottom borders of the bars
        const double yHigh = drawTop_ ? 3 :  1; //

        return
        {
            { 0,         yHigh },
            { fraction_, yHigh },
            { fraction_, yLow  },
            { 0,         yLow  },
        };
    }

    double fraction_ = 0;
    const bool drawTop_;
};

class CurveDataProgressSeparatorLine : public CurveData
{
    std::pair<double, double> getRangeX() const override { return { 0, 1 }; }

    std::vector<CurvePoint> getPoints(double minX, double maxX, const wxSize& areaSizePx) const override
    {
        return
        {
            { 0, 1 },
            { 1, 1 },
        };
    }
};
}


class CompareProgressPanel::Impl : public CompareProgressDlgGenerated
{
public:
    Impl(wxFrame& parentWindow);

    void init(const Statistics& syncStat, bool ignoreErrors, size_t autoRetryCount); //constructor/destructor semantics, but underlying Window is reused
    void teardown();                                                                 //

    void initNewPhase();
    void updateProgressGui();

    bool getOptionIgnoreErrors() const            { return ignoreErrors_; }
    void setOptionIgnoreErrors(bool ignoreErrors) { ignoreErrors_ = ignoreErrors; updateStaticGui(); }

    void timerSetStatus(bool active)
    {
        if (active)
            stopWatch_.resume();
        else
            stopWatch_.pause();
    }
    bool timerIsRunning() const { return !stopWatch_.isPaused(); }

private:
    //void onToggleIgnoreErrors(wxCommandEvent& event) override { updateStaticGui(); }

    void updateStaticGui();

    wxFrame& parentWindow_;
    wxString parentTitleBackup_;

    StopWatch stopWatch_;
    std::chrono::nanoseconds phaseStart_{}; //begin of current phase

    const Statistics* syncStat_ = nullptr; //only bound while sync is running

    std::unique_ptr<Taskbar> taskbar_;
    PerfCheck perf_{ WINDOW_REMAINING_TIME, WINDOW_BYTES_PER_SEC }; //estimate remaining time

    std::chrono::nanoseconds timeLastSpeedEstimate_ = std::chrono::seconds(-100); //used for calculating intervals between showing and collecting perf samples
    //initial value: just some big number

    std::shared_ptr<CurveDataProgressBar> curveDataBytes_{ std::make_shared<CurveDataProgressBar>(true  /*drawTop*/) };
    std::shared_ptr<CurveDataProgressBar> curveDataItems_{ std::make_shared<CurveDataProgressBar>(false /*drawTop*/) };

    bool ignoreErrors_ = false;
};


CompareProgressPanel::Impl::Impl(wxFrame& parentWindow) :
    CompareProgressDlgGenerated(&parentWindow),
    parentWindow_(parentWindow)
{
    const wxImage& imgFile = IconBuffer::genericFileIcon(IconBuffer::SIZE_SMALL);
    m_bitmapItemStat->SetBitmap(imgFile);

    const wxImage imgTime = loadImage("time", -1 /*maxWidth*/, imgFile.GetHeight());
    m_bitmapTimeStat->SetBitmap(imgTime);
    m_bitmapTimeStat->SetMinSize({-1, imgFile.GetHeight()});

    m_bitmapIgnoreErrors->SetBitmap(loadImage("error_ignore_active"));
    m_bitmapRetryErrors ->SetBitmap(loadImage("error_retry"));

    //make sure that standard height matches ProcessPhase::comparingContent statistics layout (== largest)

    //init graph
    m_panelProgressGraph->setAttributes(Graph2D::MainAttributes().setMinY(0).setMaxY(2).
                                        setLabelX(XLabelPos::none).
                                        setLabelY(YLabelPos::none).
                                        setBaseColors(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT), wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)).
                                        setSelectionMode(GraphSelMode::none));

    m_panelProgressGraph->addCurve(curveDataBytes_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(1)).fillPolygonArea(getColorBytes()).setColor(Graph2D::getBorderColor()));
    m_panelProgressGraph->addCurve(curveDataItems_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(1)).fillPolygonArea(getColorItems()).setColor(Graph2D::getBorderColor()));

    m_panelProgressGraph->addCurve(std::make_shared<CurveDataProgressSeparatorLine>(), Graph2D::CurveAttributes().setLineWidth(fastFromDIP(1)).setColor(Graph2D::getBorderColor()));

    Layout();
    m_panelItemStats->Layout();
    m_panelTimeStats->Layout();

    GetSizer()->SetSizeHints(this); //~=Fit() + SetMinSize()
    //=> works like a charm for GTK2 with window resizing problems and title bar corruption; e.g. Debian!!!
}


void CompareProgressPanel::Impl::init(const Statistics& syncStat, bool ignoreErrors, size_t autoRetryCount)
{
    syncStat_ = &syncStat;
    parentTitleBackup_ = parentWindow_.GetTitle();

    try //try to get access to Windows 7/Ubuntu taskbar
    {
        taskbar_ = std::make_unique<Taskbar>(this); //throw TaskbarNotAvailable
    }
    catch (const TaskbarNotAvailable&) {}

    stopWatch_.restart(); //measure total time

    setText(*m_staticTextRetryCount, L'(' + formatNumber(autoRetryCount) + MULT_SIGN + L')');
    bSizerErrorsRetry->Show(autoRetryCount > 0);

    //allow changing a few options dynamically during sync
    ignoreErrors_ = ignoreErrors;

    updateStaticGui();

    initNewPhase();
}


void CompareProgressPanel::Impl::teardown()
{
    syncStat_ = nullptr;
    parentWindow_.SetTitle(parentTitleBackup_);
    taskbar_.reset();
}


void CompareProgressPanel::Impl::initNewPhase()
{
    //start new measurement
    perf_ = PerfCheck(WINDOW_REMAINING_TIME, WINDOW_BYTES_PER_SEC);
    timeLastSpeedEstimate_ = std::chrono::seconds(-100); //make sure estimate is updated upon next check
    phaseStart_ = stopWatch_.elapsed();

    const int     itemsTotal = syncStat_->getStatsTotal().items;
    const int64_t bytesTotal = syncStat_->getStatsTotal().bytes;

    const bool haveTotalStats = itemsTotal >= 0 || bytesTotal >= 0;

    if (taskbar_.get()) taskbar_->setStatus(haveTotalStats ? Taskbar::STATUS_NORMAL : Taskbar::STATUS_INDETERMINATE);

    m_staticTextProcessed     ->Show(haveTotalStats);
    m_staticTextRemaining     ->Show(haveTotalStats);
    m_staticTextItemsRemaining->Show(haveTotalStats);
    m_staticTextBytesRemaining->Show(haveTotalStats);
    m_staticTextTimeRemaining ->Show(haveTotalStats);
    bSizerProgressGraph       ->Show(haveTotalStats);

    Layout();
    m_panelItemStats->Layout();
    m_panelTimeStats->Layout();

    updateProgressGui();
}


void CompareProgressPanel::Impl::updateStaticGui()
{
    bSizerErrorsIgnore->Show(ignoreErrors_);
    Layout();
}


void CompareProgressPanel::Impl::updateProgressGui()
{
    assert(syncStat_);
    if (!syncStat_) //no comparison running!!
        return;

    auto setTitle = [&](const wxString& title)
    {
        if (parentWindow_.GetTitle() != title)
            parentWindow_.SetTitle(title);
    };

    bool layoutChanged = false; //avoid screen flicker by calling layout() only if necessary
    const std::chrono::nanoseconds timeElapsed = stopWatch_.elapsed();

    const int     itemsCurrent = syncStat_->getStatsCurrent().items;
    const int64_t bytesCurrent = syncStat_->getStatsCurrent().bytes;
    const int     itemsTotal   = syncStat_->getStatsTotal  ().items;
    const int64_t bytesTotal   = syncStat_->getStatsTotal  ().bytes;

    const bool haveTotalStats = itemsTotal >= 0 || bytesTotal >= 0;

    //status texts
    setText(*m_staticTextStatus, replaceCpy(syncStat_->currentStatusText(), L'\n', L' ')); //no layout update for status texts!

    if (!haveTotalStats)
    {
        //dialog caption, taskbar
        setTitle(formatNumber(itemsCurrent) + L" | " + getDialogPhaseText(*syncStat_, false /*paused*/));

        //progress indicators
        //taskbar_ already set to STATUS_INDETERMINATE within initNewPhase()
    }
    else
    {
        //add both bytes + item count, to handle "deletion-only" cases
        const double fractionTotal = bytesTotal + itemsTotal == 0 ? 0 : 1.0 * (bytesCurrent + itemsCurrent) / (bytesTotal + itemsTotal);
        const double fractionBytes = bytesTotal == 0 ? 0 : 1.0 * bytesCurrent / bytesTotal;
        const double fractionItems = itemsTotal == 0 ? 0 : 1.0 * itemsCurrent / itemsTotal;

        //dialog caption, taskbar
        setTitle(formatFraction(fractionTotal) + L" | " + getDialogPhaseText(*syncStat_, false /*paused*/));

        //progress indicators
        if (taskbar_.get()) taskbar_->setProgress(fractionTotal);

        curveDataBytes_->setFraction(fractionBytes);
        curveDataItems_->setFraction(fractionItems);
    }

    //item and data stats
    if (!haveTotalStats)
    {
        setText(*m_staticTextItemsProcessed, formatNumber(itemsCurrent), &layoutChanged);
        setText(*m_staticTextBytesProcessed, L"", &layoutChanged);
    }
    else
    {
        setText(*m_staticTextItemsProcessed,               formatNumber(itemsCurrent), &layoutChanged);
        setText(*m_staticTextBytesProcessed, L'(' + formatFilesizeShort(bytesCurrent) + L')', &layoutChanged);

        setText(*m_staticTextItemsRemaining, formatNumber(itemsTotal - itemsCurrent), &layoutChanged);
        setText(*m_staticTextBytesRemaining, L'(' + formatFilesizeShort(bytesTotal - bytesCurrent) + L')', &layoutChanged);
    }

    //current time elapsed
    const int64_t timeElapSec = std::chrono::duration_cast<std::chrono::seconds>(timeElapsed).count();

    setText(*m_staticTextTimeElapsed, timeElapSec < 3600 ?
            wxTimeSpan::Seconds(timeElapSec).Format(   L"%M:%S") :
            wxTimeSpan::Seconds(timeElapSec).Format(L"%H:%M:%S"), &layoutChanged);

    if (haveTotalStats) //remaining time and speed: only visible during binary comparison
        if (numeric::dist(timeLastSpeedEstimate_, timeElapsed) >= SPEED_ESTIMATE_UPDATE_INTERVAL)
        {
            timeLastSpeedEstimate_ = timeElapsed;

            if (numeric::dist(phaseStart_, timeElapsed) >= SPEED_ESTIMATE_SAMPLE_SKIP) //discard stats for first second: probably messy
                perf_.addSample(timeElapsed, itemsCurrent, bytesCurrent);

            //current speed -> Win 7 copy uses 1 sec update interval instead
            std::optional<std::wstring> bps = perf_.getBytesPerSecond();
            std::optional<std::wstring> ips = perf_.getItemsPerSecond();
            m_panelProgressGraph->setAttributes(m_panelProgressGraph->getAttributes().setCornerText(bps ? *bps : L"", GraphCorner::topL));
            m_panelProgressGraph->setAttributes(m_panelProgressGraph->getAttributes().setCornerText(ips ? *ips : L"", GraphCorner::bottomL));

            //remaining time: display with relative error of 10% - based on samples taken every 0.5 sec only
            //-> call more often than once per second to correctly show last few seconds countdown, but don't call too often to avoid occasional jitter
            std::optional<double> remTimeSec = perf_.getRemainingTimeSec(bytesTotal - bytesCurrent);
            setText(*m_staticTextTimeRemaining, remTimeSec ? formatRemainingTime(*remTimeSec) : std::wstring(1, EM_DASH), &layoutChanged);
        }

    if (haveTotalStats)
        m_panelProgressGraph->Refresh();

    //adapt layout after content changes above
    if (layoutChanged)
    {
        Layout();
        m_panelItemStats->Layout();
        m_panelTimeStats->Layout();
    }

    //do the ui update
    wxTheApp->Yield();
}

//########################################################################################

//redirect to implementation
CompareProgressPanel::CompareProgressPanel(wxFrame& parentWindow) : pimpl_(new Impl(parentWindow)) {} //owned by parentWindow
wxWindow* CompareProgressPanel::getAsWindow() { return pimpl_; }
void CompareProgressPanel::init(const Statistics& syncStat, bool ignoreErrors, size_t autoRetryCount) { pimpl_->init(syncStat, ignoreErrors, autoRetryCount); }
void CompareProgressPanel::teardown()     { pimpl_->teardown(); }
void CompareProgressPanel::initNewPhase() { pimpl_->initNewPhase(); }
void CompareProgressPanel::updateGui()    { pimpl_->updateProgressGui(); }
bool CompareProgressPanel::getOptionIgnoreErrors() const { return pimpl_->getOptionIgnoreErrors(); }
void CompareProgressPanel::setOptionIgnoreErrors(bool ignoreErrors) { pimpl_->setOptionIgnoreErrors(ignoreErrors); }
void CompareProgressPanel::timerSetStatus(bool active) { pimpl_->timerSetStatus(active); }
bool CompareProgressPanel::timerIsRunning() const { return pimpl_->timerIsRunning(); }

//########################################################################################

namespace
{
class CurveDataStatistics : public SparseCurveData
{
public:
    CurveDataStatistics() : SparseCurveData(true /*addSteps*/) {}

    void clear() { samples_.clear(); lastSample_ = {}; }

    void addRecord(std::chrono::nanoseconds timeElapsed, double value)
    {
        assert(!samples_.empty() || (lastSample_ == std::pair<std::chrono::nanoseconds, double>()));

        lastSample_ = { timeElapsed, value };

        //allow for at most one sample per 100ms (handles duplicate inserts, too!) => this is unrelated to UI_UPDATE_INTERVAL!
        if (!samples_.empty()) //always unconditionally insert first sample!
            if (numeric::dist(timeElapsed, samples_.rbegin()->first) < std::chrono::milliseconds(100))
                return;

        samples_.insert(samples_.end(), { timeElapsed, value }); //time is "expected" to be monotonously ascending
        //documentation differs about whether "hint" should be before or after the to be inserted element!
        //however "std::map<>::end()" is interpreted correctly by GCC and VS2010

        if (samples_.size() > PROGRESS_GRAPH_SAMPLE_SIZE_MAX) //limit buffer size
            samples_.erase(samples_.begin());
    }

private:
    std::pair<double, double> getRangeX() const override
    {
        if (samples_.empty())
            return {};

        const std::chrono::nanoseconds upperEnd = std::max(samples_.rbegin()->first, lastSample_.first);

        /*
        //report some additional width by 5% elapsed time to make graph recalibrate before hitting the right border
        //caveat: graph for batch mode binary comparison does NOT start at elapsed time 0!! ProcessPhase::comparingContent and ProcessPhase::synchronizing!
        //=> consider width of current sample set!
        upperEndMs += 0.05 *(upperEndMs - samples.begin()->first);
        */

        return { std::chrono::duration<double>(samples_.begin()->first).count(), //need not start with 0, e.g. "binary comparison, graph reset, followed by sync"
                 std::chrono::duration<double>(upperEnd).count() };
    }

    std::optional<CurvePoint> getLessEq(double x) const override //x: seconds since begin
    {
        const auto timeX = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(x)); //round down

        //------ add artifical last sample value -------
        if (!samples_.empty() && samples_.rbegin()->first < lastSample_.first)
            if (lastSample_.first <= timeX)
                return CurvePoint{std::chrono::duration<double>(lastSample_.first).count(), lastSample_.second};
        //--------------------------------------------------

        //find first key > x, then go one step back: => samples must be a std::map, NOT std::multimap!!!
        auto it = samples_.upper_bound(timeX);
        if (it == samples_.begin())
            return std::nullopt;
        //=> samples not empty in this context
        --it;
        return CurvePoint{std::chrono::duration<double>(it->first).count(), it->second};
    }

    std::optional<CurvePoint> getGreaterEq(double x) const override
    {
        const std::chrono::nanoseconds timeX(static_cast<std::chrono::nanoseconds::rep>(std::ceil(x * (1000 * 1000 * 1000)))); //round up!

        //------ add artifical last sample value -------
        if (!samples_.empty() && samples_.rbegin()->first < lastSample_.first)
            if (samples_.rbegin()->first < timeX && timeX <= lastSample_.first)
                return CurvePoint{std::chrono::duration<double>(lastSample_.first).count(), lastSample_.second};
        //--------------------------------------------------

        auto it = samples_.lower_bound(timeX);
        if (it == samples_.end())
            return std::nullopt;
        return CurvePoint{std::chrono::duration<double>(it->first).count(), it->second};
    }

    std::map <std::chrono::nanoseconds, double> samples_;    //[!] don't use std::multimap, see getLessEq()
    std::pair<std::chrono::nanoseconds, double> lastSample_; //artificial most current record at the end of samples to visualize current time!
};


class CurveDataEstimate : public CurveData
{
public:
    void setValue(double x1, double x2, double y1, double y2) { x1_ = x1; x2_ = x2; y1_ = y1; y2_ = y2; }
    void setTotalTime(double x2) { x2_ = x2; }
    double getTotalTime() const { return x2_; }

private:
    std::pair<double, double> getRangeX() const override { return { x1_, x2_ }; }

    std::vector<CurvePoint> getPoints(double minX, double maxX, const wxSize& areaSizePx) const override
    {
        return
        {
            { x1_, y1_ },
            { x2_, y2_ },
        };
    }

    double x1_ = 0; //elapsed time [s]
    double x2_ = 0; //total time [s] (estimated)
    double y1_ = 0; //items/bytes processed
    double y2_ = 0; //items/bytes total
};


class CurveDataTimeMarker : public CurveData
{
public:
    void setValue(double x, double y) { x_ = x; y_ = y; }
    void setTime(double x) { x_ = x; }

private:
    std::pair<double, double> getRangeX() const override { return { x_, x_ }; }

    std::vector<CurvePoint> getPoints(double minX, double maxX, const wxSize& areaSizePx) const override
    {
        return
        {
            { x_, y_ },
            { x_, 0  },
        };
    }

    double x_ = 0; //time [s]
    double y_ = 0; //items/bytes
};


const double stretchDefaultBlockSize = 1.4; //enlarge block default size


struct LabelFormatterBytes : public LabelFormatter
{
    double getOptimalBlockSize(double bytesProposed) const override
    {
        bytesProposed *= stretchDefaultBlockSize; //enlarge block default size

        if (bytesProposed <= 1) //never smaller than 1 byte
            return 1;

        //round to next number which is a convenient to read block size
        const double k = std::floor(std::log(bytesProposed) / std::log(2.0));
        const double e = std::pow(2.0, k);
        if (numeric::isNull(e))
            return 0;
        const double a = bytesProposed / e; //bytesProposed = a * 2^k with a in [1, 2)
        assert(1 <= a && a < 2);
        const double steps[] = { 1, 2 };
        return e * numeric::nearMatch(a, std::begin(steps), std::end(steps));
    }

    wxString formatText(double value, double optimalBlockSize) const override { return formatFilesizeShort(static_cast<int64_t>(value)); }
};


struct LabelFormatterItemCount : public LabelFormatter
{
    double getOptimalBlockSize(double itemsProposed) const override
    {
        itemsProposed *= stretchDefaultBlockSize; //enlarge block default size

        const double steps[] = { 1, 2, 5, 10 };
        if (itemsProposed <= 10)
            return numeric::nearMatch(itemsProposed, std::begin(steps), std::end(steps)); //like nextNiceNumber(), but without the 2.5 step!
        return nextNiceNumber(itemsProposed);
    }

    wxString formatText(double value, double optimalBlockSize) const override
    {
        return formatNumber(numeric::round(value)); //not enough room for a "%x items" representation
    }
};


struct LabelFormatterTimeElapsed : public LabelFormatter
{
    LabelFormatterTimeElapsed(bool drawLabel) : drawLabel_(drawLabel) {}

    double getOptimalBlockSize(double secProposed) const override
    {
        //5 sec minimum block size
        const double stepsSec[] = { 5, 10, 20, 30, 60 }; //nice numbers for seconds
        if (secProposed <= 60)
            return numeric::nearMatch(secProposed, std::begin(stepsSec), std::end(stepsSec));

        const double stepsMin[] = { 1, 2, 5, 10, 15, 20, 30, 60 }; //nice numbers for minutes
        if (secProposed <= 3600)
            return 60 * numeric::nearMatch(secProposed / 60, std::begin(stepsMin), std::end(stepsMin));

        if (secProposed <= 3600 * 24)
            return 3600 * nextNiceNumber(secProposed / 3600); //round up to full hours

        return 24 * 3600 * nextNiceNumber(secProposed / (24 * 3600)); //round to full days
    }

    wxString formatText(double timeElapsed, double optimalBlockSize) const override
    {
        if (!drawLabel_)
            return wxString();

        return timeElapsed < 60 ?
               wxString(_P("1 sec", "%x sec", numeric::round(timeElapsed))) :
               timeElapsed < 3600 ?
               wxTimeSpan::Seconds(timeElapsed).Format(   L"%M:%S") :
               wxTimeSpan::Seconds(timeElapsed).Format(L"%H:%M:%S");
    }

private:
    const bool drawLabel_;
};
}


template <class TopLevelDialog> //can be a wxFrame or wxDialog
class SyncProgressDialogImpl : public TopLevelDialog, public SyncProgressDialog
/*we need derivation, not composition!
      1. SyncProgressDialogImpl IS a wxFrame/wxDialog
      2. implement virtual ~wxFrame()
      3. event handling below assumes lifetime is larger-equal than wxFrame's
*/
{
public:
    SyncProgressDialogImpl(long style, //wxFrame/wxDialog style
                           wxSize dlgSize, bool dlgMaximize,
                           const std::function<void()>& userRequestAbort,
                           const Statistics& syncStat,
                           wxFrame* parentFrame,
                           bool showProgress,
                           bool autoCloseDialog,
                           const std::vector<std::wstring>& jobNames,
                           const std::chrono::system_clock::time_point& syncStartTime,
                           bool ignoreErrors,
                           size_t autoRetryCount,
                           PostSyncAction2 postSyncAction);

    Result destroy(bool autoClose, bool restoreParentFrame, SyncResult syncResult, const SharedRef<const zen::ErrorLog>& log) override;

    wxWindow* getWindowIfVisible() override { return this->IsShown() ? this : nullptr; }
    //workaround OS X bug: if "this" is used as parent window for a modal dialog then this dialog will erroneously un-hide its parent!

    void initNewPhase        () override;
    void notifyProgressChange() override;
    void updateGui           () override { updateProgressGui(true /*allowYield*/); }

    bool getOptionIgnoreErrors()                 const override { return ignoreErrors_; }
    void setOptionIgnoreErrors(bool ignoreErrors)      override { ignoreErrors_ = ignoreErrors; updateStaticGui(); }
    PostSyncAction2 getOptionPostSyncAction()    const override { return getEnumVal(enumPostSyncAction_, *pnl_.m_choicePostSyncAction); }
    bool getOptionAutoCloseDialog()              const override { return pnl_.m_checkBoxAutoClose->GetValue(); }

    void timerSetStatus(bool active) override
    {
        if (active)
            stopWatch_.resume();
        else
            stopWatch_.pause();
    }

    bool timerIsRunning() const override
    {
        return !stopWatch_.isPaused();
    }

private:
    void onLocalKeyEvent (wxKeyEvent& event);
    void onParentKeyEvent(wxKeyEvent& event);
    void onPause  (wxCommandEvent& event);
    void onCancel (wxCommandEvent& event);
    void onClose(wxCloseEvent& event);
    void onIconize(wxIconizeEvent& event);
    //void onToggleIgnoreErrors(wxCommandEvent& event) { updateStaticGui(); }

    void showSummary(SyncResult syncResult, const SharedRef<const ErrorLog>& log);

    void minimizeToTray();
    void resumeFromSystray(bool userRequested);

    void updateStaticGui();
    void updateProgressGui(bool allowYield);

    void setExternalStatus(const wxString& status, const wxString& progress); //progress may be empty!

    SyncProgressPanelGenerated& pnl_; //wxPanel containing the GUI controls of *this

    const std::chrono::system_clock::time_point& syncStartTime_;
    const wxString jobName_;
    StopWatch stopWatch_;

    wxFrame* parentFrame_; //optional

    const std::function<void()> userRequestAbort_; //cancel button or dialog close

    //status variables
    const Statistics* syncStat_; //valid only while sync is running
    bool paused_ = false;
    bool closePressed_ = false;

    //remaining time
    PerfCheck perf_{ WINDOW_REMAINING_TIME, WINDOW_BYTES_PER_SEC };
    std::chrono::nanoseconds timeLastSpeedEstimate_    = std::chrono::seconds(-100); //used for calculating intervals between collecting perf samples
    std::chrono::nanoseconds timeLastGraphTotalUpdate_ = std::chrono::seconds(-100);

    //help calculate total speed
    std::chrono::nanoseconds phaseStart_{}; //begin of current phase

    std::shared_ptr<CurveDataStatistics> curveBytes_          = std::make_shared<CurveDataStatistics>();
    std::shared_ptr<CurveDataStatistics> curveItems_          = std::make_shared<CurveDataStatistics>();
    std::shared_ptr<CurveDataEstimate  > curveBytesEstim_     = std::make_shared<CurveDataEstimate>  ();
    std::shared_ptr<CurveDataEstimate  > curveItemsEstim_     = std::make_shared<CurveDataEstimate>  ();
    std::shared_ptr<CurveDataTimeMarker> curveBytesTimeNow_   = std::make_shared<CurveDataTimeMarker>();
    std::shared_ptr<CurveDataTimeMarker> curveItemsTimeNow_   = std::make_shared<CurveDataTimeMarker>();
    std::shared_ptr<CurveDataTimeMarker> curveBytesTimeEstim_ = std::make_shared<CurveDataTimeMarker>();
    std::shared_ptr<CurveDataTimeMarker> curveItemsTimeEstim_ = std::make_shared<CurveDataTimeMarker>();

    wxString parentTitleBackup_;
    std::unique_ptr<FfsTrayIcon> trayIcon_; //optional: if filled all other windows should be hidden and conversely
    std::unique_ptr<Taskbar> taskbar_;

    bool ignoreErrors_ = false;
    EnumDescrList<PostSyncAction2> enumPostSyncAction_;

    wxSize dlgSizeBuf_;
};


template <class TopLevelDialog>
SyncProgressDialogImpl<TopLevelDialog>::SyncProgressDialogImpl(long style, //wxFrame/wxDialog style
                                                               wxSize dlgSize, bool dlgMaximize,
                                                               const std::function<void()>& userRequestAbort,
                                                               const Statistics& syncStat,
                                                               wxFrame* parentFrame,
                                                               bool showProgress,
                                                               bool autoCloseDialog,
                                                               const std::vector<std::wstring>& jobNames,
                                                               const std::chrono::system_clock::time_point& syncStartTime,
                                                               bool ignoreErrors,
                                                               size_t autoRetryCount,
                                                               PostSyncAction2 postSyncAction) :
    TopLevelDialog(parentFrame, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize, style), //title is overwritten anyway in setExternalStatus()
    pnl_(*new SyncProgressPanelGenerated(this)), //ownership passed to "this"
    syncStartTime_(syncStartTime),
    jobName_([&]
{
    std::wstring tmp;
    if (!jobNames.empty())
    {
        tmp = jobNames[0];
        std::for_each(jobNames.begin() + 1, jobNames.end(), [&](const std::wstring& jobName)
        { tmp += L" + " + jobName; });
    }
    return tmp;
}
()),
parentFrame_(parentFrame),
userRequestAbort_(userRequestAbort),
syncStat_(&syncStat),
dlgSizeBuf_(dlgSize)
{
    static_assert(std::is_same_v<TopLevelDialog, wxFrame > ||
                  std::is_same_v<TopLevelDialog, wxDialog>);
    assert((std::is_same_v<TopLevelDialog, wxFrame> == !parentFrame));

    //finish construction of this dialog:
    this->pnl_.m_panelProgress->SetMinSize({fastFromDIP(550), fastFromDIP(340)});

    wxBoxSizer* bSizer170 = new wxBoxSizer(wxVERTICAL);
    bSizer170->Add(&pnl_, 1, wxEXPAND);
    this->SetSizer(bSizer170); //pass ownership

    //lifetime of event sources is subset of this instance's lifetime => no wxEvtHandler::Unbind() needed
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&   event) { onClose(event); });
    this->Bind(wxEVT_ICONIZE,      [this](wxIconizeEvent& event) { onIconize(event); });
    this->Bind(wxEVT_CHAR_HOOK,    [this](wxKeyEvent&     event) { onLocalKeyEvent(event); });

    pnl_.m_buttonClose           ->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { closePressed_ = true; });
    pnl_.m_buttonPause           ->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onPause(event); });
    pnl_.m_buttonStop            ->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { onCancel(event); });
    pnl_.m_bpButtonMinimizeToTray->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) { minimizeToTray(); });

    if (parentFrame_)
        parentFrame_->Bind(wxEVT_CHAR_HOOK, &SyncProgressDialogImpl::onParentKeyEvent, this);


    assert(pnl_.m_buttonClose->GetId() == wxID_OK); //we cannot use wxID_CLOSE else ESC key won't work: yet another wxWidgets bug??

    setRelativeFontSize(*pnl_.m_staticTextPhase, 1.5);

    if (parentFrame_)
        parentTitleBackup_ = parentFrame_->GetTitle(); //save old title (will be used as progress indicator)

    //pnl.m_animCtrlSyncing->SetAnimation(getResourceAnimation(L"working"));
    //pnl.m_animCtrlSyncing->Play();

    //this->EnableCloseButton(false); //this is NOT honored on OS X or with ALT+F4 on Windows! -> why disable button at all??

    stopWatch_.restart(); //measure total time

    try //try to get access to Windows 7/Ubuntu taskbar
    {
        taskbar_ = std::make_unique<Taskbar>(this); //throw TaskbarNotAvailable
    }
    catch (const TaskbarNotAvailable&) {}

    //hide until end of process:
    pnl_.m_notebookResult     ->Hide();
    pnl_.m_buttonClose        ->Show(false);
    //set std order after button visibility was set
    setStandardButtonLayout(*pnl_.bSizerStdButtons, StdButtons().setAffirmative(pnl_.m_buttonPause).setCancel(pnl_.m_buttonStop));

    pnl_.m_bpButtonMinimizeToTray->SetBitmapLabel(loadImage("minimize_to_tray"));

    const wxImage& imgFile = IconBuffer::genericFileIcon(IconBuffer::SIZE_SMALL);
    pnl_.m_bitmapItemStat->SetBitmap(imgFile);

    const wxImage imgTime = loadImage("time", -1 /*maxWidth*/, imgFile.GetHeight());
    pnl_.m_bitmapTimeStat->SetBitmap(imgTime);
    pnl_.m_bitmapTimeStat->SetMinSize({-1, imgFile.GetHeight()});

    pnl_.m_bitmapIgnoreErrors->SetBitmap(loadImage("error_ignore_active"));
    pnl_.m_bitmapRetryErrors ->SetBitmap(loadImage("error_retry"));

    //init graph
    const int xLabelHeight = this->GetCharHeight() + fastFromDIP(2) /*margin*/; //use same height for both graphs to make sure they stretch evenly
    const int yLabelWidth  = fastFromDIP(70);
    pnl_.m_panelGraphBytes->setAttributes(Graph2D::MainAttributes().
                                          setLabelX(XLabelPos::top,   xLabelHeight, std::make_shared<LabelFormatterTimeElapsed>(true)).
                                          setLabelY(YLabelPos::right, yLabelWidth,  std::make_shared<LabelFormatterBytes>()).
                                          setBaseColors(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT), wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)).
                                          setSelectionMode(GraphSelMode::none));

    pnl_.m_panelGraphItems->setAttributes(Graph2D::MainAttributes().
                                          setLabelX(XLabelPos::bottom, xLabelHeight, std::make_shared<LabelFormatterTimeElapsed>(true)).
                                          setLabelY(YLabelPos::right,  yLabelWidth,  std::make_shared<LabelFormatterItemCount>()).
                                          setBaseColors(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT), wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)).
                                          setSelectionMode(GraphSelMode::none));

    pnl_.m_panelGraphBytes->addCurve(curveBytes_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).fillCurveArea(getColorBytes()).setColor(getColorBytesRim()));
    pnl_.m_panelGraphItems->addCurve(curveItems_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).fillCurveArea(getColorItems()).setColor(getColorItemsRim()));

    pnl_.m_panelGraphBytes->addCurve(curveBytesEstim_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(1)).fillCurveArea(getColorLightGrey()).setColor(getColorDarkGrey()));
    pnl_.m_panelGraphItems->addCurve(curveItemsEstim_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(1)).fillCurveArea(getColorLightGrey()).setColor(getColorDarkGrey()));

    pnl_.m_panelGraphBytes->addCurve(curveBytesTimeNow_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).setColor(getColorBytesDark()));
    pnl_.m_panelGraphItems->addCurve(curveItemsTimeNow_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).setColor(getColorItemsDark()));

    pnl_.m_panelGraphBytes->addCurve(curveBytesTimeEstim_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).setColor(getColorBytesDark()));
    pnl_.m_panelGraphItems->addCurve(curveItemsTimeEstim_, Graph2D::CurveAttributes().setLineWidth(fastFromDIP(2)).setColor(getColorItemsDark()));

    //graph legend:
    auto generateSquareBitmap = [&](const wxColor& fillCol, const wxColor& borderCol)
    {
        wxBitmap bmpSquare(this->GetCharHeight(), this->GetCharHeight()); //seems we don't need to pass 24-bit depth here even for high-contrast color schemes
        {
            wxMemoryDC dc(bmpSquare);
            drawFilledRectangle(dc, wxRect(bmpSquare.GetSize()), fastFromDIP(1), borderCol, fillCol);
        }
        return bmpSquare;
    };
    pnl_.m_bitmapGraphKeyBytes->SetBitmap(generateSquareBitmap(getColorBytes(), getColorBytesRim()));
    pnl_.m_bitmapGraphKeyItems->SetBitmap(generateSquareBitmap(getColorItems(), getColorItemsRim()));


    pnl_.bSizerDynSpace->SetMinSize(yLabelWidth, -1); //ensure item/time stats are nicely centered

    setText(*pnl_.m_staticTextRetryCount, L'(' + formatNumber(autoRetryCount) + MULT_SIGN + L')');
    pnl_.bSizerErrorsRetry->Show(autoRetryCount > 0);

    //allow changing a few options dynamically during sync
    ignoreErrors_ = ignoreErrors;

    enumPostSyncAction_.add(PostSyncAction2::none, L"");
    if (parentFrame_) //enable EXIT option for gui mode sync
        enumPostSyncAction_.add(PostSyncAction2::exit, replaceCpy(_("E&xit"), L"&", L"")); //reuse translation
    enumPostSyncAction_.add(PostSyncAction2::sleep,    _("System: Sleep"));
    enumPostSyncAction_.add(PostSyncAction2::shutdown, _("System: Shut down"));

    setEnumVal(enumPostSyncAction_, *pnl_.m_choicePostSyncAction, postSyncAction);

    pnl_.m_checkBoxAutoClose->SetValue(autoCloseDialog);

    updateStaticGui(); //null-status will be shown while waiting for dir locks

    //make sure that standard height matches ProcessPhase::comparingContent statistics layout (== largest)

    this->GetSizer()->SetSizeHints(this); //~=Fit() + SetMinSize()
    pnl_.Layout();
    this->Center(); //call *after* dialog layout update and *before* wxWindow::Show()!

    setInitialWindowSize(*this, dlgSizeBuf_, std::nullopt/*pos*/, dlgMaximize, this->GetSize() /*defaultSize*/);

    if (showProgress)
    {
        this->Show();
        //clear gui flicker, remove dummy texts: window must be visible to make this work!
        updateProgressGui(true /*allowYield*/); //at least on OS X a real Yield() is required to flush pending GUI updates; Update() is not enough

        pnl_.m_buttonStop->SetFocus(); //don't steal focus when starting in sys-tray!
    }
    else
        minimizeToTray();
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onLocalKeyEvent(wxKeyEvent& event)
{
    switch (event.GetKeyCode())
    {
        case WXK_ESCAPE:
        {
            wxButton& activeButton = pnl_.m_buttonStop->IsShown() ? *pnl_.m_buttonStop : *pnl_.m_buttonClose;

            wxCommandEvent dummy(wxEVT_COMMAND_BUTTON_CLICKED);
            activeButton.Command(dummy); //simulate click
            return;
        }
        break;
    }

    event.Skip();
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onParentKeyEvent(wxKeyEvent& event)
{
    //redirect keys from main dialog to progress dialog
    switch (event.GetKeyCode())
    {
        case WXK_ESCAPE:
            this->SetFocus();
            this->onLocalKeyEvent(event); //event will be handled => no event recursion to parent dialog!
            return;
    }

    event.Skip();
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::initNewPhase()
{
    updateStaticGui(); //evaluates "syncStat_->currentPhase()"

    //reset graphs (e.g. after binary comparison)
    curveBytes_         ->clear();
    curveItems_         ->clear();
    curveBytesEstim_    ->setValue(0, 0, 0, 0);
    curveItemsEstim_    ->setValue(0, 0, 0, 0);
    curveBytesTimeNow_  ->setValue(0, 0);
    curveItemsTimeNow_  ->setValue(0, 0);
    curveBytesTimeEstim_->setValue(0, 0);
    curveItemsTimeEstim_->setValue(0, 0);

    notifyProgressChange(); //make sure graphs get initial values

    //start new measurement
    perf_ = PerfCheck(WINDOW_REMAINING_TIME, WINDOW_BYTES_PER_SEC);
    timeLastGraphTotalUpdate_ = timeLastSpeedEstimate_ = std::chrono::seconds(-100); //make sure estimate is updated upon next check
    phaseStart_ = stopWatch_.elapsed();

    updateProgressGui(false /*allowYield*/);
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::notifyProgressChange() //noexcept!
{
    if (syncStat_) //sync running
    {
        const ProgressStats stats = syncStat_->getStatsCurrent();
        curveBytes_->addRecord(stopWatch_.elapsed(), stats.bytes);
        curveItems_->addRecord(stopWatch_.elapsed(), stats.items);
    }
}


namespace
{
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::setExternalStatus(const wxString& statusTxt, const wxString& progress) //progress may be empty!
{
    //sys tray: order "top-down": jobname, status, progress
    wxString tooltip = L"FreeFileSync";
    if (!jobName_.empty())
        tooltip += L" | " + jobName_;

    tooltip += L"\n" + statusTxt;

    if (!progress.empty())
        tooltip += L' ' + progress;

    //window caption/taskbar; inverse order: progress, status, jobname
    wxString title;
    if (!progress.empty())
        title += progress + L" | ";

    title += statusTxt;

    if (!jobName_.empty())
        title += L" | " + jobName_;

    const TimeComp tc = getLocalTime(std::chrono::system_clock::to_time_t(syncStartTime_)); //returns empty string on failure
    title += L" | " + utfTo<std::wstring>(formatTime(formatDateTimeTag, tc));
    //---------------------------------------------------------------------------

    //systray tooltip, if window is minimized
    if (trayIcon_)
        trayIcon_->setToolTip(tooltip);

    //show text in dialog title (and at the same time in taskbar)
    if (parentFrame_)
        if (parentFrame_->GetTitle() != title)
            parentFrame_->SetTitle(title);

    //always set a title: we don't want wxGTK to show "nameless window" instead
    if (this->GetTitle() != title)
        this->SetTitle(title);
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::updateProgressGui(bool allowYield)
{
    assert(syncStat_);
    if (!syncStat_) //sync not running
        return;

    //normally we don't update the "static" GUI components here, but we have to make an exception
    //if sync is cancelled (by user or error handling option)
    if (syncStat_->getAbortStatus())
        updateStaticGui(); //called more than once after cancel... ok


    bool layoutChanged = false; //avoid screen flicker by calling layout() only if necessary
    const std::chrono::nanoseconds timeElapsed = stopWatch_.elapsed();
    const double timeElapsedDouble = std::chrono::duration<double>(timeElapsed).count();

    const int     itemsCurrent = syncStat_->getStatsCurrent().items;
    const int64_t bytesCurrent = syncStat_->getStatsCurrent().bytes;
    const int     itemsTotal   = syncStat_->getStatsTotal  ().items;
    const int64_t bytesTotal   = syncStat_->getStatsTotal  ().bytes;

    const bool haveTotalStats = itemsTotal >= 0 || bytesTotal >= 0;

    //status texts
    setText(*pnl_.m_staticTextStatus, replaceCpy(syncStat_->currentStatusText(), L'\n', L' ')); //no layout update for status texts!

    if (!haveTotalStats)
    {
        //dialog caption, taskbar, systray tooltip
        setExternalStatus(getDialogPhaseText(*syncStat_, paused_), formatNumber(itemsCurrent)); //status text may be "paused"!

        //progress indicators
        if (trayIcon_.get()) trayIcon_->setProgress(1); //100% = regular FFS logo
        //taskbar_ already set to STATUS_INDETERMINATE within initNewPhase()
    }
    else
    {
        //dialog caption, taskbar, systray tooltip

        const double fractionTotal = bytesTotal + itemsTotal == 0 ? 0 : 1.0 * (bytesCurrent + itemsCurrent) / (bytesTotal + itemsTotal);
        //add both data + obj-count, to handle "deletion-only" cases

        setExternalStatus(getDialogPhaseText(*syncStat_, paused_), formatFraction(fractionTotal)); //status text may be "paused"!

        //progress indicators
        if (trayIcon_.get()) trayIcon_->setProgress(fractionTotal);
        if (taskbar_ .get()) taskbar_ ->setProgress(fractionTotal);

        const double timeTotalSecTentative = bytesCurrent == bytesTotal ? timeElapsedDouble : std::max(curveBytesEstim_->getTotalTime(), timeElapsedDouble);

        curveBytesEstim_->setValue(timeElapsedDouble, timeTotalSecTentative, bytesCurrent, bytesTotal);
        curveItemsEstim_->setValue(timeElapsedDouble, timeTotalSecTentative, itemsCurrent, itemsTotal);

        //tentatively update total time, may be improved on below:
        curveBytesTimeNow_  ->setValue(timeElapsedDouble, bytesCurrent);
        curveItemsTimeNow_  ->setValue(timeElapsedDouble, itemsCurrent);

        curveBytesTimeEstim_->setValue(timeTotalSecTentative, bytesTotal);
        curveItemsTimeEstim_->setValue(timeTotalSecTentative, itemsTotal);
    }

    //even though notifyProgressChange() already set the latest data, let's add another sample to have all curves consider "timeNowMs"
    //no problem with adding too many records: CurveDataStatistics will remove duplicate entries!
    curveBytes_->addRecord(timeElapsed, bytesCurrent);
    curveItems_->addRecord(timeElapsed, itemsCurrent);

    //item and data stats
    if (!haveTotalStats)
    {
        setText(*pnl_.m_staticTextItemsProcessed, formatNumber(itemsCurrent), &layoutChanged);
        setText(*pnl_.m_staticTextBytesProcessed, L"", &layoutChanged);

        setText(*pnl_.m_staticTextItemsRemaining, std::wstring(1, EM_DASH), &layoutChanged);
        setText(*pnl_.m_staticTextBytesRemaining, L"",  &layoutChanged);
    }
    else
    {
        setText(*pnl_.m_staticTextItemsProcessed,               formatNumber(itemsCurrent), &layoutChanged);
        setText(*pnl_.m_staticTextBytesProcessed, L'(' + formatFilesizeShort(bytesCurrent) + L')', &layoutChanged);

        setText(*pnl_.m_staticTextItemsRemaining,               formatNumber(itemsTotal - itemsCurrent), &layoutChanged);
        setText(*pnl_.m_staticTextBytesRemaining, L'(' + formatFilesizeShort(bytesTotal - bytesCurrent) + L')', &layoutChanged);
        //it's possible data remaining becomes shortly negative if last file synced has ADS data and the bytesTotal was not yet corrected!
    }

    //current time elapsed
    const int64_t timeElapSec = std::chrono::duration_cast<std::chrono::seconds>(timeElapsed).count();

    setText(*pnl_.m_staticTextTimeElapsed, timeElapSec < 3600 ?
            wxTimeSpan::Seconds(timeElapSec).Format(   L"%M:%S") :
            wxTimeSpan::Seconds(timeElapSec).Format(L"%H:%M:%S"), &layoutChanged);

    //remaining time and speed
    if (numeric::dist(timeLastSpeedEstimate_, timeElapsed) >= SPEED_ESTIMATE_UPDATE_INTERVAL)
    {
        timeLastSpeedEstimate_ = timeElapsed;

        if (numeric::dist(phaseStart_, timeElapsed) >= SPEED_ESTIMATE_SAMPLE_SKIP) //discard stats for first second: probably messy
            perf_.addSample(timeElapsed, itemsCurrent, bytesCurrent);

        //current speed -> Win 7 copy uses 1 sec update interval instead
        std::optional<std::wstring> bps = perf_.getBytesPerSecond();
        std::optional<std::wstring> ips = perf_.getItemsPerSecond();
        pnl_.m_panelGraphBytes->setAttributes(pnl_.m_panelGraphBytes->getAttributes().setCornerText(bps ? *bps : L"", GraphCorner::topL));
        pnl_.m_panelGraphItems->setAttributes(pnl_.m_panelGraphItems->getAttributes().setCornerText(ips ? *ips : L"", GraphCorner::topL));

        //remaining time
        if (!haveTotalStats)
        {
            setText(*pnl_.m_staticTextTimeRemaining, std::wstring(1, EM_DASH), &layoutChanged);
            //ignore graphs: should already have been cleared in initNewPhase()
        }
        else
        {
            //remaining time: display with relative error of 10% - based on samples taken every 0.5 sec only
            //-> call more often than once per second to correctly show last few seconds countdown, but don't call too often to avoid occasional jitter
            std::optional<double> remTimeSec = perf_.getRemainingTimeSec(bytesTotal - bytesCurrent);
            setText(*pnl_.m_staticTextTimeRemaining, remTimeSec ? formatRemainingTime(*remTimeSec) : std::wstring(1, EM_DASH), &layoutChanged);

            const double timeRemainingSec = remTimeSec ? *remTimeSec : 0;
            const double timeTotalSec = timeElapsedDouble + timeRemainingSec;
            //update estimated total time marker only with precision of "15% remaining time" to avoid needless jumping around:
            if (numeric::dist(curveBytesEstim_->getTotalTime(), timeTotalSec) > 0.15 * timeRemainingSec)
            {
                //avoid needless flicker and don't update total time graph too often:
                static_assert(std::chrono::duration_cast<std::chrono::milliseconds>(GRAPH_TOTAL_TIME_UPDATE_INTERVAL).count() % SPEED_ESTIMATE_UPDATE_INTERVAL.count() == 0);
                if (numeric::dist(timeLastGraphTotalUpdate_, timeElapsed) >= GRAPH_TOTAL_TIME_UPDATE_INTERVAL)
                {
                    timeLastGraphTotalUpdate_ = timeElapsed;

                    curveBytesEstim_->setTotalTime(timeTotalSec);
                    curveItemsEstim_->setTotalTime(timeTotalSec);

                    curveBytesTimeEstim_->setTime(timeTotalSec);
                    curveItemsTimeEstim_->setTime(timeTotalSec);
                }
            }
        }
    }

    pnl_.m_panelGraphBytes->Refresh();
    pnl_.m_panelGraphItems->Refresh();

    //adapt layout after content changes above
    if (layoutChanged)
    {
        pnl_.m_panelProgress->Layout();
        //small statistics panels:
        pnl_.m_panelItemStats->Layout();
        pnl_.m_panelTimeStats->Layout();
    }


    if (allowYield)
    {
        if (paused_) //support for pause button
        {
            PauseTimers dummy(*this);

            while (paused_)
            {
                wxTheApp->Yield(); //receive UI message that ends pause
                //*first* refresh GUI (removing flicker) before sleeping!
                std::this_thread::sleep_for(UI_UPDATE_INTERVAL);
            }
        }
        else
            /*
                /|\
                 |   keep this sequence to ensure one full progress update before entering pause mode!
                \|/
            */
            wxTheApp->Yield(); //receive UI message that sets pause status OR forceful termination!
    }
    else
        this->Update(); //don't wait until next idle event (who knows what blocking process comes next?)
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::updateStaticGui() //depends on "syncStat_, paused_"
{
    assert(syncStat_);
    if (!syncStat_)
        return;

    pnl_.m_staticTextPhase->SetLabel(getDialogPhaseText(*syncStat_, paused_));
    //pnl_.m_bitmapStatus->SetToolTip(); -> redundant

    const wxImage statusImage = [&]
    {
        if (paused_)
            return loadImage("status_pause");

        if (syncStat_->getAbortStatus())
            return loadImage("result_error");

        switch (syncStat_->currentPhase())
        {
            case ProcessPhase::none:
            case ProcessPhase::scanning:
                return loadImage("status_scanning");
            case ProcessPhase::comparingContent:
                return loadImage("status_binary_compare");
            case ProcessPhase::synchronizing:
                return loadImage("status_syncing");
        }
        assert(false);
        return wxNullImage;
    }();
    pnl_.m_bitmapStatus->SetBitmap(statusImage);

    //show status on Windows 7 taskbar
    if (taskbar_.get())
    {
        if (paused_)
            taskbar_->setStatus(Taskbar::STATUS_PAUSED);
        else
        {
            const int     itemsTotal = syncStat_->getStatsTotal().items;
            const int64_t bytesTotal = syncStat_->getStatsTotal().bytes;

            const bool haveTotalStats = itemsTotal >= 0 || bytesTotal >= 0;

            taskbar_->setStatus(haveTotalStats ? Taskbar::STATUS_NORMAL : Taskbar::STATUS_INDETERMINATE);
        }
    }

    //pause button
    pnl_.m_buttonPause->SetLabel(paused_ ? _("&Continue") : _("&Pause"));

    pnl_.bSizerErrorsIgnore->Show(ignoreErrors_);

    pnl_.Layout();
    pnl_.m_panelProgress->Layout(); //for bSizerErrorsIgnore
    //this->Refresh(); //a few pixels below the status text need refreshing -> still needed?
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::showSummary(SyncResult syncResult, const SharedRef<const ErrorLog>& log)
{
    assert(syncStat_);
    //at the LATEST(!) to prevent access to currentStatusHandler
    //enable okay and close events; may be set in this method ONLY

    paused_ = false; //you never know?

    //update numbers one last time (as if sync were still running)
    notifyProgressChange(); //make one last graph entry at the *current* time
    updateProgressGui(false /*allowYield*/);
    //===================================================================================

    const int     itemsProcessed = syncStat_->getStatsCurrent().items;
    const int64_t bytesProcessed = syncStat_->getStatsCurrent().bytes;
    const int     itemsTotal     = syncStat_->getStatsTotal  ().items;
    const int64_t bytesTotal     = syncStat_->getStatsTotal  ().bytes;

    //set overall speed (instead of current speed)
    const double timeDelta = std::chrono::duration<double>(stopWatch_.elapsed() - phaseStart_).count();
    //we need to consider "time within current phase" not total "timeElapsed"!

    const wxString overallBytesPerSecond = numeric::isNull(timeDelta) ? std::wstring() :
                                           replaceCpy(_("%x/sec"), L"%x", formatFilesizeShort(numeric::round(bytesProcessed / timeDelta)));
    const wxString overallItemsPerSecond = numeric::isNull(timeDelta) ? std::wstring() :
                                           replaceCpy(_("%x/sec"), L"%x", replaceCpy(_("%x items"), L"%x", formatThreeDigitPrecision(itemsProcessed / timeDelta)));

    pnl_.m_panelGraphBytes->setAttributes(pnl_.m_panelGraphBytes->getAttributes().setCornerText(overallBytesPerSecond, GraphCorner::topL));
    pnl_.m_panelGraphItems->setAttributes(pnl_.m_panelGraphItems->getAttributes().setCornerText(overallItemsPerSecond, GraphCorner::topL));

    //...if everything was processed successfully
    if (itemsTotal >= 0 && bytesTotal >= 0 && //itemsTotal < 0 && bytesTotal < 0 => e.g. cancel during folder comparison
        itemsProcessed == itemsTotal &&
        bytesProcessed == bytesTotal)
    {
        pnl_.m_staticTextProcessed     ->Hide();
        pnl_.m_staticTextRemaining     ->Hide();
        pnl_.m_staticTextItemsRemaining->Hide();
        pnl_.m_staticTextBytesRemaining->Hide();
        pnl_.m_staticTextTimeRemaining ->Hide();
    }

    //generally not interesting anymore (e.g. items > 0 dur to skipped errors)
    pnl_.m_staticTextTimeRemaining->Hide();

    const int64_t totalTimeSec = std::chrono::duration_cast<std::chrono::seconds>(stopWatch_.elapsed()).count();
    setText(*pnl_.m_staticTextTimeElapsed, wxTimeSpan::Seconds(totalTimeSec).Format(L"%H:%M:%S"));
    //totalTimeSec < 3600 ? wxTimeSpan::Seconds(totalTimeSec).Format(L"%M:%S") -> let's use full precision for max. clarity: https://freefilesync.org/forum/viewtopic.php?t=6308
    //maybe also should rename to "Total time"!?

    resumeFromSystray(false /*userRequested*/); //if in tray mode...

    //------- change class state -------
    syncStat_ = nullptr;
    //----------------------------------

    const wxImage statusImage = [&]
    {
        switch (syncResult)
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
    pnl_.m_bitmapStatus->SetBitmap(statusImage);

    pnl_.m_staticTextPhase->SetLabel(getSyncResultLabel(syncResult));
    //pnl_.m_bitmapStatus->SetToolTip(); -> redundant

    //show status on Windows 7 taskbar
    if (taskbar_.get())
        switch (syncResult)
        {
            case SyncResult::finishedSuccess:
                taskbar_->setStatus(Taskbar::STATUS_NORMAL);
                break;

            case SyncResult::finishedWarning:
                taskbar_->setStatus(Taskbar::STATUS_WARNING);
                break;

            case SyncResult::finishedError:
            case SyncResult::aborted:
                taskbar_->setStatus(Taskbar::STATUS_ERROR);
                break;
        }
    //----------------------------------

    setExternalStatus(getSyncResultLabel(syncResult), wxString());

    //this->EnableCloseButton(true);

    pnl_.m_bpButtonMinimizeToTray->Hide();
    pnl_.m_buttonStop->Disable();
    pnl_.m_buttonStop->Hide();
    pnl_.m_buttonPause->Disable();
    pnl_.m_buttonPause->Hide();
    pnl_.m_buttonClose->Show();
    pnl_.m_buttonClose->Enable();

    pnl_.bSizerProgressFooter->Show(false);

    if (!parentFrame_) //hide checkbox for batch mode sync (where value won't be retrieved after close)
        pnl_.m_checkBoxAutoClose->Hide();

    //set std order after button visibility was set
    setStandardButtonLayout(*pnl_.bSizerStdButtons, StdButtons().setAffirmative(pnl_.m_buttonClose));

    //hide current operation status
    pnl_.bSizerStatusText->Show(false);

    pnl_.m_staticlineFooter->Hide(); //win: m_notebookResult already has a window frame

    //-------------------------------------------------------------

    pnl_.m_notebookResult->SetPadding(wxSize(fastFromDIP(2), 0)); //height cannot be changed

    //1. re-arrange graph into results listbook
    const size_t pagePosProgress = 0;
    const size_t pagePosLog      = 1;

    [[maybe_unused]] const bool wasDetached = pnl_.bSizerRoot->Detach(pnl_.m_panelProgress);
    assert(wasDetached);
    pnl_.m_panelProgress->Reparent(pnl_.m_notebookResult);
    pnl_.m_notebookResult->AddPage(pnl_.m_panelProgress, _("Progress"), true /*bSelect*/);

    //2. log file
    assert(pnl_.m_notebookResult->GetPageCount() == 1);
    LogPanel* logPanel = new LogPanel(pnl_.m_notebookResult); //owned by m_notebookResult
    logPanel->setLog(log.ptr());
    pnl_.m_notebookResult->AddPage(logPanel, _("Log"), false /*bSelect*/);

    //show log instead of graph if errors occurred! (not required for ignored warnings)
    const ErrorLog::Stats logCount = log.ref().getStats();
    if (logCount.error > 0)
        pnl_.m_notebookResult->ChangeSelection(pagePosLog);

    //fill image list to cope with wxNotebook image setting design desaster...
    const int imgListSize = loadImage("log_file_sicon").GetHeight();
    auto imgList = std::make_unique<wxImageList>(imgListSize, imgListSize);

    auto addToImageList = [&](const wxImage& img)
    {
        assert(img.GetWidth () <= imgListSize);
        assert(img.GetHeight() <= imgListSize);
        imgList->Add(img);
    };
    addToImageList(loadImage("progress_sicon"));
    addToImageList(loadImage("log_file_sicon"));

    pnl_.m_notebookResult->AssignImageList(imgList.release()); //pass ownership

    pnl_.m_notebookResult->SetPageImage(pagePosProgress, pagePosProgress);
    pnl_.m_notebookResult->SetPageImage(pagePosLog,      pagePosLog);

    //Caveat: we need "Show()" *after" the above wxNotebook::ChangeSelection() to get the correct selection on Linux
    pnl_.m_notebookResult->Show();

    //GetSizer()->SetSizeHints(this); //~=Fit() //not a good idea: will shrink even if window is maximized or was enlarged by the user
    pnl_.Layout();

    pnl_.m_panelProgress->Layout();
    //small statistics panels:
    pnl_.m_panelItemStats->Layout();
    pnl_.m_panelTimeStats->Layout();

    //this->Raise(); -> don't! user may be watching a movie in the meantime ;)

    pnl_.m_buttonClose->SetFocus();
}


template <class TopLevelDialog>
auto SyncProgressDialogImpl<TopLevelDialog>::destroy(bool autoClose, bool restoreParentFrame, SyncResult syncResult, const SharedRef<const ErrorLog>& log) -> Result
{
    if (autoClose)
    {
        assert(syncStat_);

        //ATTENTION: dialog may live a little longer, so watch callbacks!
        //e.g. wxGTK calls onIconize after wxWindow::Close() (better not ask why) and before physical destruction! => indirectly calls updateStaticGui(), which reads syncStat_!!!
        syncStat_ = nullptr;
    }
    else
    {
        showSummary(syncResult, log);

        //wait until user closes the dialog by pressing "Close"
        while (!closePressed_)
        {
            wxTheApp->Yield(); //refresh GUI *first* before sleeping! (remove flicker)
            std::this_thread::sleep_for(UI_UPDATE_INTERVAL);
        }
        restoreParentFrame = true;
    }
    //------------------------------------------------------------------------

    if (parentFrame_)
    {
        [[maybe_unused]] bool ubOk = parentFrame_->Unbind(wxEVT_CHAR_HOOK, &SyncProgressDialogImpl::onParentKeyEvent, this);
        assert(ubOk);

        parentFrame_->SetTitle(parentTitleBackup_); //restore title text

        if (restoreParentFrame)
        {
            //make sure main dialog is shown again if still "minimized to systray"!
            parentFrame_->Show();
            //if (parentFrame_->IsIconized()) //caveat: if window is maximized calling Iconize(false) will erroneously un-maximize!
            //    parentFrame_->Iconize(false);
        }
    }
    //else: don't call transformAppType(): consider "switch to main dialog" option during silent batch run

    //------------------------------------------------------------------------
    const bool autoCloseDialog = getOptionAutoCloseDialog();

    const auto& [size, pos, isMaximized] = getWindowSizeBeforeClose(*this);
    if (size)
        dlgSizeBuf_ = *size;

    this->Destroy(); //wxWidgets macOS: simple "delete"!!!!!!!

    return { autoCloseDialog, dlgSizeBuf_, isMaximized };
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onClose(wxCloseEvent& event)
{
    assert(event.CanVeto()); //this better be true: if "this" is parent of a modal error dialog, there is NO way (in hell) we allow destruction here!!!
    //note: close cannot be prevented on Windows during system shutdown! => already handled via Application::onQueryEndSession()
    event.Veto();

    closePressed_ = true; //"temporary" auto-close: preempt closing results dialog

    if (syncStat_)
    {
        //user closing dialog => cancel sync + auto-close dialog
        userRequestAbort_();

        paused_ = false; //[!] we could be pausing here!
        updateStaticGui(); //update status + pause button
    }
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onCancel(wxCommandEvent& event)
{
    userRequestAbort_();

    paused_ = false;
    updateStaticGui(); //update status + pause button
    //no UI-update here to avoid cascaded Yield()-call!
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onPause(wxCommandEvent& event)
{
    paused_ = !paused_;
    updateStaticGui(); //update status + pause button
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::onIconize(wxIconizeEvent& event)
{
    /*
        propagate progress dialog minimize/maximize to parent
        -----------------------------------------------------
        Fedora/Debian/Ubuntu:
            - wxDialog cannot be minimized
            - worse, wxGTK sends stray iconize events *after* wxDialog::Destroy()
            - worse, on Fedora an iconize event is issued directly after calling Close()
            - worse, even wxDialog::Hide() causes iconize event!
                => nothing to do
        SUSE:
            - wxDialog can be minimized (it just vanishes!) and in general also minimizes parent: except for our progress wxDialog!!!
            - worse, wxDialog::Hide() causes iconize event
            - probably the same issues with stray iconize events like Fedora/Debian/Ubuntu
            - minimize button is always shown, even if wxMINIMIZE_BOX is omitted!
                => nothing to do
        Mac OS X:
            - wxDialog can be minimized and automatically minimizes parent
            - no iconize events seen by wxWidgets!
                => nothing to do
        Windows:
            - wxDialog can be minimized but does not also minimize parent
            - iconize events only seen for manual minimize
                => propagate event to parent
    */
    event.Skip();
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::minimizeToTray()
{
    if (!trayIcon_.get())
    {
        trayIcon_ = std::make_unique<FfsTrayIcon>([this] { this->resumeFromSystray(true /*userRequested*/); }); //FfsTrayIcon lifetime is a subset of "this"'s lifetime!
        //we may destroy FfsTrayIcon even while in the FfsTrayIcon callback!!!!

        updateProgressGui(false /*allowYield*/); //set tray tooltip + progress: e.g. no updates while paused

        this->Hide();
        if (parentFrame_)
            parentFrame_->Hide();
    }
}


template <class TopLevelDialog>
void SyncProgressDialogImpl<TopLevelDialog>::resumeFromSystray(bool userRequested)
{
    if (trayIcon_)
    {
        trayIcon_.reset();

        if (parentFrame_)
        {
            //if (parentFrame_->IsIconized()) //caveat: if window is maximized calling Iconize(false) will erroneously un-maximize!
            //    parentFrame_->Iconize(false);
            parentFrame_->Show();
        }

        //if (IsIconized()) //caveat: if window is maximized calling Iconize(false) will erroneously un-maximize!
        //    Iconize(false);
        this->Show();

        updateStaticGui();                        //restore Windows 7 task bar status   (e.g. required in pause mode)
        updateProgressGui(false  /*allowYield*/); //restore Windows 7 task bar progress (e.g. required in pause mode)


        if (userRequested)
        {
            if (parentFrame_)
                parentFrame_->Raise();
            this->Raise();
            pnl_.m_bpButtonMinimizeToTray->SetFocus();
        }
    }
}

//########################################################################################

SyncProgressDialog* SyncProgressDialog::create(wxSize dlgSize, bool dlgMaximize,
                                               const std::function<void()>& userRequestAbort,
                                               const Statistics& syncStat,
                                               wxFrame* parentWindow, //may be nullptr
                                               bool showProgress,
                                               bool autoCloseDialog,
                                               const std::vector<std::wstring>& jobNames,
                                               const std::chrono::system_clock::time_point& syncStartTime,
                                               bool ignoreErrors,
                                               size_t autoRetryCount,
                                               PostSyncAction2 postSyncAction)
{
    if (parentWindow) //sync from GUI
    {
        //due to usual "wxBugs", wxDialog on OS X does not float on its parent; wxFrame OTOH does => hack!
        //https://groups.google.com/forum/#!topic/wx-users/J5SjjLaBOQE
        return new SyncProgressDialogImpl<wxDialog>(wxDEFAULT_DIALOG_STYLE | wxMAXIMIZE_BOX | wxMINIMIZE_BOX | wxRESIZE_BORDER,
                                                    dlgSize, dlgMaximize, userRequestAbort, syncStat, parentWindow, showProgress, autoCloseDialog, jobNames, syncStartTime, ignoreErrors, autoRetryCount, postSyncAction);
    }
    else //FFS batch job
    {
        auto dlg = new SyncProgressDialogImpl<wxFrame>(wxDEFAULT_FRAME_STYLE,
                                                       dlgSize, dlgMaximize, userRequestAbort, syncStat, parentWindow, showProgress, autoCloseDialog, jobNames, syncStartTime, ignoreErrors, autoRetryCount, postSyncAction);
        dlg->SetIcon(getFfsIcon()); //only top level windows should have an icon
        return dlg;
    }
}
