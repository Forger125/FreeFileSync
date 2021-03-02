// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef SYMLINK_TARGET_H_801783470198357483
#define SYMLINK_TARGET_H_801783470198357483

#include "scope_guard.h"
#include "file_error.h"

    #include <unistd.h>
    #include <stdlib.h> //realpath


namespace zen
{

struct SymlinkRawContent
{
    Zstring targetPath;
};
SymlinkRawContent getSymlinkRawContent(const Zstring& linkPath); //throw FileError

Zstring getSymlinkResolvedPath(const Zstring& linkPath); //throw FileError
}









//################################ implementation ################################

namespace
{
//retrieve raw target data of symlink or junction
zen::SymlinkRawContent getSymlinkRawContent_impl(const Zstring& linkPath) //throw FileError
{
    using namespace zen;
    const size_t BUFFER_SIZE = 10000;
    std::vector<char> buffer(BUFFER_SIZE);

    const ssize_t bytesWritten = ::readlink(linkPath.c_str(), &buffer[0], BUFFER_SIZE);
    if (bytesWritten < 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(linkPath)), "readlink");
    if (bytesWritten >= static_cast<ssize_t>(BUFFER_SIZE)) //detect truncation; not an error for readlink!
        throw FileError(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(linkPath)), formatSystemError("readlink", L"", L"Buffer truncated."));

    return {Zstring(&buffer[0], bytesWritten)}; //readlink does not append 0-termination!
}


Zstring getResolvedSymlinkPath_impl(const Zstring& linkPath) //throw FileError
{
    using namespace zen;
    char* targetPath = ::realpath(linkPath.c_str(), nullptr);
    if (!targetPath)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot determine final path for %x."), L"%x", fmtPath(linkPath)), "realpath");
    ZEN_ON_SCOPE_EXIT(::free(targetPath));
    return targetPath;
}
}


namespace zen
{
inline
SymlinkRawContent getSymlinkRawContent(const Zstring& linkPath) { return getSymlinkRawContent_impl(linkPath); } //throw FileError


inline
Zstring getSymlinkResolvedPath(const Zstring& linkPath) { return getResolvedSymlinkPath_impl(linkPath); } //throw FileError

}

#endif //SYMLINK_TARGET_H_801783470198357483
