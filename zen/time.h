// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef TIME_H_8457092814324342453627
#define TIME_H_8457092814324342453627

#include <ctime>
#include "basic_math.h"
#include "zstring.h"


namespace zen
{
struct TimeComp //replaces std::tm and SYSTEMTIME
{
    int year   = 0; // -
    int month  = 0; //1-12
    int day    = 0; //1-31
    int hour   = 0; //0-23
    int minute = 0; //0-59
    int second = 0; //0-60 (including leap second)

    bool operator==(const TimeComp&) const = default;
};

TimeComp getLocalTime(time_t utc = std::time(nullptr)); //convert time_t (UTC) to local time components, returns TimeComp() on error
time_t   localToTimeT(const TimeComp& tc);              //convert local time components to time_t (UTC), returns -1 on error

TimeComp getUtcTime(time_t utc = std::time(nullptr)); //convert time_t (UTC) to UTC time components, returns TimeComp() on error
time_t   utcToTimeT(const TimeComp& tc);              //convert UTC time components to time_t (UTC), returns -1 on error

TimeComp getCompileTime(); //returns TimeComp() on error

//----------------------------------------------------------------------------------------------------------------------------------
/* format (current) date and time; example:
            formatTime(Zstr("%Y|%m|%d")); -> "2011|10|29"
            formatTime(formatDateTag);    -> "2011-10-29"
            formatTime(formatTimeTag);    -> "17:55:34"                       */
Zstring formatTime(const Zchar* format, const TimeComp& tc = getLocalTime()); //format as specified by "std::strftime", returns empty string on failure

//the "format" parameter of formatTime() is partially specialized with the following type tags:
const Zchar* const formatDateTag     = Zstr("%x"); //locale-dependent date representation: e.g. 8/23/2001
const Zchar* const formatTimeTag     = Zstr("%X"); //locale-dependent time representation: e.g. 2:55:02 PM
const Zchar* const formatDateTimeTag = Zstr("%c"); //locale-dependent date and time:       e.g. 8/23/2001 2:55:02 PM

const Zchar* const formatIsoDateTag     = Zstr("%Y-%m-%d");          //e.g. 2001-08-23
const Zchar* const formatIsoTimeTag     = Zstr("%H:%M:%S");          //e.g. 14:55:02
const Zchar* const formatIsoDateTimeTag = Zstr("%Y-%m-%d %H:%M:%S"); //e.g. 2001-08-23 14:55:02

//----------------------------------------------------------------------------------------------------------------------------------
//example: parseTime("%Y-%m-%d %H:%M:%S",  "2001-08-23 14:55:02");
//         parseTime(formatIsoDateTimeTag, "2001-08-23 14:55:02");
template <class String, class String2>
TimeComp parseTime(const String& format, const String2& str); //similar to ::strptime()
//----------------------------------------------------------------------------------------------------------------------------------













//############################ implementation ##############################
namespace impl
{
inline
std::tm toClibTimeComponents(const TimeComp& tc)
{
    assert(1 <= tc.month  && tc.month  <= 12 &&
           1 <= tc.day    && tc.day    <= 31 &&
           0 <= tc.hour   && tc.hour   <= 23 &&
           0 <= tc.minute && tc.minute <= 59 &&
           0 <= tc.second && tc.second <= 61);

    std::tm ctc = {};
    ctc.tm_year  = tc.year - 1900; //years since 1900
    ctc.tm_mon   = tc.month - 1;   //0-11
    ctc.tm_mday  = tc.day;         //1-31
    ctc.tm_hour  = tc.hour;        //0-23
    ctc.tm_min   = tc.minute;      //0-59
    ctc.tm_sec   = tc.second;      //0-60 (including leap second)
    ctc.tm_isdst = -1;             //> 0 if DST is active, == 0 if DST is not active, < 0 if the information is not available
    //ctc.tm_wday
    //ctc.tm_yday
    return ctc;
}

inline
TimeComp toZenTimeComponents(const std::tm& ctc)
{
    TimeComp tc;
    tc.year   = ctc.tm_year + 1900;
    tc.month  = ctc.tm_mon + 1;
    tc.day    = ctc.tm_mday;
    tc.hour   = ctc.tm_hour;
    tc.minute = ctc.tm_min;
    tc.second = ctc.tm_sec;
    return tc;
}


/*
inline
bool isValid(const std::tm& t)
{
     -> not enough! MSCRT has different limits than the C standard which even seem to change with different versions:
        _VALIDATE_RETURN((( timeptr->tm_sec >=0 ) && ( timeptr->tm_sec <= 59 ) ), EINVAL, FALSE)
        _VALIDATE_RETURN(( timeptr->tm_year >= -1900 ) && ( timeptr->tm_year <= 8099 ), EINVAL, FALSE)
    -> also std::mktime does *not* help here at all!

    auto inRange = [](int value, int minVal, int maxVal) { return minVal <= value && value <= maxVal; };

    //https://www.cplusplus.com/reference/clibrary/ctime/tm/
    return inRange(t.tm_sec,  0, 61) &&
           inRange(t.tm_min,  0, 59) &&
           inRange(t.tm_hour, 0, 23) &&
           inRange(t.tm_mday, 1, 31) &&
           inRange(t.tm_mon,  0, 11) &&
           //tm_year
           inRange(t.tm_wday, 0, 6) &&
           inRange(t.tm_yday, 0, 365);
    //tm_isdst
};
*/
}


inline
TimeComp getLocalTime(time_t utc)
{
    if (utc == -1) //failure code from std::time(nullptr)
        return TimeComp();

    std::tm ctc = {};
    if (::localtime_r(&utc, &ctc) == nullptr)
        return TimeComp();

    return impl::toZenTimeComponents(ctc);
}


constexpr auto daysPer400Years = 100 * (4 * 365 /*usual days per year*/ + 1 /*including leap day*/) - 3 /*no leap days for centuries, except if divisible by 400 */;
constexpr auto secsPer400Years = 3600LL * 24 * daysPer400Years;


inline
TimeComp getUtcTime(time_t utc)
{
    if (utc == -1) //failure code from std::time(nullptr)
        return TimeComp();

    std::tm ctc = {};
    if (::gmtime_r(&utc, &ctc) == nullptr) //Linux, macOS: apparently NO limits (tested years 0 to 10.000!)
        return TimeComp();

    return impl::toZenTimeComponents(ctc);
}


inline
time_t localToTimeT(const TimeComp& tc) //returns -1 on error
{
    if (tc == TimeComp())
        return -1;

    std::tm ctc = impl::toClibTimeComponents(tc);
    return std::mktime(&ctc);
}


inline
time_t utcToTimeT(const TimeComp& tc) //returns -1 on error
{
    if (tc == TimeComp())
        return -1;

    std::tm ctc = impl::toClibTimeComponents(tc);
    ctc.tm_isdst = 0; //"Zero (0) to indicate that standard time is in effect" => unused by _mkgmtime, but take no chances


    time_t utc = ::timegm(&ctc);

    return utc;
}


inline
TimeComp getCompileTime()
{
    //https://gcc.gnu.org/onlinedocs/cpp/Standard-Predefined-Macros.html
    char compileTime[] = __DATE__ " " __TIME__; //e.g. "Aug  1 2017 01:32:26"
    if (compileTime[4] == ' ') //day is space-padded, but %d expects zero-padding
        compileTime[4] = '0';

    return parseTime("%b %d %Y %H:%M:%S", compileTime);
}




inline
Zstring formatTime(const Zchar* format, const TimeComp& tc)
{
    if (tc == TimeComp()) //failure code from getLocalTime()
        return Zstring();

    std::tm ctc = impl::toClibTimeComponents(tc);
    std::mktime(&ctc); //unfortunately std::strftime() needs all elements of "struct tm" filled, e.g. tm_wday, tm_yday
    //note: although std::mktime() explicitly expects "local time", calculating weekday and day of year *should* be time-zone and DST independent

    Zstring buffer(256, Zstr('\0'));
    //strftime() craziness on invalid input:
    //  VS 2010: CRASH unless "_invalid_parameter_handler" is set: https://docs.microsoft.com/en-us/cpp/c-runtime-library/parameter-validation
    //  GCC: returns 0, apparently no crash. Still, considering some clib maintainer's comments, we should expect the worst!
    //  Windows: avoid char-based strftime() which uses ANSI encoding! (e.g. Greek letters for AM/PM)
    const size_t charsWritten = std::strftime(&buffer[0], buffer.size(), format, &ctc);
    buffer.resize(charsWritten);
    return buffer;
}


template <class String, class String2>
TimeComp parseTime(const String& format, const String2& str)
{
    using CharType = GetCharTypeT<String>;
    static_assert(std::is_same_v<CharType, GetCharTypeT<String2>>);

    const CharType*       itStr = strBegin(str);
    const CharType* const strLast = itStr + strLength(str);

    auto extractNumber = [&](int& result, size_t digitCount)
    {
        if (strLast - itStr < makeSigned(digitCount))
            return false;

        if (!std::all_of(itStr, itStr + digitCount, isDigit<CharType>))
            return false;

        result = zen::stringTo<int>(makeStringView(itStr, digitCount));
        itStr += digitCount;
        return true;
    };

    TimeComp output;

    const CharType*       itFmt = strBegin(format);
    const CharType* const fmtLast = itFmt + strLength(format);

    for (; itFmt != fmtLast; ++itFmt)
    {
        const CharType fmt = *itFmt;

        if (fmt == '%')
        {
            ++itFmt;
            if (itFmt == fmtLast)
                return TimeComp();

            switch (*itFmt)
            {
                case 'Y':
                    if (!extractNumber(output.year, 4))
                        return TimeComp();
                    break;
                case 'm':
                    if (!extractNumber(output.month, 2))
                        return TimeComp();
                    break;
                case 'b': //abbreviated month name: Jan-Dec
                {
                    if (strLast - itStr < 3)
                        return TimeComp();

                    const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
                    auto itMonth = std::find_if(std::begin(months), std::end(months), [&](const char* month)
                    {
                        return equalAsciiNoCase(makeStringView(itStr, 3), month);
                    });
                    if (itMonth == std::end(months))
                        return TimeComp();

                    output.month = 1 + static_cast<int>(itMonth - std::begin(months));
                    itStr += 3;
                }
                break;
                case 'd':
                    if (!extractNumber(output.day, 2))
                        return TimeComp();
                    break;
                case 'H':
                    if (!extractNumber(output.hour, 2))
                        return TimeComp();
                    break;
                case 'M':
                    if (!extractNumber(output.minute, 2))
                        return TimeComp();
                    break;
                case 'S':
                    if (!extractNumber(output.second, 2))
                        return TimeComp();
                    break;
                default:
                    return TimeComp();
            }
        }
        else if (isWhiteSpace(fmt)) //single whitespace in format => skip 0..n whitespace chars
        {
            while (itStr != strLast && isWhiteSpace(*itStr))
                ++itStr;
        }
        else
        {
            if (itStr == strLast || *itStr != fmt)
                return TimeComp();
            ++itStr;
        }
    }

    if (itStr != strLast)
        return TimeComp();

    return output;
}
}

#endif //TIME_H_8457092814324342453627
