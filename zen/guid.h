// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef GUID_H_80425780237502345
#define GUID_H_80425780237502345

#include <string>
    #include <boost/uuid/uuid_generators.hpp>

namespace zen
{
inline
std::string generateGUID() //creates a 16-byte GUID
{
    //perf: generator:      0.38ms per creation;
    //      retrieve GUID:  0.13µs per call
    //generator is only thread-safe like an int => keep thread-local
    thread_local boost::uuids::random_generator gen;
    static_assert(boost::uuids::uuid::static_size() == 16);
    const boost::uuids::uuid nativeRep = gen();
    return std::string(nativeRep.begin(), nativeRep.end());
}
}

#endif //GUID_H_80425780237502345
