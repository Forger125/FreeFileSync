// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef TASKBAR_H_98170845709124456
#define TASKBAR_H_98170845709124456

#include <memory>
#include <wx/frame.h>

/*
Windows 7; show progress in windows superbar via ITaskbarList3 Interface: https://msdn.microsoft.com/en-us/library/dd391692

Ubuntu: use Unity interface (optional)

Define HAVE_UBUNTU_UNITY and set:
    Compiler flag: `pkg-config --cflags unity`
    Linker   flag: `pkg-config --libs unity`
*/

namespace fff
{
class TaskbarNotAvailable {};

class Taskbar
{
public:
    Taskbar(const wxFrame& window); //throw TaskbarNotAvailable
    ~Taskbar();

    enum Status
    {
        STATUS_INDETERMINATE,
        STATUS_NORMAL,
        STATUS_ERROR,
        STATUS_PAUSED
    };

    void setStatus(Status status); //noexcept
    void setProgress(double fraction); //between [0, 1]; noexcept

private:
    class Impl;
    const std::unique_ptr<Impl> pimpl_;
};

}

#endif //TASKBAR_H_98170845709124456
