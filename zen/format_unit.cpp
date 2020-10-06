// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "format_unit.h"
#include <ctime>
#include <cstdio>
#include <stdexcept>
#include "basic_math.h"
#include "sys_error.h"
#include "i18n.h"
#include "time.h"
#include "globals.h"
#include "utf.h"

    #include <iostream>
    #include <langinfo.h>
    #include <clocale> //thousands separator
    #include "utf.h"   //

using namespace zen;


std::wstring zen::formatTwoDigitPrecision(double value)
{
    //print two digits: 0,1 | 1,1 | 11
    if (numeric::abs(value) < 9.95) //9.99 must not be formatted as "10.0"
        return printNumber<std::wstring>(L"%.1f", value);
    return numberTo<std::wstring>(numeric::round(value));
}


std::wstring zen::formatThreeDigitPrecision(double value)
{
    //print three digits: 0,01 | 0,11 | 1,11 | 11,1 | 111
    if (numeric::abs(value) < 9.995) //9.999 must not be formatted as "10.00"
        return printNumber<std::wstring>(L"%.2f", value);
    if (numeric::abs(value) < 99.95) //99.99 must not be formatted as "100.0"
        return printNumber<std::wstring>(L"%.1f", value);
    return numberTo<std::wstring>(numeric::round(value));
}


std::wstring zen::formatFilesizeShort(int64_t size)
{
    //if (size < 0) return _("Error"); -> really?

    if (numeric::abs(size) <= 999)
        return _P("1 byte", "%x bytes", static_cast<int>(size));

    double sizeInUnit = static_cast<double>(size);

    auto formatUnit = [&](const std::wstring& unitTxt) { return replaceCpy(unitTxt, L"%x", formatThreeDigitPrecision(sizeInUnit)); };

    sizeInUnit /= 1024;
    if (numeric::abs(sizeInUnit) < 999.5)
        return formatUnit(_("%x KB"));

    sizeInUnit /= 1024;
    if (numeric::abs(sizeInUnit) < 999.5)
        return formatUnit(_("%x MB"));

    sizeInUnit /= 1024;
    if (numeric::abs(sizeInUnit) < 999.5)
        return formatUnit(_("%x GB"));

    sizeInUnit /= 1024;
    if (numeric::abs(sizeInUnit) < 999.5)
        return formatUnit(_("%x TB"));

    sizeInUnit /= 1024;
    return formatUnit(_("%x PB"));
}


namespace
{
enum class UnitRemTime
{
    sec,
    min,
    hour,
    day
};


std::wstring formatUnitTime(int val, UnitRemTime unit)
{
    switch (unit)
    {
        case UnitRemTime::sec:
            return _P("1 sec", "%x sec", val);
        case UnitRemTime::min:
            return _P("1 min", "%x min", val);
        case UnitRemTime::hour:
            return _P("1 hour", "%x hours", val);
        case UnitRemTime::day:
            return _P("1 day", "%x days", val);
    }
    assert(false);
    return _("Error");
}


template <int M, int N>
std::wstring roundToBlock(double timeInHigh,
                          UnitRemTime unitHigh, const int (&stepsHigh)[M],
                          int unitLowPerHigh,
                          UnitRemTime unitLow, const int (&stepsLow)[N])
{
    assert(unitLowPerHigh > 0);
    const double granularity = 0.1;
    const double timeInLow = timeInHigh * unitLowPerHigh;
    const int blockSizeLow = granularity * timeInHigh < 1 ?
                             numeric::nearMatch(granularity * timeInLow,  std::begin(stepsLow),  std::end(stepsLow)):
                             numeric::nearMatch(granularity * timeInHigh, std::begin(stepsHigh), std::end(stepsHigh)) * unitLowPerHigh;
    const int roundedtimeInLow = static_cast<int>(numeric::round(timeInLow / blockSizeLow) * blockSizeLow);

    std::wstring output = formatUnitTime(roundedtimeInLow / unitLowPerHigh, unitHigh);
    if (unitLowPerHigh > blockSizeLow)
        output += L' ' + formatUnitTime(roundedtimeInLow % unitLowPerHigh, unitLow);
    return output;
}
}


std::wstring zen::formatRemainingTime(double timeInSec)
{
    const int steps10[] = { 1, 2, 5, 10 };
    const int steps24[] = { 1, 2, 3, 4, 6, 8, 12, 24 };
    const int steps60[] = { 1, 2, 5, 10, 15, 20, 30, 60 };

    //determine preferred unit
    double timeInUnit = timeInSec;
    if (timeInUnit <= 60)
        return roundToBlock(timeInUnit, UnitRemTime::sec, steps60, 1, UnitRemTime::sec, steps60);

    timeInUnit /= 60;
    if (timeInUnit <= 60)
        return roundToBlock(timeInUnit, UnitRemTime::min, steps60, 60, UnitRemTime::sec, steps60);

    timeInUnit /= 60;
    if (timeInUnit <= 24)
        return roundToBlock(timeInUnit, UnitRemTime::hour, steps24, 60, UnitRemTime::min, steps60);

    timeInUnit /= 24;
    return roundToBlock(timeInUnit, UnitRemTime::day, steps10, 24, UnitRemTime::hour, steps24);
    //note: for 10% granularity steps10 yields a valid blocksize only up to timeInUnit == 100!
    //for larger time sizes this results in a finer granularity than expected: 10 days -> should not be a problem considering "usual" remaining time for synchronization
}


//std::wstring zen::fractionToString1Dec(double fraction)
//{
//    return printNumber<std::wstring>(L"%.1f", fraction * 100.0) + L'%'; //no need to internationalize fraction!?
//}


std::wstring zen::formatFraction(double fraction)
{
    return printNumber<std::wstring>(L"%.2f", fraction * 100.0) + L'%'; //no need to internationalize fraction!?
}




std::wstring zen::formatNumber(int64_t n)
{
    //::setlocale (LC_ALL, ""); -> see localization.cpp::wxWidgetsLocale
    static_assert(sizeof(long long int) == sizeof(n));
    return printNumber<std::wstring>(L"%'lld", n); //considers grouping (')
}


std::wstring zen::formatUtcToLocalTime(time_t utcTime)
{
    auto errorMsg = [&] { return _("Error") + L" (time_t: " + numberTo<std::wstring>(utcTime) + L')'; };

    TimeComp loc = getLocalTime(utcTime);

    std::wstring dateString = utfTo<std::wstring>(formatTime(Zstr("%x  %X"), loc));
    return !dateString.empty() ? dateString : errorMsg();
}




WeekDay impl::getFirstDayOfWeekImpl() //throw SysError
{
    /*  testing: change locale via command line
        ---------------------------------------
        LC_TIME=en_DK.utf8    => Monday
        LC_TIME=en_US.utf8    => Sunday           */

    const char* firstDay = ::nl_langinfo(_NL_TIME_FIRST_WEEKDAY); //[1-Sunday, 7-Saturday]
    ASSERT_SYSERROR(firstDay && 1 <= *firstDay && *firstDay <= 7);

    const int weekDayStartSunday = *firstDay;
    const int weekDayStartMonday = (weekDayStartSunday - 1 + 6) % 7; //+6 == -1 in Z_7
    // [0-Monday, 6-Sunday]
    return static_cast<WeekDay>(weekDayStartMonday);
}


WeekDay zen::getFirstDayOfWeek()
{
    static const WeekDay weekDay = []
    {
        try
        {
            return impl::getFirstDayOfWeekImpl(); //throw SysError
        }
        catch (const SysError& e)
        {
            throw std::runtime_error(std::string(__FILE__) + '[' + numberTo<std::string>(__LINE__) + "] Failed to get first day of the week." + "\n\n" +
                                     utfTo<std::string>(e.toString()));
        }
    }();
    return weekDay;
}
