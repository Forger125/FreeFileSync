// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef STATUS_HANDLER_IMPL_H_07682758976
#define STATUS_HANDLER_IMPL_H_07682758976

#include <zen/optional.h>
#include <zen/file_error.h>
#include "../process_callback.h"


namespace fff
{
template <typename Function> inline
zen::Opt<std::wstring> tryReportingError(Function cmd, ProcessCallback& cb /*throw X*/) //return ignored error message if available
{
    for (size_t retryNumber = 0;; ++retryNumber)
        try
        {
            cmd(); //throw FileError
            return zen::NoValue();
        }
        catch (zen::FileError& error)
        {
            switch (cb.reportError(error.toString(), retryNumber)) //throw X
            {
                case ProcessCallback::IGNORE_ERROR:
                    return error.toString();
                case ProcessCallback::RETRY:
                    break; //continue with loop
            }
        }
}


//manage statistics reporting for a single item of work
class ItemStatReporter
{
public:
    ItemStatReporter(int itemsExpected, int64_t bytesExpected, ProcessCallback& cb) :
        itemsExpected_(itemsExpected),
        bytesExpected_(bytesExpected),
        cb_(cb) {}

    ~ItemStatReporter()
    {
        const bool scopeFail = getUncaughtExceptionCount() > exeptionCount_;
        if (scopeFail)
            cb_.updateDataTotal(itemsReported_, bytesReported_); //=> unexpected increase of total workload
        else
            //update statistics to consider the real amount of data, e.g. more than the "file size" for ADS streams,
            //less for sparse and compressed files,  or file changed in the meantime!
            cb_.updateDataTotal(itemsReported_ - itemsExpected_, bytesReported_ - bytesExpected_); //noexcept!
    }

    void reportStatus(const std::wstring& text) { cb_.reportStatus(text); } //throw X

    void reportDelta(int itemsDelta, int64_t bytesDelta) //throw X
    {
        cb_.updateDataProcessed(itemsDelta, bytesDelta); //nothrow!
        itemsReported_ += itemsDelta;
        bytesReported_ += bytesDelta;

        //special rule: avoid temporary statistics mess up, even though they are corrected anyway below:
        if (itemsReported_ > itemsExpected_)
        {
            cb_.updateDataTotal(itemsReported_ - itemsExpected_, 0);
            itemsReported_ = itemsExpected_;
        }
        if (bytesReported_ > bytesExpected_)
        {
            cb_.updateDataTotal(0, bytesReported_ - bytesExpected_); //=> everything above "bytesExpected" adds to both "processed" and "total" data
            bytesReported_ = bytesExpected_;
        }

        cb_.requestUiRefresh(); //throw X
    }

private:
    int itemsReported_ = 0;
    int64_t bytesReported_ = 0;
    const int itemsExpected_;
    const int64_t bytesExpected_;
    ProcessCallback& cb_;
    const int exeptionCount_ = getUncaughtExceptionCount();
};
}

#endif //STATUS_HANDLER_IMPL_H_07682758976
