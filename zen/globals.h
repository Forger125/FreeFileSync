// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef GLOBALS_H_8013740213748021573485
#define GLOBALS_H_8013740213748021573485

#include <atomic>
#include <memory>
#include "scope_guard.h"


namespace zen
{
//solve static destruction order fiasco by providing shared ownership and serialized access to global variables
template <class T>
class Global
{
public:
    Global()
    {
        static_assert(std::is_trivially_destructible<Pod>::value, "this memory needs to live forever");
        assert(!pod_.inst && !pod_.spinLock); //we depend on static zero-initialization!
    }
    explicit Global(std::unique_ptr<T>&& newInst) { set(std::move(newInst)); }
    ~Global() { set(nullptr); }

    std::shared_ptr<T> get() //=> return std::shared_ptr to let instance life time be handled by caller (MT usage!)
    {
        while (pod_.spinLock.exchange(true)) ;
        ZEN_ON_SCOPE_EXIT(pod_.spinLock = false);
        if (pod_.inst)
            return *pod_.inst;
        return nullptr;
    }

    void set(std::unique_ptr<T>&& newInst)
    {
        std::shared_ptr<T>* tmpInst = nullptr;
        if (newInst)
            tmpInst = new std::shared_ptr<T>(std::move(newInst));
        {
            while (pod_.spinLock.exchange(true)) ;
            ZEN_ON_SCOPE_EXIT(pod_.spinLock = false);
            std::swap(pod_.inst, tmpInst);
        }
        delete tmpInst;
    }

private:
    //avoid static destruction order fiasco: there may be accesses to "Global<T>::get()" during process shutdown
    //e.g. _("") used by message in debug_minidump.cpp or by some detached thread assembling an error message!
    //=> use trivially-destructible POD only!!!
    struct Pod
    {
        std::shared_ptr<T>* inst;   // = nullptr;
        std::atomic<bool> spinLock; // { false }; rely entirely on static zero-initialization! => avoid potential contention with worker thread during Global<> construction!
        //serialize access; can't use std::mutex: has non-trival destructor
    } pod_;
};

}

#endif //GLOBALS_H_8013740213748021573485
