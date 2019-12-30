// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "triple_splitter.h"
#include <algorithm>
#include <zen/stl_tools.h>
#include <wx+/dc.h>

using namespace zen;
using namespace fff;


namespace
{
//------------ Grid Constants -------------------------------
const int SASH_HIT_TOLERANCE_DIP = 5; //currently only a placebo!
const int SASH_SIZE_DIP          = 10;
const int SASH_GRADIENT_SIZE_DIP = 3;

const double SASH_GRAVITY = 0.5; //value within [0, 1]; 1 := resize left only, 0 := resize right only
const int CHILD_WINDOW_MIN_SIZE_DIP = 50; //min. size of managed windows

//let's NOT create wxWidgets objects statically:
inline wxColor getColorSashGradientFrom() { return { 192, 192, 192 }; } //light grey
inline wxColor getColorSashGradientTo  () { return *wxWHITE; }
}


TripleSplitter::TripleSplitter(wxWindow* parent,
                               wxWindowID id,
                               const wxPoint& pos,
                               const wxSize& size,
                               long style) : wxWindow(parent, id, pos, size, style | wxTAB_TRAVERSAL), //tab between windows
    sashSize_          (fastFromDIP(SASH_SIZE_DIP)),
    childWindowMinSize_(fastFromDIP(CHILD_WINDOW_MIN_SIZE_DIP))
{
    Connect(wxEVT_PAINT, wxPaintEventHandler(TripleSplitter::onPaintEvent), nullptr, this);
    Connect(wxEVT_SIZE,  wxSizeEventHandler (TripleSplitter::onSizeEvent ), nullptr, this);
    Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent& event) {}); //http://wiki.wxwidgets.org/Flicker-Free_Drawing

    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Connect(wxEVT_LEFT_DOWN,    wxMouseEventHandler(TripleSplitter::onMouseLeftDown  ), nullptr, this);
    Connect(wxEVT_LEFT_UP,      wxMouseEventHandler(TripleSplitter::onMouseLeftUp    ), nullptr, this);
    Connect(wxEVT_MOTION,       wxMouseEventHandler(TripleSplitter::onMouseMovement  ), nullptr, this);
    Connect(wxEVT_LEAVE_WINDOW, wxMouseEventHandler(TripleSplitter::onLeaveWindow    ), nullptr, this);
    Connect(wxEVT_LEFT_DCLICK,  wxMouseEventHandler(TripleSplitter::onMouseLeftDouble), nullptr, this);
    Connect(wxEVT_MOUSE_CAPTURE_LOST, wxMouseCaptureLostEventHandler(TripleSplitter::onMouseCaptureLost), nullptr, this);
}


TripleSplitter::~TripleSplitter() {} //make sure correct destructor gets created for std::unique_ptr<SashMove>


void TripleSplitter::updateWindowSizes()
{
    if (windowL_ && windowC_ && windowR_)
    {
        const int centerPosX  = getCenterPosX();
        const int centerWidth = getCenterWidth();

        const wxRect clientRect = GetClientRect();

        const int widthL = centerPosX;
        const int windowRposX = widthL + centerWidth;
        const int widthR = clientRect.width - windowRposX;

        windowL_->SetSize(0,                  0, widthL,                         clientRect.height);
        windowC_->SetSize(widthL + sashSize_, 0, windowC_->GetSize().GetWidth(), clientRect.height);
        windowR_->SetSize(windowRposX,        0, widthR,                         clientRect.height);

        wxClientDC dc(this);
        drawSash(dc);
    }
}


class TripleSplitter::SashMove
{
public:
    SashMove(wxWindow& wnd, int mousePosX, int centerOffset) : wnd_(wnd), mousePosX_(mousePosX), centerOffset_(centerOffset)
    {
        wnd_.SetCursor(wxCURSOR_SIZEWE);
        wnd_.CaptureMouse();
    }
    ~SashMove()
    {
        wnd_.SetCursor(*wxSTANDARD_CURSOR);
        if (wnd_.HasCapture())
            wnd_.ReleaseMouse();
    }
    int getMousePosXStart   () const { return mousePosX_; }
    int getCenterOffsetStart() const { return centerOffset_; }

private:
    wxWindow& wnd_;
    const int mousePosX_;
    const int centerOffset_;
};


inline
int TripleSplitter::getCenterWidth() const
{
    return 2 * sashSize_ + (windowC_ ? windowC_->GetSize().GetWidth() : 0);
}


int TripleSplitter::getCenterPosXOptimal() const
{
    const wxRect clientRect = GetClientRect();
    const int centerWidth = getCenterWidth();
    return (clientRect.width - centerWidth) * SASH_GRAVITY; //allowed to be negative for extreme client widths!
}


int TripleSplitter::getCenterPosX() const
{
    const wxRect clientRect = GetClientRect();
    const int centerWidth = getCenterWidth();
    const int centerPosXOptimal = getCenterPosXOptimal();

    //normalize "centerPosXOptimal + centerOffset"
    if (clientRect.width <  2 * childWindowMinSize_ + centerWidth)
        //use fixed "centeroffset" when "clientRect.width == 2 * childWindowMinSize_ + centerWidth"
        return centerPosXOptimal + childWindowMinSize_ - static_cast<int>(2 * childWindowMinSize_ * SASH_GRAVITY); //avoid rounding error
    //make sure transition between conditional branches is continuous!
    return std::max(childWindowMinSize_, //make sure centerPosXOptimal + offset is within bounds
                    std::min(centerPosXOptimal + centerOffset_, clientRect.width - childWindowMinSize_ - centerWidth));
}


void TripleSplitter::drawSash(wxDC& dc)
{
    const int centerPosX  = getCenterPosX();
    const int centerWidth = getCenterWidth();

    auto draw = [&](wxRect rect)
    {
        rect.width = fastFromDIP(SASH_GRADIENT_SIZE_DIP);
        dc.GradientFillLinear(rect, getColorSashGradientFrom(), getColorSashGradientTo(), wxEAST);

        rect.x += rect.width;
        rect.width = sashSize_ - fastFromDIP(SASH_GRADIENT_SIZE_DIP);
        dc.GradientFillLinear(rect, getColorSashGradientFrom(), getColorSashGradientTo(), wxWEST);
    };

    const wxRect rectSashL(centerPosX,                           0, sashSize_, GetClientRect().height);
    const wxRect rectSashR(centerPosX + centerWidth - sashSize_, 0, sashSize_, GetClientRect().height);

    draw(rectSashL);
    draw(rectSashR);
}


bool TripleSplitter::hitOnSashLine(int posX) const
{
    const int centerPosX  = getCenterPosX();
    const int centerWidth = getCenterWidth();

    //we don't get events outside of sash, so SASH_HIT_TOLERANCE_DIP is currently *useless*
    auto hitSash = [&](int sashX) { return sashX - fastFromDIP(SASH_HIT_TOLERANCE_DIP) <= posX && posX < sashX + sashSize_ + fastFromDIP(SASH_HIT_TOLERANCE_DIP); };

    return hitSash(centerPosX) || hitSash(centerPosX + centerWidth - sashSize_); //hit one of the two sash lines
}


void TripleSplitter::onMouseLeftDown(wxMouseEvent& event)
{
    activeMove_.reset();

    const int posX = event.GetPosition().x;
    if (hitOnSashLine(posX))
        activeMove_ = std::make_unique<SashMove>(*this, posX, centerOffset_);
    event.Skip();
}


void TripleSplitter::onMouseLeftUp(wxMouseEvent& event)
{
    activeMove_.reset(); //nothing else to do, actual work done by onMouseMovement()
    event.Skip();
}


void TripleSplitter::onMouseMovement(wxMouseEvent& event)
{
    if (activeMove_)
    {
        centerOffset_ = activeMove_->getCenterOffsetStart() + event.GetPosition().x - activeMove_->getMousePosXStart();

        //CAVEAT: function getCenterPosX() normalizes centerPosX *not* centerOffset!
        //This can lead to the strange effect of window not immediately resizing when centerOffset is extremely off limits
        //=> normalize centerOffset right here
        centerOffset_ = getCenterPosX() - getCenterPosXOptimal();

        updateWindowSizes();
        Update(); //no time to wait until idle event!
    }
    else
    {
        //we receive those only while above the sash, not the managed windows (except when the managed windows are disabled!)
        const int posX = event.GetPosition().x;
        if (hitOnSashLine(posX))
            SetCursor(wxCURSOR_SIZEWE); //set window-local only!
        else
            SetCursor(*wxSTANDARD_CURSOR);
    }
    event.Skip();
}


void TripleSplitter::onLeaveWindow(wxMouseEvent& event)
{
    //even called when moving from sash over to managed windows!
    if (!activeMove_)
        SetCursor(*wxSTANDARD_CURSOR);
    event.Skip();
}


void TripleSplitter::onMouseCaptureLost(wxMouseCaptureLostEvent& event)
{
    activeMove_.reset();
    updateWindowSizes();
    //event.Skip(); -> we DID handle it!
}


void TripleSplitter::onMouseLeftDouble(wxMouseEvent& event)
{
    const int posX = event.GetPosition().x;
    if (hitOnSashLine(posX))
    {
        centerOffset_ = 0; //reset sash according to gravity
        updateWindowSizes();
    }
    event.Skip();
}
