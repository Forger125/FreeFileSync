// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef BINARY_H_3941281398513241134
#define BINARY_H_3941281398513241134

#include "../fs/abstract.h"


namespace fff
{
bool filesHaveSameContent(const AbstractPath& filePath1, //throw FileError, X
                          const AbstractPath& filePath2,
                          const zen::IOCallback& notifyUnbufferedIO  /*throw X*/);
}

#endif //BINARY_H_3941281398513241134
