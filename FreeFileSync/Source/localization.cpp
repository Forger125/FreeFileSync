// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "localization.h"
#include <unordered_map>
#include <clocale> //setlocale
#include <map>
#include <list>
#include <iterator>
#include <zen/string_tools.h>
#include <zen/file_traverser.h>
#include <zen/file_io.h>
#include <zen/i18n.h>
#include <zen/format_unit.h>
#include <zen/perf.h>
#include <wx/zipstrm.h>
#include <wx/mstream.h>
#include <wx/intl.h>
#include <wx/log.h>
#include "parse_plural.h"
#include "parse_lng.h"

    #include <wchar.h> //wcscasecmp


using namespace zen;
using namespace fff;


namespace
{
class FFSTranslation : public TranslationHandler
{
public:
    explicit FFSTranslation(const std::string& lngStream); //throw lng::ParsingError, plural::ParsingError

    std::wstring translate(const std::wstring& text) const override
    {
        //look for translation in buffer table
        auto it = transMapping_.find(text);
        if (it != transMapping_.end() && !it->second.empty())
            return it->second;
        return text; //fallback
    }

    std::wstring translate(const std::wstring& singular, const std::wstring& plural, int64_t n) const override
    {
        auto it = transMappingPl_.find({singular, plural});
        if (it != transMappingPl_.end())
        {
            const size_t formNo = pluralParser_->getForm(n);
            assert(formNo < it->second.size());
            if (formNo < it->second.size())
                return replaceCpy(it->second[formNo], L"%x", formatNumber(n));
        }
        return replaceCpy(std::abs(n) == 1 ? singular : plural, L"%x", formatNumber(n)); //fallback
    }

private:
    using Translation       = std::unordered_map<std::wstring, std::wstring>; //hash_map is 15% faster than std::map on GCC
    using TranslationPlural = std::map<std::pair<std::wstring, std::wstring>, std::vector<std::wstring>>;

    Translation       transMapping_; //map original text |-> translation
    TranslationPlural transMappingPl_;
    std::unique_ptr<plural::PluralForm> pluralParser_; //bound!
};


FFSTranslation::FFSTranslation(const std::string& lngStream) //throw lng::ParsingError, plural::ParsingError
{
    lng::TransHeader          header;
    lng::TranslationMap       transUtf;
    lng::TranslationPluralMap transPluralUtf;
    lng::parseLng(lngStream, header, transUtf, transPluralUtf); //throw ParsingError

    pluralParser_ = std::make_unique<plural::PluralForm>(header.pluralDefinition); //throw plural::ParsingError

    for (const auto& [original, translation] : transUtf)
        transMapping_.emplace(utfTo<std::wstring>(original),
                              utfTo<std::wstring>(translation));

    for (const auto& [singAndPlural, pluralForms] : transPluralUtf)
    {
        std::vector<std::wstring> transPluralForms;
        for (const std::string& pf : pluralForms)
            transPluralForms.push_back(utfTo<std::wstring>(pf));

        transMappingPl_.insert({{
                utfTo<std::wstring>(singAndPlural.first),
                utfTo<std::wstring>(singAndPlural.second)
            },
            std::move(transPluralForms)});
    }
}


std::vector<TranslationInfo> loadTranslations(const Zstring& zipPath) //throw FileError
{
    std::vector<std::pair<Zstring /*file name*/, std::string /*byte stream*/>> streams;

    try //to load from ZIP first:
    {
        const std::string rawStream = getFileContent(zipPath, nullptr /*notifyUnbufferedIO*/); //throw FileError
        wxMemoryInputStream memStream(rawStream.c_str(), rawStream.size()); //does not take ownership
        wxZipInputStream zipStream(memStream, wxConvUTF8);

        while (const auto& entry = std::unique_ptr<wxZipEntry>(zipStream.GetNextEntry())) //take ownership!
            if (std::string stream(entry->GetSize(), '\0');
                zipStream.ReadAll(stream.data(), stream.size()))
                streams.emplace_back(utfTo<Zstring>(entry->GetName()), std::move(stream));
            else
                assert(false);
    }
    catch (FileError&) //fall back to folder: dev build (only!?)
    {
        const Zstring fallbackFolder = beforeLast(zipPath, Zstr(".zip"), IfNotFoundReturn::none);
        if (!itemStillExists(fallbackFolder)) //throw FileError
            throw;

        traverseFolder(fallbackFolder, [&](const FileInfo& fi)
        {
            if (endsWith(fi.fullPath, Zstr(".lng")))
            {
                std::string stream = getFileContent(fi.fullPath, nullptr /*notifyUnbufferedIO*/); //throw FileError
                streams.emplace_back(fi.itemName, std::move(stream));
            }
        }, nullptr, nullptr, [](const std::wstring& errorMsg) { throw FileError(errorMsg); });
    }
    //--------------------------------------------------------------------

    std::vector<TranslationInfo> translations
    {
        //default entry:
        {
            .languageID     = wxLANGUAGE_ENGLISH_US,
            .locale         = "en_US",
            .languageName   = L"English",
            .translatorName = L"Zenju",
            .languageFlag   = "flag_usa",
            .lngFileName    = Zstr(""),
            .lngStream      = "",
        }
    };

    for (/*const*/ auto& [fileName, stream] : streams)
        try
        {
            const lng::TransHeader lngHeader = lng::parseHeader(stream); //throw ParsingError
            assert(!lngHeader.languageName  .empty());
            assert(!lngHeader.translatorName.empty());
            assert(!lngHeader.locale        .empty());
            assert(!lngHeader.flagFile      .empty());

            const wxLanguageInfo* lngInfo = wxLocale::FindLanguageInfo(utfTo<wxString>(lngHeader.locale));
            assert(lngInfo && lngInfo->CanonicalName == utfTo<wxString>(lngHeader.locale));
            if (lngInfo)
                translations.push_back(
            {
                .languageID     = static_cast<wxLanguage>(lngInfo->Language),
                .locale         = lngHeader.locale,
                .languageName   = utfTo<std::wstring>(lngHeader.languageName),
                .translatorName = utfTo<std::wstring>(lngHeader.translatorName),
                .languageFlag   = lngHeader.flagFile,
                .lngFileName    = fileName,
                .lngStream      = std::move(stream),
            });
        }
        catch (lng::ParsingError&) { assert(false); }

    std::sort(translations.begin(), translations.end(), [](const TranslationInfo& lhs, const TranslationInfo& rhs)
    {
        return LessNaturalSort()(utfTo<Zstring>(lhs.languageName),
                                 utfTo<Zstring>(rhs.languageName)); //"natural" sort: ignore case and diacritics
    });
    return translations;
}


/* Some ISO codes are used by multiple wxLanguage IDs which can lead to incorrect mapping by wxLocale::FindLanguageInfo()!!!
    => Identify by description, e.g. "Chinese (Traditional)". The following IDs are affected:
    - zh_TW: wxLANGUAGE_CHINESE_TAIWAN, wxLANGUAGE_CHINESE, wxLANGUAGE_CHINESE_TRADITIONAL_EXPLICIT
    - en_GB: wxLANGUAGE_ENGLISH_UK, wxLANGUAGE_ENGLISH
    - es_ES: wxLANGUAGE_SPANISH, wxLANGUAGE_SPANISH_SPAIN                      */
wxLanguage mapLanguageDialect(wxLanguage lng)
{
    if (const wxString& canonicalName = wxLocale::GetLanguageCanonicalName(lng);
        !canonicalName.empty())
    {
        assert(!contains(canonicalName, L'-'));
        const std::string locale = beforeFirst(utfTo<std::string>(canonicalName), '@', IfNotFoundReturn::all); //e.g. "sr_RS@latin"; see wxLocale::InitLanguagesDB()
        const std::string lngCode = beforeFirst(locale, '_', IfNotFoundReturn::all);

        if (lngCode == "zh")
        {
            if (lng == wxLANGUAGE_CHINESE) //wxWidgets assigns this to "zh" or "zh_TW" for some reason
                return wxLANGUAGE_CHINESE_CHINA;

            for (const char* l : {"zh_HK", "zh_MO", "zh_TW"})
                if (locale == l)
                    return wxLANGUAGE_CHINESE_TAIWAN;

            return wxLANGUAGE_CHINESE_CHINA;
        }

        if (lngCode == "en")
        {
            if (lng == wxLANGUAGE_ENGLISH || //wxWidgets assigns this to "en" or "en_GB" for some reason
                lng == wxLANGUAGE_ENGLISH_WORLD)
                return wxLANGUAGE_ENGLISH_US;

            for (const char* l : {"en_US", "en_CA", "en_AS", "en_UM", "en_VI"})
                if (locale == l)
                    return wxLANGUAGE_ENGLISH_US;

            return wxLANGUAGE_ENGLISH_UK;
        }

        if (lngCode == "nb" || lngCode == "nn") //wxLANGUAGE_NORWEGIAN_BOKMAL, wxLANGUAGE_NORWEGIAN_NYNORSK
            return wxLANGUAGE_NORWEGIAN;

        if (locale == "pt_BR")
            return wxLANGUAGE_PORTUGUESE_BRAZILIAN;

        //all other cases: map to primary language code
        if (contains(locale, '_'))
            if (const wxLanguageInfo* lngInfo2 = wxLocale::FindLanguageInfo(utfTo<wxString>(lngCode)))
                return static_cast<wxLanguage>(lngInfo2->Language);
    }
    return lng; //including wxLANGUAGE_DEFAULT, wxLANGUAGE_UNKNOWN
}


//we need to interface with wxWidgets' translation handling for a few translations used in their internal source files
// => since there is no better API: dynamically generate a MO file and feed it to wxTranslation
class MemoryTranslationLoader : public wxTranslationsLoader
{
public:
    MemoryTranslationLoader(wxLanguage langId, std::map<std::string, std::wstring>&& transMapping) :
        canonicalName_(wxLocale::GetLanguageCanonicalName(langId))
    {
        assert(!canonicalName_.empty());
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(transMapping)>, std::map<std::string, std::wstring>>); //translations *must* be sorted in MO file!

        //https://www.gnu.org/software/gettext/manual/html_node/MO-Files.html
        transMapping[""] = L"Content-Type: text/plain; charset=UTF-8\n";

        const int headerSize = 28;
        writeNumber<uint32_t>(moBuf_, 0x950412de); //magic number
        writeNumber<uint32_t>(moBuf_, 0); //format version
        writeNumber<uint32_t>(moBuf_, transMapping.size()); //string count
        writeNumber<uint32_t>(moBuf_, headerSize);                           //string references offset: original
        writeNumber<uint32_t>(moBuf_, headerSize + (2 * sizeof(uint32_t)) * transMapping.size()); //string references offset: translation
        writeNumber<uint32_t>(moBuf_, 0); //size of hashing table
        writeNumber<uint32_t>(moBuf_, 0); //offset of hashing table

        const int stringsOffset = headerSize + 2 * (2 * sizeof(uint32_t)) * transMapping.size();
        std::string stringsList;

        for (const auto& [original, translation] : transMapping)
        {
            writeNumber<uint32_t>(moBuf_, original.size()); //string length
            writeNumber<uint32_t>(moBuf_, stringsOffset + stringsList.size()); //string offset
            stringsList.append(original.c_str(), original.size() + 1); //include 0-termination
        }

        for (const auto& [original, translationW] : transMapping)
        {
            const auto& translation = utfTo<std::string>(translationW);
            writeNumber<uint32_t>(moBuf_, translation.size()); //string length
            writeNumber<uint32_t>(moBuf_, stringsOffset + stringsList.size()); //string offset
            stringsList.append(translation.c_str(), translation.size() + 1); //include 0-termination
        }

        writeArray(moBuf_, stringsList.c_str(), stringsList.size());
    }

    wxMsgCatalog* LoadCatalog(const wxString& domain, const wxString& lang) override
    {
        //"lang" is NOT (exactly) what we return from GetAvailableTranslations(), but has a little "extra", e.g.: de_DE.WINDOWS-1252 or ar.WINDOWS-1252
        auto extractIsoLangCode = [](wxString langCode)
        {
            langCode = beforeLast(langCode, L".", IfNotFoundReturn::all);
            return beforeLast(langCode, L"_", IfNotFoundReturn::all);
        };

        if (equalAsciiNoCase(extractIsoLangCode(lang), extractIsoLangCode(canonicalName_)))
            return wxMsgCatalog::CreateFromData(wxScopedCharBuffer::CreateNonOwned(moBuf_.ref().c_str(), moBuf_.ref().size()), domain);
        assert(false);
        return nullptr;
    }

    wxArrayString GetAvailableTranslations(const wxString& domain) const override
    {
        wxArrayString available;
        available.push_back(canonicalName_);
        return available;
    }

private:
    const wxString canonicalName_;
    MemoryStreamOut<std::string> moBuf_;
};


//global wxWidgets localization: sets up C locale as well!
class ZenLocale
{
public:
    static ZenLocale& getInstance()
    {
        static ZenLocale inst;
        return inst;
    }

    void init(wxLanguage lng)
    {
        lng_ = lng;

        if (const wxLanguageInfo* selLngInfo = wxLocale::GetLanguageInfo(lng))
            layoutDir_ = selLngInfo->LayoutDirection;
        else
            layoutDir_ = wxLayout_LeftToRight;

        /* use wxLANGUAGE_DEFAULT to preserve sub-language-specific rules (e.g. number and date format)
            - beneficial even for Arabic locale: use user-specific date settings instead of Hijri calendar year 1441 (= Gregorian 2019)
            - note when testing: format_unit.cpp::formatNumber() always uses LOCALE_NAME_USER_DEFAULT => test date format instead */
        if (!locale_)
        {
            //wxWidgets shows a modal dialog on error during wxLocale::Init() -> at least we can shut it up!
            wxLog* oldLogTarget = wxLog::SetActiveTarget(new wxLogStderr); //transfer and receive ownership!
            ZEN_ON_SCOPE_EXIT(delete wxLog::SetActiveTarget(oldLogTarget));

            //locale_.reset(); //avoid global locale lifetime overlap! wxWidgets cannot handle this and will crash!
            locale_ = std::make_unique<wxLocale>(wxLANGUAGE_DEFAULT, wxLOCALE_DONT_LOAD_DEFAULT /*we're not using wxwin.mo*/);
            //wxLANGUAGE_DEFAULT => internally calls std::setlocale(LC_ALL, "" /*== user-preferred locale*/) on Windows/Linux (but not macOS)
            /*  => exactly what's needed on Windows/Linux

                but not needed on macOS; even detrimental:
                - breaks wxWidgets file drag and drop! https://freefilesync.org/forum/viewtopic.php?t=8215
                - even wxWidgets knows: "under macOS C locale must not be changed, as doing this exposes bugs in the system"
                    https://docs.wxwidgets.org/trunk/classwx_u_i_locale.html

            reproduce: - std::setlocale(LC_ALL, "");
                       - double-click the app (*)
                       - drag and drop folder named "アアアア"
                       - wxFileDropTarget::OnDropFiles() called with empty file array!

            *) CAVEAT: context matters! this yields a different user-preferred locale than running Contents/MacOS/FreeFileSync_main!!!
            e.g. 1. locale after wxLocale creation is "en_US"
                 2. call std::setlocale(LC_ALL, ""):
                   a) app was double-clicked:                 locale is "C"            => drag/drop FAILS!
                   b) run Contents/MacOS/FreeFileSync_main:   locale is "en_US.UTF-8"  => drag/drop works!                        */
            assert(locale_->IsOk());

            //const char* currentLocale = std::setlocale(LC_ALL, nullptr);
        }
    }

    void tearDown() { locale_.reset(); lng_ = wxLANGUAGE_UNKNOWN; layoutDir_ = wxLayout_Default; }

    wxLanguage getLanguage() const { return lng_; }
    wxLayoutDirection getLayoutDirection() const { return layoutDir_; }

private:
    ZenLocale() {}
    ~ZenLocale() { assert(!locale_); }

    wxLanguage lng_ = wxLANGUAGE_UNKNOWN;
    wxLayoutDirection layoutDir_ = wxLayout_Default;
    std::unique_ptr<wxLocale> locale_;
    //use wxWidgets 3.1.6 wxUILocale? wxLocale already does *exactly* what we need, while wxLocale does a half-arsed job (as expected):
    //setlocale() is only called on wxGTK, but not on Windows + wxTranslations is not initialized.
};


std::vector<TranslationInfo> globalTranslations;
}


const std::vector<TranslationInfo>& fff::getAvailableTranslations()
{
    assert(!globalTranslations.empty()); //localizationInit() not called, or failed!?
    return globalTranslations;
}


void fff::localizationInit(const Zstring& zipPath) //throw FileError
{
    assert(globalTranslations.empty());
    globalTranslations = loadTranslations(zipPath); //throw FileError
    setLanguage(getDefaultLanguage()); //throw FileError
}


void fff::localizationCleanup()
{
    assert(!globalTranslations.empty());
    ZenLocale::getInstance().tearDown();
    setTranslator(nullptr); //good place for clean up rather than some time during static destruction: is this an actual benefit???
    globalTranslations.clear();
}


void fff::setLanguage(wxLanguage lng) //throw FileError
{
    if (getLanguage() == lng)
        return; //support polling

    //(try to) retrieve language file
    std::string lngStream;
    Zstring lngFileName;

    for (const TranslationInfo& e : getAvailableTranslations())
        if (e.languageID == lng)
        {
            lngStream   = e.lngStream;
            lngFileName = e.lngFileName;
            break;
        }

    //load language file into buffer
    if (lngStream.empty()) //if file stream is empty, texts will be English (US) by default
    {
        setTranslator(nullptr);
        lng = wxLANGUAGE_ENGLISH_US;
    }
    else
        try
        {
            setTranslator(std::make_unique<FFSTranslation>(lngStream)); //throw lng::ParsingError, plural::ParsingError
        }
        catch (lng::ParsingError& e)
        {
            throw FileError(replaceCpy(replaceCpy(replaceCpy(_("Error parsing file %x, row %y, column %z."),
                                                             L"%x", fmtPath(lngFileName)),
                                                  L"%y", formatNumber(e.row + 1)),
                                       L"%z", formatNumber(e.col + 1))
                            + L"\n\n" + e.msg);
        }
        catch (plural::ParsingError&)
        {
            throw FileError(L"Invalid plural form definition: " + fmtPath(lngFileName)); //user should never see this!
        }

    //handle RTL swapping: we need wxWidgets to do this
    ZenLocale::getInstance().init(lng);

    //add translation for wxWidgets-internal strings:
    assert(wxTranslations::Get()); //already initialized by wxLocale
    if (wxTranslations* wxtrans = wxTranslations::Get())
    {
        std::map<std::string, std::wstring> transMapping =
        {
        };
        wxtrans->SetLanguage(lng); //!= wxLocale's language, which could be wxLANGUAGE_DEFAULT (see ZenLocale)
        wxtrans->SetLoader(new MemoryTranslationLoader(lng, std::move(transMapping)));
        [[maybe_unused]] const bool catalogAdded = wxtrans->AddCatalog(wxString());
        assert(catalogAdded || lng == wxLANGUAGE_ENGLISH_US);
    }
}


wxLanguage fff::getDefaultLanguage()
{
    static const wxLanguage defaultLng = mapLanguageDialect(static_cast<wxLanguage>(wxLocale::GetSystemLanguage()));
    //uses GetUserPreferredUILanguages() since wxWidgets 1.3.6, not GetUserDefaultUILanguage() anymore:
    // https://github.com/wxWidgets/wxWidgets/blob/master/src/common/intl.cpp
    return defaultLng;
}


wxLanguage fff::getLanguage()
{
    return ZenLocale::getInstance().getLanguage();
}


wxLayoutDirection fff::getLayoutDirection()
{
    return ZenLocale::getInstance().getLayoutDirection();
}
