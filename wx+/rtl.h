// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef RTL_H_0183487180058718273432148
#define RTL_H_0183487180058718273432148

#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/app.h>


namespace zen
{
//functions supporting right-to-left GUI layout
void drawBitmapRtlMirror  (wxDC& dc, const wxImage& img, const wxRect& rect, int alignment, std::optional<wxBitmap>& buffer);
void drawBitmapRtlNoMirror(wxDC& dc, const wxImage& img, const wxRect& rect, int alignment);
//wxDC::DrawIcon DOES mirror by default -> implement RTL support when needed

wxImage mirrorIfRtl(const wxImage& img);

//manual text flow correction: https://www.w3.org/International/articles/inline-bidi-markup/








//---------------------- implementation ------------------------
namespace impl
{
//don't use wxDC::DrawLabel:
//  - expensive GetTextExtent() call even when passing an empty string!!!
//  - 1-off alignment bugs!
inline
void drawBitmapAligned(wxDC& dc, const wxImage& img, const wxRect& rect, int alignment)
{
    wxPoint pt = rect.GetTopLeft();
    if (alignment & wxALIGN_RIGHT) //note: wxALIGN_LEFT == 0!
        pt.x += rect.width - img.GetWidth();
    else if (alignment & wxALIGN_CENTER_HORIZONTAL)
        pt.x += (rect.width - img.GetWidth()) / 2;

    if (alignment & wxALIGN_BOTTOM) //note: wxALIGN_TOP == 0!
        pt.y += rect.height - img.GetHeight();
    else if (alignment & wxALIGN_CENTER_VERTICAL)
        pt.y += (rect.height - img.GetHeight()) / 2;

    dc.DrawBitmap(img, pt);
}
}


inline
void drawBitmapRtlMirror(wxDC& dc, const wxImage& img, const wxRect& rect, int alignment, std::optional<wxBitmap>& buffer)
{
    switch (dc.GetLayoutDirection())
    {
        case wxLayout_LeftToRight:
            return impl::drawBitmapAligned(dc, img, rect, alignment);

        case wxLayout_RightToLeft:
        {
            if (!buffer || buffer->GetWidth() != rect.width || buffer->GetHeight() < rect.height) //[!] since we do a mirror, width needs to match exactly!
                buffer = wxBitmap(rect.width, rect.height);

            wxMemoryDC memDc(*buffer);
            memDc.Blit(wxPoint(0, 0), rect.GetSize(), &dc, rect.GetTopLeft()); //blit in: background is mirrored due to memDc, dc having different layout direction!

            impl::drawBitmapAligned(memDc, img, wxRect(0, 0, rect.width, rect.height), alignment);
            //note: we cannot simply use memDc.SetLayoutDirection(wxLayout_RightToLeft) due to some strange 1 pixel bug!

            dc.Blit(rect.GetTopLeft(), rect.GetSize(), &memDc, wxPoint(0, 0)); //blit out: mirror once again
        }
        break;

        case wxLayout_Default: //CAVEAT: wxPaintDC/wxMemoryDC on wxGTK/wxMAC does not implement SetLayoutDirection()!!! => GetLayoutDirection() == wxLayout_Default
            if (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft)
                return impl::drawBitmapAligned(dc, img.Mirror(), rect, alignment);
            else
                return impl::drawBitmapAligned(dc, img, rect, alignment);
    }
}


inline
void drawBitmapRtlNoMirror(wxDC& dc, const wxImage& img, const wxRect& rect, int alignment)
{
    return impl::drawBitmapAligned(dc, img, rect, alignment); //wxDC::DrawBitmap does NOT mirror by default
}


inline
wxImage mirrorIfRtl(const wxImage& img)
{
    if (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft)
        return img.Mirror();
    else
        return img;
}
}

#endif //RTL_H_0183487180058718273432148
