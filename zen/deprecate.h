// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef DEPRECATE_H_234897087787348
#define DEPRECATE_H_234897087787348

//compiler macros: http://predef.sourceforge.net/precomp.html
#ifdef __GNUC__
    #define ZEN_DEPRECATE __attribute__ ((deprecated))

#else
    #error add your platform here!
#endif

#endif //DEPRECATE_H_234897087787348
