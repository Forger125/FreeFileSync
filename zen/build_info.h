// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef BUILD_INFO_H_5928539285603428657
#define BUILD_INFO_H_5928539285603428657


#define ZEN_ARCH_32BIT 32
#define ZEN_ARCH_64BIT 64

    #ifdef __LP64__
        #define ZEN_BUILD_ARCH ZEN_ARCH_64BIT
    #else
        #define ZEN_BUILD_ARCH ZEN_ARCH_32BIT
    #endif

static_assert(ZEN_BUILD_ARCH == sizeof(void*) * 8);


namespace zen
{
    #if ZEN_BUILD_ARCH == ZEN_ARCH_32BIT
        const char cpuArchName[] = "i686";
    #else
        const char cpuArchName[] = "x86-64";
    #endif

}

#endif //BUILD_INFO_H_5928539285603428657
