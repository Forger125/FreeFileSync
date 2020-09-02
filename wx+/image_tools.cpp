// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "image_tools.h"
#include <zen/string_tools.h>
#include <zen/zstring.h>
#include <wx/app.h>

using namespace zen;


namespace
{
template <int PixBytes>
void copyImageBlock(const unsigned char* src, int srcWidth,
                    /**/  unsigned char* trg, int trgWidth, int blockWidth, int blockHeight)
{
    assert(srcWidth >= blockWidth && trgWidth >= blockWidth);
    const int srcPitch = srcWidth * PixBytes;
    const int trgPitch = trgWidth * PixBytes;
    const int blockPitch = blockWidth * PixBytes;
    for (int y = 0; y < blockHeight; ++y)
        std::memcpy(trg + y * trgPitch, src + y * srcPitch, blockPitch);
}


//...what wxImage::Resize() wants to be when it grows up
void copySubImage(const wxImage& src, wxPoint srcPos,
                  /**/  wxImage& trg, wxPoint trgPos, wxSize blockSize)
{
    auto pointClamp = [](const wxPoint& pos, const wxImage& img) -> wxPoint
    {
        return {
            std::clamp(pos.x, 0, img.GetWidth ()),
            std::clamp(pos.y, 0, img.GetHeight())};
    };
    auto pointMinus = [](const wxPoint& lhs, const wxPoint& rhs) { return wxSize{lhs.x - rhs.x, lhs.y - rhs.y}; };
    //work around yet another wxWidgets screw up: WTF does "operator-(wxPoint, wxPoint)" return wxPoint instead of wxSize!??

    const wxPoint trgPos2    = pointClamp(trgPos,             trg);
    const wxPoint trgPos2End = pointClamp(trgPos + blockSize, trg);

    blockSize = pointMinus(trgPos2End, trgPos2);
    srcPos += pointMinus(trgPos2, trgPos);
    trgPos = trgPos2;
    if (blockSize.x <= 0 || blockSize.y <= 0)
        return;

    const wxPoint srcPos2    = pointClamp(srcPos,             src);
    const wxPoint srcPos2End = pointClamp(srcPos + blockSize, src);

    blockSize = pointMinus(srcPos2End, srcPos2);
    trgPos += pointMinus(srcPos2, srcPos);
    srcPos = srcPos2;
    if (blockSize.x <= 0 || blockSize.y <= 0)
        return;
    //what if target block size is bigger than source block size? should we clear the area that is not copied from source?

    copyImageBlock<3>(src.GetData() + 3 * (srcPos.x + srcPos.y * src.GetWidth()), src.GetWidth(),
                      trg.GetData() + 3 * (trgPos.x + trgPos.y * trg.GetWidth()), trg.GetWidth(),
                      blockSize.x, blockSize.y);

    copyImageBlock<1>(src.GetAlpha() + srcPos.x + srcPos.y * src.GetWidth(), src.GetWidth(),
                      trg.GetAlpha() + trgPos.x + trgPos.y * trg.GetWidth(), trg.GetWidth(),
                      blockSize.x, blockSize.y);
}


void copyImageLayover(const wxImage& src,
                      /**/  wxImage& trg, wxPoint trgPos)
{
    const int srcWidth  = src.GetWidth ();
    const int srcHeight = src.GetHeight();
    const int trgWidth  = trg.GetWidth();

    assert(0 <= trgPos.x && trgPos.x + srcWidth  <= trgWidth       ); //draw area must be a
    assert(0 <= trgPos.y && trgPos.y + srcHeight <= trg.GetHeight()); //subset of target image!

    //https://en.wikipedia.org/wiki/Alpha_compositing
    const unsigned char* srcRgb   = src.GetData();
    const unsigned char* srcAlpha = src.GetAlpha();

    for (int y = 0; y < srcHeight; ++y)
    {
        unsigned char* trgRgb   = trg.GetData () + 3 * (trgPos.x + (trgPos.y + y) * trgWidth);
        unsigned char* trgAlpha = trg.GetAlpha() +      trgPos.x + (trgPos.y + y) * trgWidth;

        for (int x = 0; x < srcWidth; ++x)
        {
            const int w1 = *srcAlpha; //alpha-composition interpreted as weighted average
            const int w2 = *trgAlpha * (255 - w1) / 255;
            const int wSum = w1 + w2;

            auto calcColor = [w1, w2, wSum](unsigned char colsrc, unsigned char colTrg)
            {
                return static_cast<unsigned char>(wSum == 0 ? 0 : (colsrc * w1 + colTrg * w2) / wSum);
            };
            trgRgb[0] = calcColor(srcRgb[0], trgRgb[0]);
            trgRgb[1] = calcColor(srcRgb[1], trgRgb[1]);
            trgRgb[2] = calcColor(srcRgb[2], trgRgb[2]);

            *trgAlpha = static_cast<unsigned char>(wSum);

            srcRgb += 3;
            trgRgb += 3;
            ++srcAlpha;
            ++trgAlpha;
        }
    }
}


std::vector<std::pair<wxString, wxSize>> getTextExtentInfo(const wxString& text, const wxFont& font)
{
    wxMemoryDC dc; //the context used for bitmaps
    dc.SetFont(font); //the font parameter of GetMultiLineTextExtent() is not evaluated on OS X, wxWidgets 2.9.5, so apply it to the DC directly!

    std::vector<std::pair<wxString, wxSize>> lineInfo; //text + extent
    for (const wxString& line : split(text, L'\n', SplitOnEmpty::allow))
        lineInfo.emplace_back(line, line.empty() ? wxSize() : dc.GetTextExtent(line));

    return lineInfo;
}
}


wxImage zen::stackImages(const wxImage& img1, const wxImage& img2, ImageStackLayout dir, ImageStackAlignment align, int gap)
{
    assert(gap >= 0);
    gap = std::max(0, gap);

    const int img1Width  = img1.GetWidth ();
    const int img1Height = img1.GetHeight();
    const int img2Width  = img2.GetWidth ();
    const int img2Height = img2.GetHeight();

    const wxSize newSize = dir == ImageStackLayout::horizontal ?
                           wxSize(img1Width + gap + img2Width,    std::max(img1Height, img2Height)) :
                           wxSize(std::max(img1Width, img2Width), img1Height + gap + img2Height);

    wxImage output(newSize);
    output.SetAlpha();
    std::memset(output.GetAlpha(), wxIMAGE_ALPHA_TRANSPARENT, newSize.x * newSize.y);

    auto calcPos = [&](int imageExtent, int totalExtent)
    {
        switch (align)
        {
            case ImageStackAlignment::center:
                return (totalExtent - imageExtent) / 2;
            case ImageStackAlignment::left: //or top
                return 0;
            case ImageStackAlignment::right: //or bottom
                return totalExtent - imageExtent;
        }
        assert(false);
        return 0;
    };

    switch (dir)
    {
        case ImageStackLayout::horizontal:
            copySubImage(img1, wxPoint(), output, wxPoint(0,               calcPos(img1Height, newSize.y)), img1.GetSize());
            copySubImage(img2, wxPoint(), output, wxPoint(img1Width + gap, calcPos(img2Height, newSize.y)), img2.GetSize());
            break;

        case ImageStackLayout::vertical:
            copySubImage(img1, wxPoint(), output, wxPoint(calcPos(img1Width, newSize.x), 0),                img1.GetSize());
            copySubImage(img2, wxPoint(), output, wxPoint(calcPos(img2Width, newSize.x), img1Height + gap), img2.GetSize());
            break;
    }
    return output;
}


wxImage zen::createImageFromText(const wxString& text, const wxFont& font, const wxColor& col, ImageStackAlignment textAlign)
{
    //assert(!contains(text, L"&")); //accelerator keys not supported here
    wxString textFmt = replaceCpy(text, L"&", L"", false);

    //for some reason wxDC::DrawText messes up "weak" bidi characters even when wxLayout_RightToLeft is set! (--> arrows in hebrew/arabic)
    //=> use mark characters instead:
    if (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft)
        textFmt = RTL_MARK + textFmt + RTL_MARK;

    const std::vector<std::pair<wxString, wxSize>> lineInfo = getTextExtentInfo(textFmt, font);

    int maxWidth   = 0;
    int lineHeight = 0;
    for (const auto& [lineText, lineSize] : lineInfo)
    {
        maxWidth   = std::max(maxWidth,   lineSize.GetWidth());
        lineHeight = std::max(lineHeight, lineSize.GetHeight()); //wxWidgets comment "GetTextExtent will return 0 for empty string"
    }
    if (maxWidth == 0 || lineHeight == 0)
        return wxNullImage;

    wxBitmap newBitmap(maxWidth, lineHeight * lineInfo.size()); //seems we don't need to pass 24-bit depth here even for high-contrast color schemes
    {
        wxMemoryDC dc(newBitmap);
        dc.SetBackground(*wxWHITE_BRUSH);
        dc.Clear();

        dc.SetTextForeground(*wxBLACK); //for proper alpha-channel calculation
        dc.SetTextBackground(*wxWHITE); //
        dc.SetFont(font);

        int posY = 0;
        for (const auto& [lineText, lineSize] : lineInfo)
        {
            if (!lineText.empty())
                switch (textAlign)
                {
                    case ImageStackAlignment::left:
                        dc.DrawText(lineText, wxPoint(0, posY));
                        break;
                    case ImageStackAlignment::right:
                        dc.DrawText(lineText, wxPoint(maxWidth - lineSize.GetWidth(), posY));
                        break;
                    case ImageStackAlignment::center:
                        dc.DrawText(lineText, wxPoint((maxWidth - lineSize.GetWidth()) / 2, posY));
                        break;
                }
            posY += lineHeight;
        }
    }

    //wxDC::DrawLabel() doesn't respect alpha channel => calculate alpha values manually:
    wxImage output(newBitmap.ConvertToImage());
    output.SetAlpha();

    unsigned char* rgb   = output.GetData();
    unsigned char* alpha = output.GetAlpha();
    const int pixelCount = output.GetWidth() * output.GetHeight();

    for (int i = 0; i < pixelCount; ++i)
    {
        //black(0,0,0) becomes wxIMAGE_ALPHA_OPAQUE(255), while white(255,255,255) becomes wxIMAGE_ALPHA_TRANSPARENT(0)
        *alpha++ = static_cast<unsigned char>((255 - rgb[0] + 255 - rgb[1] + 255 - rgb[2]) / 3); //mixed-mode arithmetics!

        *rgb++ = col.Red  (); //
        *rgb++ = col.Green(); //apply actual text color
        *rgb++ = col.Blue (); //
    }
    return output;
}


wxImage zen::layOver(const wxImage& back, const wxImage& front, int alignment)
{
    if (!front.IsOk()) return back;
    assert(front.HasAlpha() && back.HasAlpha());

    const wxSize newSize(std::max(back.GetWidth(),  front.GetWidth()),
                         std::max(back.GetHeight(), front.GetHeight()));

    auto calcNewPos = [&](const wxImage& img)
    {
        wxPoint newPos;
        if (alignment & wxALIGN_RIGHT) //note: wxALIGN_LEFT == 0!
            newPos.x = newSize.GetWidth() - img.GetWidth();
        else if (alignment & wxALIGN_CENTER_HORIZONTAL)
            newPos.x = (newSize.GetWidth() - img.GetWidth()) / 2;

        if (alignment & wxALIGN_BOTTOM) //note: wxALIGN_TOP == 0!
            newPos.y = newSize.GetHeight() - img.GetHeight();
        else if (alignment & wxALIGN_CENTER_VERTICAL)
            newPos.y = (newSize.GetHeight() - img.GetHeight()) / 2;

        return newPos;
    };

    wxImage output(newSize);
    output.SetAlpha();
    std::memset(output.GetAlpha(), wxIMAGE_ALPHA_TRANSPARENT, newSize.x * newSize.y);

    copySubImage(back, wxPoint(), output, calcNewPos(back), back.GetSize());
    //use resizeCanvas()? might return ref-counted copy!

    //can't use wxMemoryDC and wxDC::DrawBitmap(): no alpha channel support on wxGTK!
    copyImageLayover(front, output, calcNewPos(front));

    return output;
}


wxImage zen::resizeCanvas(const wxImage& img, wxSize newSize, int alignment)
{
    if (newSize == img.GetSize())
        return img; //caveat: wxImage is ref-counted *without* copy on write

    wxPoint newPos;
    if (alignment & wxALIGN_RIGHT) //note: wxALIGN_LEFT == 0!
        newPos.x = newSize.GetWidth() - img.GetWidth();
    else if (alignment & wxALIGN_CENTER_HORIZONTAL)
        newPos.x = static_cast<int>(std::floor((newSize.GetWidth() - img.GetWidth()) / 2)); //consistency: round down negative values, too!

    if (alignment & wxALIGN_BOTTOM) //note: wxALIGN_TOP == 0!
        newPos.y = newSize.GetHeight() - img.GetHeight();
    else if (alignment & wxALIGN_CENTER_VERTICAL)
        newPos.y = static_cast<int>(std::floor((newSize.GetHeight() - img.GetHeight()) / 2)); //consistency: round down negative values, too!

    wxImage output(newSize);
    output.SetAlpha();
    std::memset(output.GetAlpha(), wxIMAGE_ALPHA_TRANSPARENT, newSize.x * newSize.y);

    copySubImage(img, wxPoint(), output, newPos, img.GetSize());
    //about 50x faster than e.g. wxImage::Resize!!! suprise :>
    return output;
}


wxImage zen::shrinkImage(const wxImage& img, int maxWidth /*optional*/, int maxHeight /*optional*/)
{
    wxSize newSize = img.GetSize();

    if (maxWidth >= 0)
        if (maxWidth < newSize.x)
        {
            newSize.y = newSize.y * maxWidth / newSize.x;
            newSize.x = maxWidth;
        }
    if (maxHeight >= 0)
        if (maxHeight < newSize.y)
        {
            newSize = img.GetSize();                       //avoid loss of precision
            newSize.x = newSize.x * maxHeight / newSize.y; //
            newSize.y = maxHeight;
        }

    if (newSize == img.GetSize())
        return img;

    return img.Scale(newSize.x, newSize.y, wxIMAGE_QUALITY_BILINEAR); //looks sharper than wxIMAGE_QUALITY_HIGH!
    //perf: use xbrz::bilinearScale instead? only about 10% shorter runtime
}


void zen::convertToVanillaImage(wxImage& img)
{
    if (!img.HasAlpha())
    {
        const int width  = img.GetWidth ();
        const int height = img.GetHeight();
        if (width <= 0 || height <= 0) return;

        unsigned char mask_r = 0;
        unsigned char mask_g = 0;
        unsigned char mask_b = 0;
        const bool haveMask = img.HasMask() && img.GetOrFindMaskColour(&mask_r, &mask_g, &mask_b);
        //check for mask before calling wxImage::GetOrFindMaskColour() to skip needlessly searching for new mask color

        img.SetAlpha();
        ::memset(img.GetAlpha(), wxIMAGE_ALPHA_OPAQUE, width * height);

        //wxWidgets, as always, tries to be more clever than it really is and fucks up wxStaticBitmap if wxBitmap is fully opaque:
        img.GetAlpha()[width * height - 1] = 254;

        if (haveMask)
        {
            img.SetMask(false);
            unsigned char*       alpha = img.GetAlpha();
            const unsigned char* rgb   = img.GetData();

            const int pixelCount = width * height;
            for (int i = 0; i < pixelCount; ++ i)
            {
                const unsigned char r = *rgb++;
                const unsigned char g = *rgb++;
                const unsigned char b = *rgb++;

                if (r == mask_r &&
                    g == mask_g &&
                    b == mask_b)
                    alpha[i] = wxIMAGE_ALPHA_TRANSPARENT;
            }
        }
    }
    else
    {
        assert(!img.HasMask());
    }
}
