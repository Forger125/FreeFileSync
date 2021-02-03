// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef GRID_H_834702134831734869987
#define GRID_H_834702134831734869987

#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <vector>
//#include <zen/basic_math.h>
#include <wx/scrolwin.h>


//a user-friendly, extensible and high-performance grid control
namespace zen
{
enum class ColumnType { none = -1 }; //user-defiend column type
enum class HoverArea  { none = -1 }; //user-defined area for mouse selections for a given row (may span multiple columns or split a single column into multiple areas)

//------------------------ events ------------------------------------------------
//example: wnd.Bind(EVENT_GRID_COL_LABEL_LEFT_CLICK, [this](GridClickEvent& event) { onGridLeftClick(event); });

struct GridClickEvent;
struct GridSelectEvent;
struct GridLabelClickEvent;
struct GridColumnResizeEvent;
struct GridContextMenuEvent;

wxDECLARE_EVENT(EVENT_GRID_MOUSE_LEFT_DOUBLE, GridClickEvent);
wxDECLARE_EVENT(EVENT_GRID_MOUSE_LEFT_DOWN,   GridClickEvent);
wxDECLARE_EVENT(EVENT_GRID_MOUSE_RIGHT_DOWN,  GridClickEvent);

wxDECLARE_EVENT(EVENT_GRID_SELECT_RANGE, GridSelectEvent);
//NOTE: neither first nor second row need to match EVENT_GRID_MOUSE_LEFT_DOWN/EVENT_GRID_MOUSE_LEFT_UP: user holding SHIFT; moving out of window...

wxDECLARE_EVENT(EVENT_GRID_COL_LABEL_MOUSE_LEFT,  GridLabelClickEvent);
wxDECLARE_EVENT(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, GridLabelClickEvent);
wxDECLARE_EVENT(EVENT_GRID_COL_RESIZE, GridColumnResizeEvent);

//wxContextMenuEvent? => generated by wxWidgets when right mouse down/up is not handled; even OS-dependent in which case event is generated
//=> inappropriate! we know better when to show context!
wxDECLARE_EVENT(EVENT_GRID_CONTEXT_MENU, GridContextMenuEvent);


struct GridClickEvent : public wxEvent
{
    GridClickEvent(wxEventType et, ptrdiff_t row, HoverArea hoverArea, const wxPoint& mousePos) :
        wxEvent(0 /*winid*/, et), row_(row), hoverArea_(hoverArea), mousePos_(mousePos) {}
    GridClickEvent* Clone() const override { return new GridClickEvent(*this); }

    const ptrdiff_t row_; //-1 for invalid position, >= rowCount if out of range
    const HoverArea hoverArea_; //may be HoverArea::none
    const wxPoint mousePos_; //client coordinates
};

struct GridSelectEvent : public wxEvent
{
    GridSelectEvent(size_t rowFirst, size_t rowLast, bool positive, const GridClickEvent* mouseClick) :
        wxEvent(0 /*winid*/, EVENT_GRID_SELECT_RANGE), rowFirst_(rowFirst), rowLast_(rowLast), positive_(positive),
        mouseClick_(mouseClick ? *mouseClick : std::optional<GridClickEvent>()) { assert(rowFirst <= rowLast); }
    GridSelectEvent* Clone() const override { return new GridSelectEvent(*this); }

    const size_t rowFirst_; //selected range: [rowFirst_, rowLast_)
    const size_t rowLast_;  //
    const bool positive_; //"false" when clearing selection!
    const std::optional<GridClickEvent> mouseClick_; //filled unless selection was performed via keyboard shortcuts
};

struct GridLabelClickEvent : public wxEvent
{
    GridLabelClickEvent(wxEventType et, ColumnType colType, const wxPoint& mousePos) : wxEvent(0 /*winid*/, et), colType_(colType), mousePos_(mousePos) {}
    GridLabelClickEvent* Clone() const override { return new GridLabelClickEvent(*this); }

    const ColumnType colType_; //may be ColumnType::none
    const wxPoint mousePos_; //client coordinates
};

struct GridColumnResizeEvent : public wxEvent
{
    GridColumnResizeEvent(int offset, ColumnType colType) : wxEvent(0 /*winid*/, EVENT_GRID_COL_RESIZE), colType_(colType), offset_(offset) {}
    GridColumnResizeEvent* Clone() const override { return new GridColumnResizeEvent(*this); }

    const ColumnType colType_;
    const int offset_;
};

struct GridContextMenuEvent : public wxEvent
{
    explicit GridContextMenuEvent(const wxPoint& mousePos) : wxEvent(0 /*winid*/, EVENT_GRID_CONTEXT_MENU), mousePos_(mousePos) {}
    GridContextMenuEvent* Clone() const override { return new GridContextMenuEvent(*this); }

    const wxPoint mousePos_; //client coordinates
};
//------------------------------------------------------------------------------------------------------------

class Grid;


class GridData
{
public:
    virtual ~GridData() {}

    virtual size_t getRowCount() const = 0;

    //cell area:
    virtual std::wstring getValue(size_t row, ColumnType colType) const = 0;
    virtual void         renderRowBackgound(wxDC& dc, const wxRect& rect, size_t row,                     bool enabled, bool selected, HoverArea rowHover); //default implementation
    virtual void         renderCell        (wxDC& dc, const wxRect& rect, size_t row, ColumnType colType, bool enabled, bool selected, HoverArea rowHover);
    virtual int          getBestSize       (wxDC& dc, size_t row, ColumnType colType); //must correspond to renderCell()!
    virtual HoverArea    getMouseHover     (wxDC& dc, size_t row, ColumnType colType, int cellRelativePosX, int cellWidth) { return HoverArea::none; }
    virtual std::wstring getToolTip        (size_t row, ColumnType colType, HoverArea rowHover) { return std::wstring(); }

    //label area:
    virtual std::wstring getColumnLabel(ColumnType colType) const = 0;
    virtual void renderColumnLabel(wxDC& dc, const wxRect& rect, ColumnType colType, bool enabled, bool highlighted); //default implementation
    virtual std::wstring getToolTip(ColumnType colType) const { return std::wstring(); }

    static int getColumnGapLeft(); //for left-aligned text
    static wxColor getColorSelectionGradientFrom();
    static wxColor getColorSelectionGradientTo();

    //optional helper routines:
    static void drawCellText(wxDC& dc, const wxRect& rect, const std::wstring& text,
                             int alignment = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, const wxSize* textExtentHint = nullptr); //returns text extent
    static wxRect drawCellBorder    (wxDC& dc, const wxRect& rect); //returns inner rectangle

    static wxRect drawColumnLabelBackground(wxDC& dc, const wxRect& rect, bool highlighted); //returns inner rectangle
    static void   drawColumnLabelText      (wxDC& dc, const wxRect& rect, const std::wstring& text, bool enabled);
};


enum class GridEventPolicy
{
    allow,
    deny
};


class Grid : public wxScrolledWindow
{
public:
    Grid(wxWindow* parent,
         wxWindowID id        = wxID_ANY,
         const wxPoint& pos   = wxDefaultPosition,
         const wxSize& size   = wxDefaultSize,
         long style           = wxTAB_TRAVERSAL | wxNO_BORDER,
         const wxString& name = wxASCII_STR(wxPanelNameStr));

    size_t getRowCount() const;

    void setRowHeight(int height);

    struct ColAttributes
    {
        ColumnType type = ColumnType::none;
        //first, client width is partitioned according to all available stretch factors, then "offset_" is added
        //universal model: a non-stretched column has stretch factor 0 with the "offset" becoming identical to final width!
        int offset  = 0;
        int stretch = 0; //>= 0
        bool visible = false;
    };

    void setColumnConfig(const std::vector<ColAttributes>& attr); //set column count + widths
    std::vector<ColAttributes> getColumnConfig() const;

    void setDataProvider(const std::shared_ptr<GridData>& dataView) { dataView_ = dataView; }
    /**/  GridData* getDataProvider()       { return dataView_.get(); }
    const GridData* getDataProvider() const { return dataView_.get(); }
    //-----------------------------------------------------------------------------

    void setColumnLabelHeight(int height);
    int getColumnLabelHeight() const;
    void showRowLabel(bool visible);

    enum ScrollBarStatus
    {
        SB_SHOW_AUTOMATIC,
        SB_SHOW_ALWAYS,
        SB_SHOW_NEVER,
    };
    //alternative until wxScrollHelper::ShowScrollbars() becomes available in wxWidgets 2.9
    void showScrollBars(ScrollBarStatus horizontal, ScrollBarStatus vertical);

    std::vector<size_t> getSelectedRows() const { return selection_.get(); }

    void selectRow(size_t row, GridEventPolicy rangeEventPolicy);
    void selectAllRows (GridEventPolicy rangeEventPolicy); //turn off range selection event when calling this function in an event handler to avoid recursion!
    void clearSelection(GridEventPolicy rangeEventPolicy); //
    void selectRange(size_t rowFirst, size_t rowLast, bool positive, GridEventPolicy rangeEventPolicy); //select [rowFirst, rowLast)

    void scrollDelta(int deltaX, int deltaY); //in scroll units

    wxWindow& getCornerWin  ();
    wxWindow& getRowLabelWin();
    wxWindow& getColLabelWin();
    wxWindow& getMainWin    ();
    const wxWindow& getMainWin() const;

    ptrdiff_t getRowAtPos(int posY) const; //return -1 for invalid position, >= rowCount if out of range; absolute coordinates!

    struct ColumnPosInfo
    {
        ColumnType colType   = ColumnType::none; //ColumnType::none no column at x position!
        int cellRelativePosX = 0;
        int colWidth         = 0;
    };
    ColumnPosInfo getColumnAtPos(int posX) const; //absolute position!

    void refreshCell(size_t row, ColumnType colType);

    void enableColumnMove  (bool value) { allowColumnMove_   = value; }
    void enableColumnResize(bool value) { allowColumnResize_ = value; }

    void setGridCursor(size_t row, GridEventPolicy rangeEventPolicy); //set + show + select cursor
    size_t getGridCursor() const; //returns row

    void scrollTo(size_t row);
    size_t getTopRow() const;

    void makeRowVisible(size_t row);

    void Refresh(bool eraseBackground = true, const wxRect* rect = nullptr) override;
    bool Enable(bool enable = true) override;

    //############################################################################################################

private:
    void onKeyDown(wxKeyEvent& event);

    void updateWindowSizes(bool updateScrollbar = true);

    void selectWithCursor(ptrdiff_t row); //emits GridSelectEvent

    wxSize GetSizeAvailableForScrollTarget(const wxSize& size) override; //required since wxWidgets 2.9 if SetTargetWindow() is used


    int getBestColumnSize(size_t col) const; //return -1 on error

    void autoSizeColumns(GridEventPolicy columnResizeEventPolicy);

    friend class GridData;
    class SubWindow;
    class CornerWin;
    class RowLabelWin;
    class ColLabelWin;
    class MainWin;

    class Selection
    {
    public:
        void init(size_t rowCount) { selected_.resize(rowCount); clear(); }

        size_t gridSize() const { return selected_.size(); }

        std::vector<size_t> get() const
        {
            std::vector<size_t> result;
            for (size_t row = 0; row < selected_.size(); ++row)
                if (selected_[row] != 0)
                    result.push_back(row);
            return result;
        }

        void clear() { selectRange(0, selected_.size(), false); }

        bool isSelected(size_t row) const { return row < selected_.size() ? selected_[row] != 0 : false; }

        void selectRange(size_t rowFirst, size_t rowLast, bool positive = true) //select [rowFirst, rowLast), trims if required!
        {
            if (rowFirst <= rowLast)
            {
                rowFirst = std::clamp<size_t>(rowFirst, 0, selected_.size());
                rowLast  = std::clamp<size_t>(rowLast,  0, selected_.size());

                std::fill(selected_.begin() + rowFirst, selected_.begin() + rowLast, positive);
            }
            else assert(false);
        }

    private:
        std::vector<unsigned char> selected_; //effectively a vector<bool> of size "number of rows"
    };

    struct VisibleColumn
    {
        ColumnType type = ColumnType::none;
        int offset  = 0;
        int stretch = 0; //>= 0
    };

    struct ColumnWidth
    {
        ColumnType type = ColumnType::none;
        int width = 0;
    };
    std::vector<ColumnWidth> getColWidths()                 const; //
    std::vector<ColumnWidth> getColWidths(int mainWinWidth) const; //evaluate stretched columns
    int                      getColWidthsSum(int mainWinWidth) const;
    std::vector<int> getColStretchedWidths(int clientWidth) const; //final width = (normalized) (stretchedWidth + offset)

    std::optional<int> getColWidth(size_t col) const
    {
        const auto& widths = getColWidths();
        if (col < widths.size())
            return widths[col].width;
        return {};
    }

    void setColumnWidth(int width, size_t col, GridEventPolicy columnResizeEventPolicy, bool notifyAsync = false);

    wxRect getColumnLabelArea(ColumnType colType) const; //returns empty rect if column not found

    //select inclusive range [rowFrom, rowTo]
    void selectRange2(size_t rowFirst, size_t rowLast, bool positive, const GridClickEvent* mouseClick, GridEventPolicy rangeEventPolicy);

    bool isSelected(size_t row) const { return selection_.isSelected(row); }

    struct ColAction
    {
        bool wantResize = false; //"!wantResize" means "move" or "single click"
        size_t col = 0;
    };
    void moveColumn(size_t colFrom, size_t colTo);

    ColumnType colToType(size_t col) const; //returns ColumnType::none on error

    /*  Grid window layout:
        _______________________________
        | CornerWin   | ColLabelWin   |
        |_____________|_______________|
        | RowLabelWin | MainWin       |
        |             |               |
        |_____________|_______________|  */
    CornerWin*   cornerWin_;
    RowLabelWin* rowLabelWin_;
    ColLabelWin* colLabelWin_;
    MainWin*     mainWin_;

    ScrollBarStatus showScrollbarH_ = SB_SHOW_AUTOMATIC;
    ScrollBarStatus showScrollbarV_ = SB_SHOW_AUTOMATIC;

    bool drawRowLabel_ = true;

    std::shared_ptr<GridData> dataView_;
    Selection selection_;
    bool allowColumnMove_   = true;
    bool allowColumnResize_ = true;

    std::vector<VisibleColumn> visibleCols_; //individual widths, type and total column count
    std::vector<ColAttributes> oldColAttributes_; //visible + nonvisible columns; use for conversion in setColumnConfig()/getColumnConfig() *only*!

    size_t rowCountOld_ = 0; //at the time of last Grid::Refresh()

    int scrollBarHeightH_ = 0; //optional: may not be known (yet)
    int scrollBarWidthV_  = 0; //
};

//------------------------------------------------------------------------------------------------------------

template <class ColAttrReal>
std::vector<ColAttrReal> makeConsistent(const std::vector<ColAttrReal>& attribs, const std::vector<ColAttrReal>& defaults)
{
    using ColTypeReal = decltype(ColAttrReal().type);
    std::vector<ColAttrReal> output;

    std::set<ColTypeReal> usedTypes; //remove duplicates
    auto appendUnique = [&](const std::vector<ColAttrReal>& attr)
    {
        std::copy_if(attr.begin(), attr.end(), std::back_inserter(output),
        [&](const ColAttrReal& a) { return usedTypes.insert(a.type).second; });
    };
    appendUnique(attribs);
    appendUnique(defaults); //make sure each type is existing!

    return output;
}


template <class ColAttrReal>
std::vector<Grid::ColAttributes> convertColAttributes(const std::vector<ColAttrReal>& attribs, const std::vector<ColAttrReal>& defaults)
{
    std::vector<Grid::ColAttributes> output;
    for (const ColAttrReal& ca : makeConsistent(attribs, defaults))
        output.push_back({ static_cast<ColumnType>(ca.type), ca.offset, ca.stretch, ca.visible });
    return output;
}


template <class ColAttrReal>
std::vector<ColAttrReal> convertColAttributes(const std::vector<Grid::ColAttributes>& attribs)
{
    using ColTypeReal = decltype(ColAttrReal().type);

    std::vector<ColAttrReal> output;
    for (const Grid::ColAttributes& ca : attribs)
        output.push_back({ static_cast<ColTypeReal>(ca.type), ca.offset, ca.stretch, ca.visible });
    return output;
}
}

#endif //GRID_H_834702134831734869987
