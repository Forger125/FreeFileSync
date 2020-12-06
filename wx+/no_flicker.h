// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef NO_FLICKER_H_893421590321532
#define NO_FLICKER_H_893421590321532

#include <zen/string_tools.h>
#include <zen/scope_guard.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/wupdlock.h>


namespace zen
{
namespace
{
void setText(wxTextCtrl& control, const wxString& newText, bool* additionalLayoutChange = nullptr)
{
    const wxString& label = control.GetValue(); //perf: don't call twice!
    if (additionalLayoutChange && !*additionalLayoutChange) //never revert from true to false!
        *additionalLayoutChange = label.length() != newText.length(); //avoid screen flicker: update layout only when necessary

    if (label != newText)
        control.ChangeValue(newText);
}


void setText(wxStaticText& control, wxString newText, bool* additionalLayoutChange = nullptr)
{

    const wxString& label = control.GetLabel(); //perf: don't call twice!
    if (additionalLayoutChange && !*additionalLayoutChange)
        *additionalLayoutChange = label.length() != newText.length(); //avoid screen flicker: update layout only when necessary

    if (label != newText)
        control.SetLabel(newText);
}


void setTextWithUrls(wxRichTextCtrl& richCtrl, const wxString& newText)
{
    enum class BlockType
    {
        text,
        url,
    };
    std::vector<std::pair<BlockType, wxString>> blocks;

    for (auto it = newText.begin();;)
    {
        const wchar_t urlPrefix[] = L"https://";
        const auto itUrl = std::search(it, newText.end(),
                                       urlPrefix, urlPrefix + strLength(urlPrefix));
        if (it != itUrl)
            blocks.emplace_back(BlockType::text, wxString(it, itUrl));

        if (itUrl == newText.end())
            break;

        auto itUrlEnd = std::find_if(itUrl, newText.end(), [](wchar_t c) { return isWhiteSpace(c); });
        blocks.emplace_back(BlockType::url, wxString(itUrl, itUrlEnd));
        it = itUrlEnd;
    }
    richCtrl.BeginSuppressUndo();
    ZEN_ON_SCOPE_EXIT(richCtrl.EndSuppressUndo());

    //fix mouse scroll speed: why the FUCK is this even necessary!
    richCtrl.SetLineHeight(richCtrl.GetCharHeight());

    //get rid of margins and space between text blocks/"paragraphs"
    richCtrl.SetMargins({0, 0});
    richCtrl.BeginParagraphSpacing(0, 0);
    ZEN_ON_SCOPE_EXIT(richCtrl.EndParagraphSpacing());

    richCtrl.Clear();

    if (std::any_of(blocks.begin(), blocks.end(), [](const auto& item) { return item.first == BlockType::url; }))
    {
        wxRichTextAttr urlStyle;
        urlStyle.SetTextColour(*wxBLUE);
        urlStyle.SetFontUnderlined(true);

        for (const auto& [type, text] : blocks)
            switch (type)
            {
                case BlockType::text:
                    richCtrl.WriteText(text);
                    break;

                case BlockType::url:
                    richCtrl.BeginStyle(urlStyle);
                    ZEN_ON_SCOPE_EXIT(richCtrl.EndStyle());
                    richCtrl.BeginURL(text);
                    ZEN_ON_SCOPE_EXIT(richCtrl.EndURL());
                    richCtrl.WriteText(text);
                    break;
            }

        //register only once! => use a global function pointer, so that Unbind() works correctly:
        using LaunchUrlFun = void(*)(wxTextUrlEvent& event);
        static const LaunchUrlFun launchUrl = [](wxTextUrlEvent& event) { wxLaunchDefaultBrowser(event.GetString()); };

        [[maybe_unused]] const bool unbindOk = richCtrl.Unbind(wxEVT_TEXT_URL, launchUrl);
        richCtrl.Bind(wxEVT_TEXT_URL, launchUrl);
    }
    else
        richCtrl.WriteText(newText);
}
}
}

#endif //NO_FLICKER_H_893421590321532
