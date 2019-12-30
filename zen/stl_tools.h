// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef STL_TOOLS_H_84567184321434
#define STL_TOOLS_H_84567184321434

#include <set>
#include <map>
#include <vector>
#include <memory>
#include <cassert>
#include <algorithm>
#include <optional>
#include "string_traits.h"
//#include "build_info.h"


//enhancements for <algorithm>
namespace zen
{
//erase selected elements from any container:
template <class T, class Alloc, class Predicate>
void eraseIf(std::vector<T, Alloc>& v, Predicate p);

template <class T, class LessType, class Alloc, class Predicate>
void eraseIf(std::set<T, LessType, Alloc>& s, Predicate p);

template <class KeyType, class ValueType, class LessType, class Alloc, class Predicate>
void eraseIf(std::map<KeyType, ValueType, LessType, Alloc>& m, Predicate p);

//append STL containers
template <class T, class Alloc, class C>
void append(std::vector<T, Alloc>& v, const C& c);

template <class T, class LessType, class Alloc, class C>
void append(std::set<T, LessType, Alloc>& s, const C& c);

template <class KeyType, class ValueType, class LessType, class Alloc, class C>
void append(std::map<KeyType, ValueType, LessType, Alloc>& m, const C& c);

template <class T, class Alloc>
void removeDuplicates(std::vector<T, Alloc>& v);

template <class T, class Alloc, class CompLess>
void removeDuplicates(std::vector<T, Alloc>& v, CompLess less);

//binary search returning an iterator
template <class Iterator, class T, class CompLess>
Iterator binarySearch(Iterator first, Iterator last, const T& value, CompLess less);

template <class BidirectionalIterator, class T>
BidirectionalIterator findLast(BidirectionalIterator first, BidirectionalIterator last, const T& value);

//replacement for std::find_end taking advantage of bidirectional iterators (and giving the algorithm a reasonable name)
template <class BidirectionalIterator1, class BidirectionalIterator2>
BidirectionalIterator1 searchLast(BidirectionalIterator1 first1, BidirectionalIterator1 last1,
                                   BidirectionalIterator2 first2, BidirectionalIterator2 last2);

template <class Num, class ByteIterator> Num hashBytes                   (ByteIterator first, ByteIterator last);
template <class Num, class ByteIterator> Num hashBytesAppend(Num hashVal, ByteIterator first, ByteIterator last);

//support for custom string classes in std::unordered_set/map
struct StringHash
{
    template <class String>
    size_t operator()(const String& str) const
    {
        const auto* strFirst = strBegin(str);
        return hashBytes<size_t>(reinterpret_cast<const char*>(strFirst),
                                 reinterpret_cast<const char*>(strFirst + strLength(str)));
    }
};


//why, oh why is there no std::optional<T>::get()???
template <class T> inline       T* get(      std::optional<T>& opt) { return opt ? &*opt : nullptr; }
template <class T> inline const T* get(const std::optional<T>& opt) { return opt ? &*opt : nullptr; }



//===========================================================================
template <class T> class SharedRef;
template <class T, class... Args> SharedRef<T> makeSharedRef(Args&& ... args);

template <class T>
class SharedRef //why is there no std::shared_ref???
{
public:
    SharedRef() = delete; //no suprise memory allocations => always construct with makeSharedRef()

    template <class U>
    SharedRef(const SharedRef<U>& other) : ref_(other.ref_) {}

    /**/  T& ref()       { return *ref_; };
    const T& ref() const { return *ref_; };

    std::shared_ptr<T> ptr() { return ref_; };

private:
    explicit SharedRef(std::shared_ptr<T>&& ptr) : ref_(std::move(ptr)) { assert(ref_); }

    template <class U, class... Args> friend SharedRef<U> makeSharedRef(Args&& ... args);
    template <class U> friend class SharedRef;

    std::shared_ptr<T> ref_; //always bound
};

template <class T, class... Args> inline
SharedRef<T> makeSharedRef(Args&&... args) { return SharedRef<T>(std::make_shared<T>(std::forward<Args>(args)...)); }
//===========================================================================



//######################## implementation ########################

template <class T, class Alloc, class Predicate> inline
void eraseIf(std::vector<T, Alloc>& v, Predicate p)
{
    v.erase(std::remove_if(v.begin(), v.end(), p), v.end());
}


namespace impl
{
template <class S, class Predicate> inline
void setOrMapEraseIf(S& s, Predicate p)
{
    for (auto it = s.begin(); it != s.end();)
        if (p(*it))
            s.erase(it++);
        else
            ++it;
}
}


template <class T, class LessType, class Alloc, class Predicate> inline
void eraseIf(std::set<T, LessType, Alloc>& s, Predicate p) { impl::setOrMapEraseIf(s, p); } //don't make this any more generic! e.g. must not compile for std::vector!!!


template <class KeyType, class ValueType, class LessType, class Alloc, class Predicate> inline
void eraseIf(std::map<KeyType, ValueType, LessType, Alloc>& m, Predicate p) { impl::setOrMapEraseIf(m, p); }


template <class T, class Alloc, class C> inline
void append(std::vector<T, Alloc>& v, const C& c) { v.insert(v.end(), c.begin(), c.end()); }


template <class T, class LessType, class Alloc, class C> inline
void append(std::set<T, LessType, Alloc>& s, const C& c) { s.insert(c.begin(), c.end()); }


template <class KeyType, class ValueType, class LessType, class Alloc, class C> inline
void append(std::map<KeyType, ValueType, LessType, Alloc>& m, const C& c) { m.insert(c.begin(), c.end()); }


template <class T, class Alloc, class CompLess, class CompEqual> inline
void removeDuplicates(std::vector<T, Alloc>& v, CompLess less, CompEqual eq)
{
    std::sort(v.begin(), v.end(), less);
    v.erase(std::unique(v.begin(), v.end(), eq), v.end());
}


template <class T, class Alloc, class CompLess> inline
void removeDuplicates(std::vector<T, Alloc>& v, CompLess less)
{
    removeDuplicates(v, less, [&](const auto& lhs, const auto& rhs) { return !less(lhs, rhs) && !less(rhs, lhs); });
}


template <class T, class Alloc> inline
void removeDuplicates(std::vector<T, Alloc>& v)
{
    removeDuplicates(v, std::less<T>(), std::equal_to<T>());
}


template <class Iterator, class T, class CompLess> inline
Iterator binarySearch(Iterator first, Iterator last, const T& value, CompLess less)
{
    static_assert(std::is_same_v<typename std::iterator_traits<Iterator>::iterator_category, std::random_access_iterator_tag>);

    first = std::lower_bound(first, last, value, less);
    if (first != last && !less(value, *first))
        return first;
    else
        return last;
}


template <class BidirectionalIterator, class T> inline
BidirectionalIterator findLast(const BidirectionalIterator first, const BidirectionalIterator last, const T& value)
{
    for (BidirectionalIterator it = last; it != first;) //reverse iteration: 1. check 2. decrement 3. evaluate
    {
        --it; //

        if (*it == value)
            return it;
    }
    return last;
}


template <class BidirectionalIterator1, class BidirectionalIterator2> inline
BidirectionalIterator1 searchLast(const BidirectionalIterator1 first1,       BidirectionalIterator1 last1,
                                   const BidirectionalIterator2 first2, const BidirectionalIterator2 last2)
{
    const BidirectionalIterator1 itNotFound = last1;

    //reverse iteration: 1. check 2. decrement 3. evaluate
    for (;;)
    {
        BidirectionalIterator1 it1 = last1;
        BidirectionalIterator2 it2 = last2;

        for (;;)
        {
            if (it2 == first2) return it1;
            if (it1 == first1) return itNotFound;

            --it1;
            --it2;

            if (*it1 != *it2) break;
        }
        --last1;
    }
}


//FNV-1a: http://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
template <class Num, class ByteIterator> inline
Num hashBytes(ByteIterator first, ByteIterator last)
{
    static_assert(IsInteger<Num>::value);
    static_assert(sizeof(Num) == 4 || sizeof(Num) == 8); //macOS: size_t is "unsigned long"
    constexpr Num base = sizeof(Num) == 4 ? 2166136261U : 14695981039346656037ULL;

    return hashBytesAppend(base, first, last);
}


template <class Num, class ByteIterator> inline
Num hashBytesAppend(Num hashVal, ByteIterator first, ByteIterator last)
{
    static_assert(sizeof(typename std::iterator_traits<ByteIterator>::value_type) == 1);
    constexpr Num prime = sizeof(Num) == 4 ? 16777619U : 1099511628211ULL;

    for (; first != last; ++first)
    {
        hashVal ^= static_cast<Num>(*first);
        hashVal *= prime;
    }
    return hashVal;
}
}

#endif //STL_TOOLS_H_84567184321434
