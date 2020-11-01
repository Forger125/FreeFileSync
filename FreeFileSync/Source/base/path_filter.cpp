// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "path_filter.h"
#include <set>
#include <stdexcept>
#include <vector>
#include <typeinfo>
#include <iterator>
#include <typeindex>

using namespace zen;
using namespace fff;


std::strong_ordering fff::operator<=>(const FilterRef& lhs, const FilterRef& rhs)
{
    //caveat: typeid returns static type for pointers, dynamic type for references!!!
    if (const std::strong_ordering cmp = std::type_index(typeid(lhs.ref())) <=>
                                         std::type_index(typeid(rhs.ref()));
        std::is_neq(cmp))
        return cmp;

    return lhs.ref().compareSameType(rhs.ref());
}


std::vector<Zstring> fff::splitByDelimiter(const Zstring& filterPhrase)
{
    //delimiters may be FILTER_ITEM_SEPARATOR or '\n'
    std::vector<Zstring> output;

    for (const Zstring& str : split(filterPhrase, FILTER_ITEM_SEPARATOR, SplitOnEmpty::skip)) //split by less common delimiter first (create few, large strings)
        for (Zstring entry : split(str, Zstr('\n'), SplitOnEmpty::skip))
        {
            trim(entry);
            if (!entry.empty())
                output.push_back(std::move(entry));
        }

    return output;
}


namespace
{
void parseFilterPhrase(const Zstring& filterPhrase, std::vector<Zstring>& masksFileFolder, std::vector<Zstring>& masksFolder)
{
    const Zstring sepAsterisk = Zstr("/*");
    const Zstring asteriskSep = Zstr("*/");
    static_assert(FILE_NAME_SEPARATOR == '/');

    auto processTail = [&](const Zstring& phrase)
    {
        if (endsWith(phrase, FILE_NAME_SEPARATOR) || //only relevant for folder filtering
            endsWith(phrase, sepAsterisk)) // abc\*
        {
            const Zstring dirPhrase = beforeLast(phrase, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
            if (!dirPhrase.empty())
                masksFolder.push_back(dirPhrase);
        }
        else if (!phrase.empty())
            masksFileFolder.push_back(phrase);
    };

    for (const Zstring& itemPhrase : splitByDelimiter(filterPhrase))
    {
        //normalize filter input: 1. ignore Unicode normalization form 2. ignore case 3. ignore path separator
        Zstring phraseFmt = getUpperCase(itemPhrase);
        if constexpr (FILE_NAME_SEPARATOR != Zstr('/' )) replace(phraseFmt, Zstr('/'),  FILE_NAME_SEPARATOR);
        if constexpr (FILE_NAME_SEPARATOR != Zstr('\\')) replace(phraseFmt, Zstr('\\'), FILE_NAME_SEPARATOR);
        /*    phrase  | action
            +---------+--------
            | \blah   | remove \
            | \*blah  | remove \
            | \*\blah | remove \
            | \*\*    | remove \
            +---------+--------
            | *blah   |
            | *\blah  | -> add blah
            | *\*blah | -> add *blah
            +---------+--------
            | blah\   | remove \; folder only
            | blah*\  | remove \; folder only
            | blah\*\ | remove \; folder only
            +---------+--------
            | blah*   |
            | blah\*  | remove \*; folder only
            | blah*\* | remove \*; folder only
            +---------+--------                    */

        if (startsWith(phraseFmt, FILE_NAME_SEPARATOR)) // \abc
            processTail(afterFirst(phraseFmt, FILE_NAME_SEPARATOR, IfNotFoundReturn::none));
        else
        {
            processTail(phraseFmt);
            if (startsWith(phraseFmt, asteriskSep)) // *\abc
                processTail(afterFirst(phraseFmt, asteriskSep, IfNotFoundReturn::none));
        }
    }
}


template <class Char> inline
const Char* cStringFind(const Char* str, Char ch) //= strchr(), wcschr()
{
    for (;;)
    {
        const Char s = *str;
        if (s == ch) //ch is allowed to be 0 by contract! must return end of string in this case
            return str;

        if (s == 0)
            return nullptr;
        ++str;
    }
}


/*  struct FullMatch
    {
        static bool matchesMaskEnd (const Zchar* path) { return *path == 0; }
        static bool matchesMaskStar(const Zchar* path) { return true; }
    };                                                                         */

struct ParentFolderMatch //strict match of parent folder path!
{
    static bool matchesMaskEnd (const Zchar* path) { return *path == FILE_NAME_SEPARATOR; }
    static bool matchesMaskStar(const Zchar* path) { return cStringFind(path, FILE_NAME_SEPARATOR) != nullptr; }
};

struct AnyMatch
{
    static bool matchesMaskEnd (const Zchar* path) { return *path == 0 || *path == FILE_NAME_SEPARATOR; }
    static bool matchesMaskStar(const Zchar* path) { return true; }
};


template <class PathEndMatcher>
bool matchesMask(const Zchar* path, const Zchar* mask)
{
    for (;; ++mask, ++path)
    {
        Zchar m = *mask;
        switch (m)
        {
            case 0:
                return PathEndMatcher::matchesMaskEnd(path);

            case Zstr('?'):
                if (*path == 0)
                    return false;
                break;

            case Zstr('*'):
                do //advance mask to next non-* char
                {
                    m = *++mask;
                }
                while (m == Zstr('*'));

                if (m == 0) //mask ends with '*':
                    return PathEndMatcher::matchesMaskStar(path);

                //*? - pattern
                if (m == Zstr('?'))
                {
                    ++mask;
                    while (*path++ != 0)
                        if (matchesMask<PathEndMatcher>(path, mask))
                            return true;
                    return false;
                }

                //*[letter] - pattern
                ++mask;
                for (;;)
                {
                    path = cStringFind(path, m);
                    if (!path)
                        return false;

                    ++path;
                    if (matchesMask<PathEndMatcher>(path, mask))
                        return true;
                }

            default:
                if (*path != m)
                    return false;
        }
    }
}


//returns true if string matches at least the beginning of mask
inline
bool matchesMaskBegin(const Zchar* str, const Zchar* mask)
{
    for (;; ++mask, ++str)
    {
        const Zchar m = *mask;
        switch (m)
        {
            case 0:
                return *str == 0;

            case Zstr('?'):
                if (*str == 0)
                    return true;
                break;

            case Zstr('*'):
                return true;

            default:
                if (*str != m)
                    return *str == 0;
        }
    }
}


template <class PathEndMatcher> inline
bool matchesMask(const Zstring& name, const std::vector<Zstring>& masks)
{
    return std::any_of(masks.begin(), masks.end(), [&](const Zstring& mask) { return matchesMask<PathEndMatcher>(name.c_str(), mask.c_str()); });
}


inline
bool matchesMaskBegin(const Zstring& name, const std::vector<Zstring>& masks)
{
    return std::any_of(masks.begin(), masks.end(), [&](const Zstring& mask) { return matchesMaskBegin(name.c_str(), mask.c_str()); });
}
}

//#################################################################################################

NameFilter::NameFilter(const Zstring& includePhrase, const Zstring& excludePhrase)
{
    //setup include/exclude filters for files and directories
    parseFilterPhrase(includePhrase, includeMasksFileFolder, includeMasksFolder);
    parseFilterPhrase(excludePhrase, excludeMasksFileFolder, excludeMasksFolder);

    removeDuplicates(includeMasksFileFolder);
    removeDuplicates(includeMasksFolder    );
    removeDuplicates(excludeMasksFileFolder);
    removeDuplicates(excludeMasksFolder    );
}


void NameFilter::addExclusion(const Zstring& excludePhrase)
{
    parseFilterPhrase(excludePhrase, excludeMasksFileFolder, excludeMasksFolder);

    removeDuplicates(excludeMasksFileFolder);
    removeDuplicates(excludeMasksFolder    );
}


bool NameFilter::passFileFilter(const Zstring& relFilePath) const
{
    assert(!startsWith(relFilePath, FILE_NAME_SEPARATOR));

    //normalize input: 1. ignore Unicode normalization form 2. ignore case
    const Zstring& pathFmt = getUpperCase(relFilePath);

    if (matchesMask<AnyMatch         >(pathFmt, excludeMasksFileFolder) || //either full match on file or partial match on any parent folder
        matchesMask<ParentFolderMatch>(pathFmt, excludeMasksFolder)) //partial match on any parent folder only
        return false;

    return matchesMask<AnyMatch         >(pathFmt, includeMasksFileFolder) ||
           matchesMask<ParentFolderMatch>(pathFmt, includeMasksFolder);
}


bool NameFilter::passDirFilter(const Zstring& relDirPath, bool* childItemMightMatch) const
{
    assert(!startsWith(relDirPath, FILE_NAME_SEPARATOR));
    assert(!childItemMightMatch || *childItemMightMatch); //check correct usage

    //normalize input: 1. ignore Unicode normalization form 2. ignore case
    const Zstring& pathFmt = getUpperCase(relDirPath);

    if (matchesMask<AnyMatch>(pathFmt, excludeMasksFileFolder) ||
        matchesMask<AnyMatch>(pathFmt, excludeMasksFolder))
    {
        if (childItemMightMatch)
            *childItemMightMatch = false; //perf: no need to traverse deeper; subfolders/subfiles would be excluded by filter anyway!

        /* Attention: the design choice that "childItemMightMatch" is optional implies that the filter must provide correct results no matter if this
           value is considered by the client!
           In particular, if *childItemMightMatch == false, then any filter evaluations for child items must also return "false"!
           This is not a problem for folder traversal which stops at the first *childItemMightMatch == false anyway, but other code continues recursing further,
           e.g. the database update code in db_file.cpp recurses unconditionally without filter check! It's possible to construct edge cases with incorrect
           behavior if "childItemMightMatch" were not optional:
               1. two folders including a subfolder with some files are in sync with up-to-date database files
               2. deny access to this subfolder on both sides and start sync ignoring errors
               3. => database entries of this subfolder are incorrectly deleted! (if sub-folder is excluded, but child items are not!)           */
        return false;
    }

    if (matchesMask<AnyMatch>(pathFmt, includeMasksFileFolder) ||
        matchesMask<AnyMatch>(pathFmt, includeMasksFolder))
        return true;

    if (childItemMightMatch)
    {
        const Zstring& childPathBegin = pathFmt + FILE_NAME_SEPARATOR;

        *childItemMightMatch = matchesMaskBegin(childPathBegin, includeMasksFileFolder) || //might match a file  or folder in subdirectory
                               matchesMaskBegin(childPathBegin, includeMasksFolder );      //
    }
    return false;
}


bool NameFilter::isNull(const Zstring& includePhrase, const Zstring& excludePhrase)
{
    const Zstring include = trimCpy(includePhrase);
    const Zstring exclude = trimCpy(excludePhrase);

    return include == Zstr("*") && exclude.empty();
    //return NameFilter(includePhrase, excludePhrase).isNull(); -> very expensive for huge lists
}


bool NameFilter::isNull() const
{
    return includeMasksFileFolder.size() == 1 && includeMasksFileFolder[0] == Zstr("*") &&
           includeMasksFolder    .empty() &&
           excludeMasksFileFolder.empty() &&
           excludeMasksFolder    .empty();
    //avoid static non-POD null-NameFilter instance; instead test manually and verify function on startup:
}


std::strong_ordering NameFilter::compareSameType(const PathFilter& other) const
{
    assert(typeid(*this) == typeid(other)); //always given in this context!

    const NameFilter& lhs = *this;
    const NameFilter& rhs = static_cast<const NameFilter&>(other);

    return std::tie(lhs.includeMasksFileFolder, lhs.includeMasksFolder, lhs.excludeMasksFileFolder, lhs.excludeMasksFolder) <=>
           std::tie(rhs.includeMasksFileFolder, rhs.includeMasksFolder, rhs.excludeMasksFileFolder, rhs.excludeMasksFolder);
}
