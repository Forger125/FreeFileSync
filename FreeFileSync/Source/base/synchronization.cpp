﻿// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "synchronization.h"
#include <tuple>
#include <zen/process_priority.h>
#include <zen/perf.h>
#include <zen/guid.h>
#include <zen/crc.h>
#include "algorithm.h"
#include "db_file.h"
#include "status_handler_impl.h"
#include "versioning.h"
#include "binary.h"
#include "../afs/concrete.h"
#include "../afs/native.h"

    #include <unistd.h> //fsync
    #include <fcntl.h>  //open

using namespace zen;
using namespace fff;


namespace
{
const size_t CONFLICTS_PREVIEW_MAX = 25; //=> consider memory consumption, log file size, email size!


}


SyncStatistics::SyncStatistics(const FolderComparison& folderCmp)
{
    std::for_each(begin(folderCmp), end(folderCmp), [&](const BaseFolderPair& baseFolder) { recurse(baseFolder); });
}


SyncStatistics::SyncStatistics(const ContainerObject& hierObj)
{
    recurse(hierObj);
}


SyncStatistics::SyncStatistics(const FilePair& file)
{
    processFile(file);
    ++rowsTotal_;
}


inline
void SyncStatistics::recurse(const ContainerObject& hierObj)
{
    for (const FilePair& file : hierObj.refSubFiles())
        processFile(file);
    for (const SymlinkPair& symlink : hierObj.refSubLinks())
        processLink(symlink);
    for (const FolderPair& folder : hierObj.refSubFolders())
        processFolder(folder);

    rowsTotal_ += hierObj.refSubFolders().size();
    rowsTotal_ += hierObj.refSubFiles  ().size();
    rowsTotal_ += hierObj.refSubLinks  ().size();
}


inline
void SyncStatistics::processFile(const FilePair& file)
{
    switch (file.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<SelectSide::right>());
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<SelectSide::left>());
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            break;

        case SO_MOVE_LEFT_TO:
            ++updateLeft_;
            break;

        case SO_MOVE_RIGHT_TO:
            ++updateRight_;
            break;

        case SO_MOVE_LEFT_FROM:  //ignore; already counted
        case SO_MOVE_RIGHT_FROM: //=> harmonize with FileView::applyActionFilter()
            break;

        case SO_OVERWRITE_LEFT:
            ++updateLeft_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<SelectSide::right>());
            break;

        case SO_OVERWRITE_RIGHT:
            ++updateRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<SelectSide::left>());
            break;

        case SO_UNRESOLVED_CONFLICT:
            ++conflictCount_;
            if (conflictsPreview_.size() < CONFLICTS_PREVIEW_MAX)
                conflictsPreview_.push_back({file.getRelativePathAny(), file.getSyncOpConflict()});
            break;

        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            break;

        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            break;

        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }
}


inline
void SyncStatistics::processLink(const SymlinkPair& symlink)
{
    switch (symlink.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            break;

        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            break;

        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            break;

        case SO_UNRESOLVED_CONFLICT:
            ++conflictCount_;
            if (conflictsPreview_.size() < CONFLICTS_PREVIEW_MAX)
                conflictsPreview_.push_back({symlink.getRelativePathAny(), symlink.getSyncOpConflict()});
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
            break;
        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }
}


inline
void SyncStatistics::processFolder(const FolderPair& folder)
{
    switch (folder.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            break;

        case SO_DELETE_LEFT: //if deletion variant == versioning with user-defined directory existing on other volume, this results in a full copy + delete operation!
            ++deleteLeft_;    //however we cannot (reliably) anticipate this situation, fortunately statistics can be adapted during sync!
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            break;

        case SO_UNRESOLVED_CONFLICT:
            ++conflictCount_;
            if (conflictsPreview_.size() < CONFLICTS_PREVIEW_MAX)
                conflictsPreview_.push_back({folder.getRelativePathAny(), folder.getSyncOpConflict()});
            break;

        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            break;

        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
            [[fallthrough]];
        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }

    recurse(folder); //since we model logical stats, we recurse, even if deletion variant is "recycler" or "versioning + same volume", which is a single physical operation!
}


/*    DeletionVariant::permanent:  deletion frees space
      DeletionVariant::recycler:   won't free space until recycler is full, but then frees space
      DeletionVariant::versioning: depends on whether versioning folder is on a different volume
    -> if deleted item is a followed symlink, no space is freed
    -> created/updated/deleted item may be on a different volume than base directory: consider symlinks, junctions!

    => generally assume deletion frees space; may avoid false-positive disk space warnings for recycler and versioning   */
class MinimumDiskSpaceNeeded
{
public:
    static std::pair<int64_t, int64_t> calculate(const BaseFolderPair& baseFolder)
    {
        MinimumDiskSpaceNeeded inst;
        inst.recurse(baseFolder);
        return {inst.spaceNeededLeft_, inst.spaceNeededRight_};
    }

private:
    void recurse(const ContainerObject& hierObj)
    {
        //process files
        for (const FilePair& file : hierObj.refSubFiles())
            switch (file.getSyncOperation()) //evaluate comparison result and sync direction
            {
                case SO_CREATE_NEW_LEFT:
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<SelectSide::right>());
                    break;

                case SO_CREATE_NEW_RIGHT:
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<SelectSide::left>());
                    break;

                case SO_DELETE_LEFT:
                    if (!file.isFollowedSymlink<SelectSide::left>())
                        spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<SelectSide::left>());
                    break;

                case SO_DELETE_RIGHT:
                    if (!file.isFollowedSymlink<SelectSide::right>())
                        spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<SelectSide::right>());
                    break;

                case SO_OVERWRITE_LEFT:
                    if (!file.isFollowedSymlink<SelectSide::left>())
                        spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<SelectSide::left>());
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<SelectSide::right>());
                    break;

                case SO_OVERWRITE_RIGHT:
                    if (!file.isFollowedSymlink<SelectSide::right>())
                        spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<SelectSide::right>());
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<SelectSide::left>());
                    break;

                case SO_DO_NOTHING:
                case SO_EQUAL:
                case SO_UNRESOLVED_CONFLICT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                    break;
            }

        //symbolic links
        //[...]

        //recurse into sub-dirs
        for (const FolderPair& folder : hierObj.refSubFolders())
            switch (folder.getSyncOperation())
            {
                case SO_DELETE_LEFT:
                    if (!folder.isFollowedSymlink<SelectSide::left>())
                        recurse(folder); //not 100% correct: in fact more that what our model contains may be deleted (consider file filter!)
                    break;
                case SO_DELETE_RIGHT:
                    if (!folder.isFollowedSymlink<SelectSide::right>())
                        recurse(folder);
                    break;

                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                    assert(false);
                    [[fallthrough]];
                case SO_CREATE_NEW_LEFT:
                case SO_CREATE_NEW_RIGHT:
                case SO_OVERWRITE_LEFT:
                case SO_OVERWRITE_RIGHT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                case SO_DO_NOTHING:
                case SO_EQUAL:
                case SO_UNRESOLVED_CONFLICT:
                    recurse(folder); //not 100% correct: what if left or right folder is symlink!? => file operations may happen on different volume!
                    break;
            }
    }

    int64_t spaceNeededLeft_  = 0;
    int64_t spaceNeededRight_ = 0;
};

//-----------------------------------------------------------------------------------------------------------

std::vector<FolderPairSyncCfg> fff::extractSyncCfg(const MainConfiguration& mainCfg)
{
    //merge first and additional pairs
    std::vector<LocalPairConfig> localCfgs = {mainCfg.firstPair};
    append(localCfgs, mainCfg.additionalPairs);

    std::vector<FolderPairSyncCfg> output;

    for (const LocalPairConfig& lpc : localCfgs)
    {
        //const CompConfig cmpCfg  = lpc.localCmpCfg  ? *lpc.localCmpCfg  : mainCfg.cmpCfg;
        const SyncConfig syncCfg = lpc.localSyncCfg ? *lpc.localSyncCfg : mainCfg.syncCfg;

        output.push_back(
        {
            syncCfg.directionCfg.var,
            syncCfg.directionCfg.var == SyncVariant::twoWay || detectMovedFilesEnabled(syncCfg.directionCfg),

            syncCfg.deletionVariant,
            syncCfg.versioningFolderPhrase,
            syncCfg.versioningStyle,
            syncCfg.versionMaxAgeDays,
            syncCfg.versionCountMin,
            syncCfg.versionCountMax
        });
    }
    return output;
}

//------------------------------------------------------------------------------------------------------------

namespace
{
inline
std::optional<SelectSide> getTargetDirection(SyncOperation syncOp)
{
    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_DELETE_LEFT:
        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_LEFT_TO:
            return SelectSide::left;

        case SO_CREATE_NEW_RIGHT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_RIGHT_TO:
            return SelectSide::right;

        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            break; //nothing to do
    }
    return {};
}


//test if user accidentally selected the wrong folders to sync
bool significantDifferenceDetected(const SyncStatistics& folderPairStat)
{
    //initial file copying shall not be detected as major difference
    if ((folderPairStat.createCount<SelectSide::left >() == 0 ||
         folderPairStat.createCount<SelectSide::right>() == 0) &&
        folderPairStat.updateCount  () == 0 &&
        folderPairStat.deleteCount  () == 0 &&
        folderPairStat.conflictCount() == 0)
        return false;

    const int nonMatchingRows = folderPairStat.createCount() +
                                folderPairStat.deleteCount();
    //folderPairStat.updateCount() +  -> not relevant when testing for "wrong folder selected"
    //folderPairStat.conflictCount();

    return nonMatchingRows >= 10 && nonMatchingRows > 0.5 * folderPairStat.rowCount();
}

//---------------------------------------------------------------------------------------------

template <SelectSide side>
bool plannedWriteAccess(const FileSystemObject& fsObj)
{
    if (std::optional<SelectSide> dir = getTargetDirection(fsObj.getSyncOperation()))
        return side == *dir;
    else
        return false;
}


inline
AbstractPath getAbstractPath(const FileSystemObject& fsObj, SelectSide side)
{
    return side == SelectSide::left ? fsObj.getAbstractPath<SelectSide::left>() : fsObj.getAbstractPath<SelectSide::right>();
}


struct PathRaceItem
{
    const FileSystemObject* fsObj;
    SelectSide side;

    std::strong_ordering operator<=>(const PathRaceItem&) const = default;
};


std::weak_ordering comparePathNoCase(const PathRaceItem& lhs, const PathRaceItem& rhs)
{
    const AbstractPath& itemPathL = getAbstractPath(*lhs.fsObj, lhs.side);
    const AbstractPath& itemPathR = getAbstractPath(*rhs.fsObj, rhs.side);

    if (const std::weak_ordering cmp = itemPathL.afsDevice <=> itemPathR.afsDevice;
        cmp != std::weak_ordering::equivalent)
        return cmp;

    return compareNoCase(itemPathL.afsPath.value, //no hashing: want natural sort order!
                         itemPathR.afsPath.value);
}


std::wstring formatRaceItem(const PathRaceItem& item)
{
    const std::optional<SelectSide> syncDir = getTargetDirection(item.fsObj->getSyncOperation());

    return AFS::getDisplayPath(item.side == SelectSide::left ?
                               item.fsObj->base().getAbstractPath<SelectSide::left>() :
                               item.fsObj->base().getAbstractPath<SelectSide::right>()) +
           (syncDir && *syncDir == item.side ? L" 💾 " : L" 👓 ") +
           utfTo<std::wstring>(item.side == SelectSide::left ?
                               item.fsObj->getRelativePath<SelectSide::left>() :
                               item.fsObj->getRelativePath<SelectSide::right>());
    //e.g.  D:\folder 💾 subfolder\file.txt
    //      D:\folder\subfolder 👓 file.txt
}


struct ChildPathRef
{
    const FileSystemObject* fsObj = nullptr;
    uint64_t afsPathHash = 0; //of *case-normalized* AfsPath
};


template <SelectSide side>
class GetChildItemsHashed
{
public:
    static std::vector<ChildPathRef> execute(const ContainerObject& folder)
    {
        FNV1aHash<uint64_t> pathHash;
        for (const Zstring& itemName : splitCpy<Zstring>(folder.getAbstractPath<side>().afsPath.value, FILE_NAME_SEPARATOR, SplitOnEmpty::skip))
            hashAdd(pathHash, itemName); //not really needed ATM, but it's cleaner to hash *full* afsPath

        GetChildItemsHashed inst;
        inst.recurse(folder, pathHash.get());
        return std::move(inst.childPathRefs_);
    }

private:
    GetChildItemsHashed           (const GetChildItemsHashed&) = delete;
    GetChildItemsHashed& operator=(const GetChildItemsHashed&) = delete;

    GetChildItemsHashed() {}

    void recurse(const ContainerObject& hierObj, uint64_t parentPathHash)
    {
        for (const FilePair& file : hierObj.refSubFiles())
            childPathRefs_.push_back({&file, getPathHash(file, parentPathHash)});
        //S1 -> T (update)   is not a conflict (anymore) if S1, S2 contain different files
        //S2 -> T (update)   https://freefilesync.org/forum/viewtopic.php?t=9365#p36466
        for (const SymlinkPair& symlink : hierObj.refSubLinks())
            childPathRefs_.push_back({&symlink, getPathHash(symlink, parentPathHash)});

        for (const FolderPair& subFolder : hierObj.refSubFolders())
        {
            const uint64_t folderPathHash = getPathHash(subFolder, parentPathHash);

            childPathRefs_.push_back({&subFolder, folderPathHash});

            recurse(subFolder, folderPathHash);
        }
    }

    static void hashAdd(FNV1aHash<uint64_t>& hash, const Zstring& itemName)
    {
        if (isAsciiString(itemName)) //fast path: no need for extra memory allocation!
            for (const Zchar c : itemName)
                hash.add(asciiToUpper(c));
        else
            for (const Zchar c : getUpperCase(itemName))
                hash.add(c);
    }

    static uint64_t getPathHash(const FileSystemObject& fsObj, uint64_t parentPathHash)
    {
        FNV1aHash<uint64_t> hash(parentPathHash);
        hashAdd(hash, fsObj.getItemName<side>());
        return hash.get();
    }

    std::vector<ChildPathRef> childPathRefs_;
};


template <SelectSide sideL, SelectSide sideR>
std::weak_ordering compareHashedPathNoCase(const ChildPathRef& lhs, const ChildPathRef& rhs)
{
    //assert(lhs.fsObj->getAbstractPath<sideL>().afsDevice ==         -> too slow, even for debug build
    //       rhs.fsObj->getAbstractPath<sideR>().afsDevice);

    if (const std::weak_ordering cmp = lhs.afsPathHash <=> rhs.afsPathHash;
        cmp != std::weak_ordering::equivalent)
        return cmp; //fast path!

    return compareNoCase(lhs.fsObj->getAbstractPath<sideL>().afsPath.value,  //fsObj may come from *different* BaseFolderPair
                         rhs.fsObj->getAbstractPath<sideR>().afsPath.value); // => don't compare getRelativePath()!
}


template <SelectSide side>
void sortAndRemoveDuplicates(std::vector<ChildPathRef>& pathRefs)
{
    std::sort(pathRefs.begin(), pathRefs.end(), [](const ChildPathRef& lhs, const ChildPathRef& rhs)
    {
        if (const std::weak_ordering cmp = compareHashedPathNoCase<side, side>(lhs, rhs);
            cmp != std::weak_ordering::equivalent)
            return cmp < 0;

        return //multiple (case-insensitive) relPaths? => order write-access before read-access, so that std::unique leaves a write if existing!
            plannedWriteAccess<side>(*lhs.fsObj) >
            plannedWriteAccess<side>(*rhs.fsObj);
    });

    pathRefs.erase(std::unique(pathRefs.begin(), pathRefs.end(),
    [](const ChildPathRef& lhs, const ChildPathRef& rhs) { return compareHashedPathNoCase<side, side>(lhs, rhs) == std::weak_ordering::equivalent; }),
    pathRefs.end());

    //let's not use removeDuplicates(): we rely too much on implementation details!
}


//check if some files/folders are included more than once and form a race condition (:= multiple accesses of which at least one is a write)
// - checking filter for subfolder exclusion is not good enough: one folder may have a *.txt include-filter, the other a *.lng include filter => still no dependencies
// - user may have manually excluded the conflicting items or changed the filter settings without running a re-compare
template <SelectSide sideP, SelectSide sideC>
void checkPathRaceCondition(const BaseFolderPair& baseFolderP, const BaseFolderPair& baseFolderC, std::vector<PathRaceItem>& pathRaceItems)
{
    const AbstractPath basePathP = baseFolderP.getAbstractPath<sideP>(); //parent/child notion is tentative at this point
    const AbstractPath basePathC = baseFolderC.getAbstractPath<sideC>(); //=> will be swapped if necessary

    if (!AFS::isNullPath(basePathP) && !AFS::isNullPath(basePathC))
        if (basePathP.afsDevice == basePathC.afsDevice)
        {
            if (basePathP.afsPath.value.size() > basePathC.afsPath.value.size())
                return checkPathRaceCondition<sideC, sideP>(baseFolderC, baseFolderP, pathRaceItems);

            const std::vector<Zstring> relPathP = splitCpy(basePathP.afsPath.value, FILE_NAME_SEPARATOR, SplitOnEmpty::skip);
            const std::vector<Zstring> relPathC = splitCpy(basePathC.afsPath.value, FILE_NAME_SEPARATOR, SplitOnEmpty::skip);

            if (relPathP.size() <= relPathC.size() &&
            /**/std::equal(relPathP.begin(), relPathP.end(), relPathC.begin(), [](const Zstring& lhs, const Zstring& rhs) { return equalNoCase(lhs, rhs); }))
            {
                //=> at this point parent/child folders are confirmed
                //now find child folder match inside baseFolderP
                //e.g.  C:\folder <-> C:\folder\sub    =>  find "sub" inside C:\folder
                std::vector<const ContainerObject*> childFolderP{&baseFolderP};

                std::for_each(relPathC.begin() + relPathP.size(), relPathC.end(), [&](const Zstring& itemName)
                {
                    std::vector<const ContainerObject*> childFolderP2;

                    for (const ContainerObject* childFolder : childFolderP)
                        for (const FolderPair& folder : childFolder->refSubFolders())
                            if (equalNoCase(folder.getItemName<sideP>(), itemName))
                                childFolderP2.push_back(&folder);
                    //no "break": yes, weird, but there could be more than one (for case-sensitive file system)

                    childFolderP = std::move(childFolderP2);
                });

                std::vector<ChildPathRef> pathRefsP;
                for (const ContainerObject* childFolder : childFolderP)
                    append(pathRefsP, GetChildItemsHashed<sideP>::execute(*childFolder));

                std::vector<ChildPathRef> pathRefsC = GetChildItemsHashed<sideC>::execute(baseFolderC);

                //---------------------------------------------------------------------------------------------------
                //case-sensitive comparison because items were scanned by FFS (=> no messy user input)?
                //not good enough! E.g. not-yet-existing files are set to be created with different case!
                // + (weird) a file and a folder are set to be created with same name
                // => (throw hands in the air) fine, check path only and don't consider case
                sortAndRemoveDuplicates<sideP>(pathRefsP);
                sortAndRemoveDuplicates<sideC>(pathRefsC);

                mergeTraversal(pathRefsP.begin(), pathRefsP.end(),
                               pathRefsC.begin(), pathRefsC.end(),
                [](const ChildPathRef&) {} /*left only*/,
                [&](const ChildPathRef& lhs, const ChildPathRef& rhs)
                {
                    if (plannedWriteAccess<sideP>(*lhs.fsObj) ||
                        plannedWriteAccess<sideC>(*rhs.fsObj))
                    {
                        pathRaceItems.push_back({lhs.fsObj, sideP});
                        pathRaceItems.push_back({rhs.fsObj, sideC});
                    }
                },
                [](const ChildPathRef&) {} /*right only*/, compareHashedPathNoCase<sideP, sideC>);
            }
        }
}

//#################################################################################################################

warn_static("review: does flushFileBuffers() make sense?")
//https://devblogs.microsoft.com/oldnewthing/20221007-00/?p=107261

//--------------------- data verification -------------------------
void flushFileBuffers(const Zstring& nativeFilePath) //throw FileError
{
    const int fdFile = ::open(nativeFilePath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fdFile == -1)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot open file %x."), L"%x", fmtPath(nativeFilePath)), "open");
    ZEN_ON_SCOPE_EXIT(::close(fdFile));

    if (::fsync(fdFile) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(nativeFilePath)), "fsync");
}


void verifyFiles(const AbstractPath& sourcePath, const AbstractPath& targetPath, const IoCallback& notifyUnbufferedIO /*throw X*/) //throw FileError, X
{
    try
    {
        //do like "copy /v": 1. flush target file buffers, 2. read again as usual (using OS buffers)
        // => it seems OS buffers are not invalidated by this: snake oil???
        if (const Zstring& targetPathNative = getNativeItemPath(targetPath);
            !targetPathNative.empty())
            flushFileBuffers(targetPathNative); //throw FileError

        if (!filesHaveSameContent(sourcePath, targetPath, notifyUnbufferedIO)) //throw FileError, X
            throw FileError(replaceCpy(replaceCpy(_("%x and %y have different content."),
                                                  L"%x", L'\n' + fmtPath(AFS::getDisplayPath(sourcePath))),
                                       L"%y", L'\n' + fmtPath(AFS::getDisplayPath(targetPath))));
    }
    catch (const FileError& e) //add some context to error message
    {
        throw FileError(_("Data verification error:"), e.toString());
    }
}

//#################################################################################################################
//#################################################################################################################

/* ________________________________________________________________
   |                                                              |
   | Multithreaded File Copy: Parallel API for expensive file I/O |
   |______________________________________________________________| */

namespace parallel
{
inline
AFS::ItemType getItemType(const AbstractPath& itemPath, std::mutex& singleThread) //throw FileError
{ return parallelScope([itemPath] { return AFS::getItemType(itemPath); /*throw FileError*/ }, singleThread); }

inline
bool itemExists(const AbstractPath& itemPath, std::mutex& singleThread) //throw FileError
{ return parallelScope([itemPath] { return AFS::itemExists(itemPath); /*throw FileError*/ }, singleThread); }

inline
void removeFileIfExists(const AbstractPath& filePath, std::mutex& singleThread) //throw FileError
{ parallelScope([filePath] { AFS::removeFileIfExists(filePath); /*throw FileError*/ }, singleThread); }

inline
void removeSymlinkIfExists(const AbstractPath& linkPath, std::mutex& singleThread) //throw FileError
{ parallelScope([linkPath] { AFS::removeSymlinkIfExists(linkPath); /*throw FileError*/ }, singleThread); }

inline
void moveAndRenameItem(const AbstractPath& pathFrom, const AbstractPath& pathTo, std::mutex& singleThread) //throw FileError, ErrorMoveUnsupported
{ parallelScope([pathFrom, pathTo] { AFS::moveAndRenameItem(pathFrom, pathTo); /*throw FileError, ErrorMoveUnsupported*/ }, singleThread); }

inline
AbstractPath getSymlinkResolvedPath(const AbstractPath& linkPath, std::mutex& singleThread) //throw FileError
{ return parallelScope([linkPath] { return AFS::getSymlinkResolvedPath(linkPath); /*throw FileError*/ }, singleThread); }

inline
void copySymlink(const AbstractPath& sourcePath, const AbstractPath& targetPath, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ parallelScope([sourcePath, targetPath, copyFilePermissions] { AFS::copySymlink(sourcePath, targetPath, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
AFS::FolderCopyResult copyNewFolder(const AbstractPath& sourcePath, const AbstractPath& targetPath, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ return parallelScope([sourcePath, targetPath, copyFilePermissions] { return AFS::copyNewFolder(sourcePath, targetPath, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
void removeFilePlain(const AbstractPath& filePath, std::mutex& singleThread) //throw FileError
{ parallelScope([filePath] { AFS::removeFilePlain(filePath); /*throw FileError*/ }, singleThread); }

//--------------------------------------------------------------
//ATTENTION CALLBACKS: they also run asynchronously *outside* the singleThread lock!
//--------------------------------------------------------------
inline
void removeFolderIfExistsRecursion(const AbstractPath& folderPath, //throw FileError
                                   const std::function<void(const std::wstring& displayPath)>& onBeforeFileDeletion    /*throw X*/, //
                                   const std::function<void(const std::wstring& displayPath)>& onBeforeSymlinkDeletion /*throw X*/, //optional
                                   const std::function<void(const std::wstring& displayPath)>& onBeforeFolderDeletion  /*throw X*/, //
                                   std::mutex& singleThread)
{
    parallelScope([folderPath, onBeforeFileDeletion, onBeforeSymlinkDeletion, onBeforeFolderDeletion]
    { AFS::removeFolderIfExistsRecursion(folderPath, onBeforeFileDeletion, onBeforeSymlinkDeletion, onBeforeFolderDeletion); /*throw FileError*/ }, singleThread);
}


inline
AFS::FileCopyResult copyFileTransactional(const AbstractPath& sourcePath, const AFS::StreamAttributes& attrSource, //throw FileError, ErrorFileLocked, X
                                          const AbstractPath& targetPath,
                                          bool copyFilePermissions,
                                          bool transactionalCopy,
                                          const std::function<void()>& onDeleteTargetFile /*throw X*/,
                                          const IoCallback& notifyUnbufferedIO /*throw X*/,
                                          std::mutex& singleThread)
{
    return parallelScope([=]
    {
        return AFS::copyFileTransactional(sourcePath, attrSource, targetPath, copyFilePermissions, transactionalCopy, onDeleteTargetFile, notifyUnbufferedIO); //throw FileError, ErrorFileLocked, X
    }, singleThread);
}

inline //RecycleSession::moveToRecycleBin() is internally synchronized!
void moveToRecycleBinIfExists(AFS::RecycleSession& recyclerSession, const AbstractPath& itemPath, const Zstring& logicalRelPath, std::mutex& singleThread) //throw FileError, RecycleBinUnavailable
{ parallelScope([=, &recyclerSession] { return recyclerSession.moveToRecycleBinIfExists(itemPath, logicalRelPath); /*throw FileError, RecycleBinUnavailable*/ }, singleThread); }

inline //FileVersioner::revisionFile() is internally synchronized!
void revisionFile(FileVersioner& versioner, const FileDescriptor& fileDescr, const Zstring& relativePath, const IoCallback& notifyUnbufferedIO /*throw X*/, std::mutex& singleThread) //throw FileError, X
{ parallelScope([=, &versioner] { versioner.revisionFile(fileDescr, relativePath, notifyUnbufferedIO); /*throw FileError, X*/ }, singleThread); }

inline //FileVersioner::revisionSymlink() is internally synchronized!
void revisionSymlink(FileVersioner& versioner, const AbstractPath& linkPath, const Zstring& relativePath, std::mutex& singleThread) //throw FileError
{ parallelScope([=, &versioner] { versioner.revisionSymlink(linkPath, relativePath); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionFolder() is internally synchronized!
void revisionFolder(FileVersioner& versioner,
                    const AbstractPath& folderPath, const Zstring& relativePath,
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFileMove    /*throw X*/,
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFolderMove  /*throw X*/,
                    const IoCallback& notifyUnbufferedIO  /*throw X*/,
                    std::mutex& singleThread) //throw FileError, X
{ parallelScope([=, &versioner] { versioner.revisionFolder(folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO); /*throw FileError, X*/ }, singleThread); }

inline
void verifyFiles(const AbstractPath& sourcePath, const AbstractPath& targetPath, const IoCallback& notifyUnbufferedIO /*throw X*/, std::mutex& singleThread) //throw FileError, X
{ parallelScope([=] { ::verifyFiles(sourcePath, targetPath, notifyUnbufferedIO); /*throw FileError, X*/ }, singleThread); }

}

//#################################################################################################################
//#################################################################################################################

class DeletionHandler //abstract deletion variants: permanently, recycle bin, user-defined directory
{
public:
    DeletionHandler(const AbstractPath& baseFolderPath,
                    bool& recyclerMissingReportOnce,
                    bool& warnRecyclerMissing,
                    DeletionVariant deletionVariant,
                    const AbstractPath& versioningFolderPath,
                    VersioningStyle versioningStyle,
                    time_t syncStartTime); //nothrow!

    //clean-up temporary directory (recycle bin optimization)
    void tryCleanup(PhaseCallback& cb /*throw X*/); //throw X

    void removeFileWithCallback(const FileDescriptor& fileDescr, const Zstring& relPath, bool beforeOverwrite, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //throw FileError, ThreadStopRequest
    void removeLinkWithCallback(const AbstractPath& linkPath,    const Zstring& relPath, bool beforeOverwrite, AsyncItemStatReporter& statReporter, std::mutex& singleThread);  //
    void removeDirWithCallback (const AbstractPath& dirPath,     const Zstring& relPath, AsyncItemStatReporter& statReporter, std::mutex& singleThread);                        //

private:
    DeletionHandler           (const DeletionHandler&) = delete;
    DeletionHandler& operator=(const DeletionHandler&) = delete;

    void moveToRecycleBinIfExists(const AbstractPath& itemPath, const Zstring& relPath, std::mutex& singleThread) //throw FileError, RecycleBinUnavailable
    {
        assert(deletionVariant_ == DeletionVariant::recycler);

        //might not be needed => create lazily:
        if (!recyclerSession_ && !recyclerUnavailableExcept_)
            try
            {
                recyclerSession_ = AFS::createRecyclerSession(baseFolderPath_); //throw FileError, RecycleBinUnavailable
                //double-initialization caveat: do NOT run session initialization in parallel!
                // => createRecyclerSession must *not* do file I/O!
            }
            catch (const RecycleBinUnavailable& e) { recyclerUnavailableExcept_ = e; }

        if (recyclerUnavailableExcept_) //add context, or user might think we're removing baseFolderPath_!
            throw RecycleBinUnavailable(replaceCpy(_("Unable to move %x to the recycle bin."), L"%x", fmtPath(AFS::getDisplayPath(itemPath))),
                                        replaceCpy(recyclerUnavailableExcept_->toString(), L"\n\n", L'\n'));
        /*  "Unable to move "Z:\folder\file.txt" to the recycle bin.

             The recycle bin is not available for "Z:\".

             Ignore and delete permanently each time recycle bin is unavailable?"                */

        parallel::moveToRecycleBinIfExists(*recyclerSession_, itemPath, relPath, singleThread); //throw FileError, RecycleBinUnavailable
    }

    //might not be needed => create lazily:
    FileVersioner& getOrCreateVersioner() //throw FileError
    {
        assert(deletionVariant_ == DeletionVariant::versioning);
        if (!versioner_)
            versioner_ = std::make_unique<FileVersioner>(versioningFolderPath_, versioningStyle_, syncStartTime_); //throw FileError
        return *versioner_;
    }

    bool& recyclerMissingReportOnce_; //shared by threads! access under "singleThread" lock!
    bool& warnRecyclerMissing_;       //WarningDialogs::warnRecyclerMissing

    const DeletionVariant deletionVariant_; //keep it invariant! e.g. consider getOrCreateVersioner() one-time construction!

    const AbstractPath baseFolderPath_;

    std::unique_ptr<AFS::RecycleSession> recyclerSession_;           //it's one of these (or none if not yet initialized)
    std::optional<RecycleBinUnavailable> recyclerUnavailableExcept_; //

    //used only for DeletionVariant::versioning:
    const AbstractPath versioningFolderPath_;
    const VersioningStyle versioningStyle_;
    const time_t syncStartTime_;
    std::unique_ptr<FileVersioner> versioner_;

    //buffer status texts:
    const std::wstring txtDelFilePermanent_  = _("Deleting file %x");
    const std::wstring txtDelFileRecycler_   = _("Moving file %x to the recycle bin");
    const std::wstring txtDelFileVersioning_ = replaceCpy(_("Moving file %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));

    const std::wstring txtDelSymlinkPermanent_  = _("Deleting symbolic link %x");
    const std::wstring txtDelSymlinkRecycler_   = _("Moving symbolic link %x to the recycle bin");
    const std::wstring txtDelSymlinkVersioning_ = replaceCpy(_("Moving symbolic link %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));

    const std::wstring txtDelFolderPermanent_  = _("Deleting folder %x");
    const std::wstring txtDelFolderRecycler_   = _("Moving folder %x to the recycle bin");
    const std::wstring txtDelFolderVersioning_ = replaceCpy(_("Moving folder %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));

    const std::wstring txtMovingFileXtoY_   = _("Moving file %x to %y");
    const std::wstring txtMovingFolderXtoY_ = _("Moving folder %x to %y");
};


DeletionHandler::DeletionHandler(const AbstractPath& baseFolderPath,
                                 bool& recyclerMissingReportOnce,
                                 bool& warnRecyclerMissing,
                                 DeletionVariant deletionVariant,
                                 const AbstractPath& versioningFolderPath,
                                 VersioningStyle versioningStyle,
                                 time_t syncStartTime) :
    recyclerMissingReportOnce_(recyclerMissingReportOnce),
    warnRecyclerMissing_(warnRecyclerMissing),
    deletionVariant_(deletionVariant),
    baseFolderPath_(baseFolderPath),
    versioningFolderPath_(versioningFolderPath),
    versioningStyle_(versioningStyle),
    syncStartTime_(syncStartTime) {}


void DeletionHandler::tryCleanup(PhaseCallback& cb /*throw X*/) //throw X
{
    assert(runningOnMainThread());
    switch (deletionVariant_)
    {
        case DeletionVariant::recycler:
            if (recyclerSession_)
            {
                auto notifyDeletionStatus = [&](const std::wstring& displayPath)
                {
                    if (!displayPath.empty())
                        cb.updateStatus(replaceCpy(txtDelFileRecycler_, L"%x", fmtPath(displayPath))); //throw X
                    else
                        cb.requestUiUpdate(); //throw X
                };
                //move content of temporary directory to recycle bin in one go
                tryReportingError([&] { recyclerSession_->tryCleanup(notifyDeletionStatus); /*throw FileError*/}, cb); //throw X
            }
            break;

        case DeletionVariant::permanent:
        case DeletionVariant::versioning:
            break;
    }
}


void DeletionHandler::removeFileWithCallback(const FileDescriptor& fileDescr, const Zstring& relPath, bool beforeOverwrite,
                                             AsyncItemStatReporter& statReporter, std::mutex& singleThread) //throw FileError, ThreadStopRequest
{
    if (deletionVariant_ != DeletionVariant::permanent &&
        endsWith(relPath, AFS::TEMP_FILE_ENDING)) //special rule: always delete .ffs_tmp files permanently!
    {
        if (!beforeOverwrite) reportInfo(replaceCpy(txtDelFilePermanent_, L"%x", fmtPath(AFS::getDisplayPath(fileDescr.path))), statReporter); //throw ThreadStopRequest
        parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
    }
    else
        /*  don't use AsyncItemStatReporter if "beforeOverwrite":
            - logInfo/updateStatus() is superfluous/confuses user, except: do show progress and allow cancel for versioning!
            - no (logical) item count update desired
              => BUT: total byte count should still be adjusted if versioning requires a file copy instead of a move!
            - if fail-safe file copy is active, then the next operation will be a simple "rename"
              => don't risk updateStatus() throwing ThreadStopRequest() leaving the target deleted rather than updated!     */
        switch (deletionVariant_)
        {
            case DeletionVariant::permanent:
                if (!beforeOverwrite) reportInfo(replaceCpy(txtDelFilePermanent_, L"%x", fmtPath(AFS::getDisplayPath(fileDescr.path))), statReporter); //throw ThreadStopRequest
                parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
                break;

            case DeletionVariant::recycler:
                if (!beforeOverwrite) reportInfo(replaceCpy(txtDelFileRecycler_, L"%x", fmtPath(AFS::getDisplayPath(fileDescr.path))), statReporter); //throw ThreadStopRequest
                try
                {
                    moveToRecycleBinIfExists(fileDescr.path, relPath, singleThread); //throw FileError, RecycleBinUnavailable
                }
                catch (const RecycleBinUnavailable& e)
                {
                    if (!recyclerMissingReportOnce_) //shared by threads! access under "singleThread" lock!
                    {
                        recyclerMissingReportOnce_ = true;
                        statReporter.reportWarning(e.toString() + L"\n\n" + _("Ignore and delete permanently each time recycle bin is unavailable?"), warnRecyclerMissing_); //throw ThreadStopRequest
                    }
                    if (!beforeOverwrite) statReporter.logMessage(replaceCpy(txtDelFilePermanent_, L"%x", fmtPath(AFS::getDisplayPath(fileDescr.path))) +
                                                                      L" [" + _("Recycle bin unavailable") + L']', PhaseCallback::MsgType::warning); //throw ThreadStopRequest
                    parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
                }
                break;

            case DeletionVariant::versioning:
            {
                std::wstring statusMsg = replaceCpy(txtDelFileVersioning_, L"%x", fmtPath(AFS::getDisplayPath(fileDescr.path)));
                PercentStatReporter percentReporter(statusMsg, fileDescr.attr.fileSize, statReporter);

                if (!beforeOverwrite) reportInfo(std::move(statusMsg), statReporter); //throw ThreadStopRequest
                //else: 1. versioning is moving only: no (potentially throwing) status updates
                //      2. versioning needs to copy: may throw ThreadStopRequest, but *no* status updates, unless copying takes so long that % needs to be displayed

                //callback runs *outside* singleThread_ lock! => fine
                IoCallback notifyUnbufferedIO = [&](int64_t bytesDelta)
                {
                    percentReporter.updateDeltaAndStatus(bytesDelta); //throw ThreadStopRequest
                    interruptionPoint(); //throw ThreadStopRequest => not reliably covered by PercentStatReporter::updateDeltaAndStatus()!
                };
                parallel::revisionFile(getOrCreateVersioner(), fileDescr, relPath, notifyUnbufferedIO, singleThread); //throw FileError, ThreadStopRequest
            }
            break;
        }

    //even if the source item did not exist anymore, significant I/O work was done => report unconditionally
    if (!beforeOverwrite) statReporter.reportDelta(1, 0);
}


void DeletionHandler::removeLinkWithCallback(const AbstractPath& linkPath, const Zstring& relPath, bool beforeOverwrite,
                                             AsyncItemStatReporter& statReporter, std::mutex& singleThread) //throw FileError, throw ThreadStopRequest
{
    /*  don't use AsyncItemStatReporter if "beforeOverwrite":
        - logInfo() is superfluous/confuses user
        - no (logical) item count update desired
        - don't risk updateStatus() throwing ThreadStopRequest() leaving the target deleted rather than updated!  */
    switch (deletionVariant_)
    {
        case DeletionVariant::permanent:
            if (!beforeOverwrite) reportInfo(replaceCpy(txtDelSymlinkPermanent_, L"%x", fmtPath(AFS::getDisplayPath(linkPath))), statReporter); //throw ThreadStopRequest
            parallel::removeSymlinkIfExists(linkPath, singleThread); //throw FileError
            break;

        case DeletionVariant::recycler:
            if (!beforeOverwrite) reportInfo(replaceCpy(txtDelSymlinkRecycler_, L"%x", fmtPath(AFS::getDisplayPath(linkPath))), statReporter); //throw ThreadStopRequest
            try
            {
                moveToRecycleBinIfExists(linkPath, relPath, singleThread); //throw FileError, RecycleBinUnavailable
            }
            catch (const RecycleBinUnavailable& e)
            {
                if (!recyclerMissingReportOnce_) //shared by threads! access under "singleThread" lock!
                {
                    recyclerMissingReportOnce_ = true;
                    statReporter.reportWarning(e.toString() + L"\n\n" + _("Ignore and delete permanently each time recycle bin is unavailable?"), warnRecyclerMissing_); //throw ThreadStopRequest
                }
                if (!beforeOverwrite) statReporter.logMessage(replaceCpy(txtDelSymlinkPermanent_, L"%x", fmtPath(AFS::getDisplayPath(linkPath))) +
                                                                  L" [" + _("Recycle bin unavailable") + L']', PhaseCallback::MsgType::warning); //throw ThreadStopRequest
                parallel::removeSymlinkIfExists(linkPath, singleThread); //throw FileError
            }
            break;

        case DeletionVariant::versioning:
            if (!beforeOverwrite) reportInfo(replaceCpy(txtDelSymlinkVersioning_, L"%x", fmtPath(AFS::getDisplayPath(linkPath))), statReporter); //throw ThreadStopRequest
            parallel::revisionSymlink(getOrCreateVersioner(), linkPath, relPath, singleThread); //throw FileError
            break;
    }
    //remain transactional as much as possible => no more callbacks that can throw after successful deletion! (next: update file model!)

    //even if the source item did not exist anymore, significant I/O work was done => report unconditionally
    if (!beforeOverwrite) statReporter.reportDelta(1, 0);
}


void DeletionHandler::removeDirWithCallback(const AbstractPath& folderPath, const Zstring& relPath,
                                            AsyncItemStatReporter& statReporter, std::mutex& singleThread) //throw FileError, ThreadStopRequest
{
    auto removeFolderPermanently = [&]
    {
        //callbacks run *outside* singleThread_ lock! => fine
        auto onBeforeDeletion = [&statReporter](const std::wstring& statusText, const std::wstring& displayPath)
        {
            statReporter.updateStatus(replaceCpy(statusText, L"%x", fmtPath(displayPath))); //throw ThreadStopRequest
            statReporter.reportDelta(1, 0); //it would be more correct to report *after* work was done!
        };
        static_assert(std::is_const_v<decltype(txtDelFilePermanent_)>, "callbacks better be thread-safe!");

        parallel::removeFolderIfExistsRecursion(folderPath,
        [&](const std::wstring& displayPath) { onBeforeDeletion(txtDelFilePermanent_,    displayPath); },
        [&](const std::wstring& displayPath) { onBeforeDeletion(txtDelSymlinkPermanent_, displayPath); },
        [&](const std::wstring& displayPath) { onBeforeDeletion(txtDelFolderPermanent_,  displayPath); }, singleThread); //throw FileError, ThreadStopRequest
    };

    switch (deletionVariant_)
    {
        case DeletionVariant::permanent:
        {
            reportInfo(replaceCpy(txtDelFolderPermanent_, L"%x", fmtPath(AFS::getDisplayPath(folderPath))), statReporter); //throw ThreadStopRequest
            removeFolderPermanently(); //throw FileError, ThreadStopRequest
        }
        break;

        case DeletionVariant::recycler:
            reportInfo(replaceCpy(txtDelFolderRecycler_, L"%x", fmtPath(AFS::getDisplayPath(folderPath))), statReporter); //throw ThreadStopRequest
            try
            {
                moveToRecycleBinIfExists(folderPath, relPath, singleThread); //throw FileError, RecycleBinUnavailable
                statReporter.reportDelta(1, 0); //moving to recycler is ONE logical operation, irrespective of the number of child elements!
            }
            catch (const RecycleBinUnavailable& e)
            {
                if (!recyclerMissingReportOnce_) //shared by threads! access under "singleThread" lock!
                {
                    recyclerMissingReportOnce_ = true;
                    statReporter.reportWarning(e.toString() + L"\n\n" + _("Ignore and delete permanently each time recycle bin is unavailable?"), warnRecyclerMissing_); //throw ThreadStopRequest
                }
                statReporter.logMessage(replaceCpy(txtDelFolderPermanent_, L"%x", fmtPath(AFS::getDisplayPath(folderPath))) +
                                        L" [" + _("Recycle bin unavailable") + L']', PhaseCallback::MsgType::warning); //throw ThreadStopRequest
                removeFolderPermanently(); //throw FileError, ThreadStopRequest
            }
            break;

        case DeletionVariant::versioning:
        {
            reportInfo(replaceCpy(txtDelFolderVersioning_, L"%x", fmtPath(AFS::getDisplayPath(folderPath))), statReporter); //throw ThreadStopRequest

            //callbacks run *outside* singleThread_ lock! => fine
            auto notifyMove = [&statReporter](const std::wstring& statusText, const std::wstring& displayPathFrom, const std::wstring& displayPathTo)
            {
                statReporter.updateStatus(replaceCpy(replaceCpy(statusText, L"%x", L'\n' + fmtPath(displayPathFrom)), L"%y", L'\n' + fmtPath(displayPathTo))); //throw ThreadStopRequest
                statReporter.reportDelta(1, 0); //it would be more correct to report *after* work was done!
            };
            static_assert(std::is_const_v<decltype(txtMovingFileXtoY_)>, "callbacks better be thread-safe!");
            auto onBeforeFileMove   = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFileXtoY_,   displayPathFrom, displayPathTo); };
            auto onBeforeFolderMove = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFolderXtoY_, displayPathFrom, displayPathTo); };
            auto notifyUnbufferedIO = [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); interruptionPoint(); }; //throw ThreadStopRequest

            parallel::revisionFolder(getOrCreateVersioner(), folderPath, relPath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO, singleThread); //throw FileError, ThreadStopRequest
        }
        break;
    }
}

//===================================================================================================
//===================================================================================================

class Workload
{
public:
    Workload(size_t threadCount, AsyncCallback& acb) : acb_(acb), workload_(threadCount) { assert(threadCount > 0); }

    using WorkItem  = std::function<void() /*throw ThreadStopRequest*/>;
    using WorkItems = RingBuffer<WorkItem>; //FIFO!

    //blocking call: context of worker thread
    WorkItem getNext(size_t threadIdx) //throw ThreadStopRequest
    {
        interruptionPoint(); //throw ThreadStopRequest

        std::unique_lock dummy(lockWork_);
        for (;;)
        {
            if (!workload_[threadIdx].empty())
            {
                auto wi = std::move(workload_[threadIdx].    front());
                /**/                workload_[threadIdx].pop_front();
                return wi;
            }
            if (!pendingWorkload_.empty())
            {
                workload_[threadIdx] = std::move(pendingWorkload_.    front());
                /**/                             pendingWorkload_.pop_front();
                assert(!workload_[threadIdx].empty());
            }
            else
            {
                WorkItems& items = *std::max_element(workload_.begin(), workload_.end(), [](const WorkItems& lhs, const WorkItems& rhs) { return lhs.size() < rhs.size(); });
                if (!items.empty()) //=> != workload_[threadIdx]
                {
                    //steal half of largest workload from other thread
                    const size_t sz = items.size(); //[!] changes during loop!
                    for (size_t i = 0; i < sz; ++i)
                    {
                        auto wi = std::move(items.    front());
                        /**/                items.pop_front();
                        if (i % 2 == 0)
                            workload_[threadIdx].push_back(std::move(wi));
                        else
                            items.push_back(std::move(wi));
                    }
                }
                else //wait...
                {
                    if (++idleThreads_ == workload_.size())
                        acb_.notifyAllDone(); //noexcept
                    ZEN_ON_SCOPE_EXIT(--idleThreads_);

                    auto haveNewWork = [&] { return !pendingWorkload_.empty() || std::any_of(workload_.begin(), workload_.end(), [](const WorkItems& wi) { return !wi.empty(); }); };

                    interruptibleWait(conditionNewWork_, dummy, [&] { return haveNewWork(); }); //throw ThreadStopRequest
                    //it's sufficient to notify condition in addWorkItems() only (as long as we use std::condition_variable::notify_all())
                }
            }
        }
    }

    void addWorkItems(RingBuffer<WorkItems>&& buckets)
    {
        {
            std::lock_guard dummy(lockWork_);
            while (!buckets.empty())
            {
                pendingWorkload_.push_back(std::move(buckets.    front()));
                /**/                                 buckets.pop_front();
            }
        }
        conditionNewWork_.notify_all();
    }

private:
    Workload           (const Workload&) = delete;
    Workload& operator=(const Workload&) = delete;

    AsyncCallback& acb_;

    std::mutex lockWork_;
    std::condition_variable conditionNewWork_;

    size_t idleThreads_ = 0;

    std::vector<WorkItems> workload_; //thread-specific buckets
    RingBuffer<WorkItems> pendingWorkload_; //FIFO: buckets of work items for use by any thread
};


template <class List> inline
bool haveNameClash(const Zstring& itemName, const List& m)
{
    return std::any_of(m.begin(), m.end(),
    [&](const typename List::value_type& obj) { return equalNoCase(obj.getItemNameAny(), itemName); }); //equalNoCase: when in doubt => assume name clash!
}


class FolderPairSyncer
{
public:
    struct SyncCtx
    {
        bool verifyCopiedFiles;
        bool copyFilePermissions;
        bool failSafeFileCopy;
        DeletionHandler& delHandlerLeft;
        DeletionHandler& delHandlerRight;
    };

    static void runSync(SyncCtx& syncCtx, BaseFolderPair& baseFolder, PhaseCallback& cb)
    {
        runPass(PassNo::zero, syncCtx, baseFolder, cb); //prepare file moves
        runPass(PassNo::one,  syncCtx, baseFolder, cb); //delete files (or overwrite big ones with smaller ones)
        runPass(PassNo::two,  syncCtx, baseFolder, cb); //copy rest
    }

private:
    friend class Workload;

    enum class PassNo
    {
        zero, //prepare file moves
        one,  //delete files
        two,  //create, modify
        never //skip item
    };

    FolderPairSyncer(SyncCtx& syncCtx, std::mutex& singleThread, AsyncCallback& acb) :
        delHandlerLeft_     (syncCtx.delHandlerLeft),
        delHandlerRight_    (syncCtx.delHandlerRight),
        verifyCopiedFiles_  (syncCtx.verifyCopiedFiles),
        copyFilePermissions_(syncCtx.copyFilePermissions),
        failSafeFileCopy_   (syncCtx.failSafeFileCopy),
        singleThread_(singleThread),
        acb_(acb) {}

    static PassNo getPass(const FilePair&    file);
    static PassNo getPass(const SymlinkPair& symlink);
    static PassNo getPass(const FolderPair&  folder);
    static bool needZeroPass(const FilePair& file);
    static bool needZeroPass(const FolderPair& folder);

    static void runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, PhaseCallback& cb); //throw X

    RingBuffer<Workload::WorkItems> getFolderLevelWorkItems(PassNo pass, ContainerObject& parentFolder, Workload& workload);

    static bool containsMoveTarget(const FolderPair& parent);
    void executeFileMove(FilePair& file); //throw ThreadStopRequest
    template <SelectSide side> void executeFileMoveImpl(FilePair& fileFrom, FilePair& fileTo); //throw ThreadStopRequest

    void synchronizeFile(FilePair& file);                                                     //
    template <SelectSide side> void synchronizeFileInt(FilePair& file, SyncOperation syncOp); //throw FileError, ErrorMoveUnsupported, ThreadStopRequest

    void synchronizeLink(SymlinkPair& symlink);                                                        //
    template <SelectSide sideTrg> void synchronizeLinkInt(SymlinkPair& symlink, SyncOperation syncOp); //throw FileError, ThreadStopRequest

    void synchronizeFolder(FolderPair& folder);                                                        //
    template <SelectSide sideTrg> void synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp); //throw FileError, ThreadStopRequest

    void reportItemInfo(const std::wstring& msgTemplate, const AbstractPath& itemPath) { reportInfo(replaceCpy(msgTemplate, L"%x", fmtPath(AFS::getDisplayPath(itemPath))), acb_); }

    void reportItemInfo(const std::wstring& msgTemplate, const AbstractPath& itemPath1, const AbstractPath& itemPath2) //throw ThreadStopRequest
    {
        reportInfo(replaceCpy(replaceCpy(msgTemplate, L"%x", L'\n' + fmtPath(AFS::getDisplayPath(itemPath1))),
                              L"%y", L'\n' + fmtPath(AFS::getDisplayPath(itemPath2))), acb_); //throw ThreadStopRequest
    }

    //already existing after onDeleteTargetFile(): undefined behavior! (e.g. fail/overwrite/auto-rename)
    AFS::FileCopyResult copyFileWithCallback(const FileDescriptor& sourceDescr,
                                             const AbstractPath& targetPath,
                                             const std::function<void()>& onDeleteTargetFile /*throw X*/, //optional!
                                             AsyncItemStatReporter& statReporter, //ThreadStopRequest
                                             const std::wstring& statusMsg); //throw FileError, ThreadStopRequest, X

    DeletionHandler& delHandlerLeft_;
    DeletionHandler& delHandlerRight_;

    const bool verifyCopiedFiles_;
    const bool copyFilePermissions_;
    const bool failSafeFileCopy_;

    std::mutex& singleThread_;
    AsyncCallback& acb_;

    //preload status texts (premature?)
    const std::wstring txtCreatingFile_      {_("Creating file %x"         )};
    const std::wstring txtCreatingLink_      {_("Creating symbolic link %x")};
    const std::wstring txtCreatingFolder_    {_("Creating folder %x"       )};
    const std::wstring txtUpdatingFile_      {_("Updating file %x"         )};
    const std::wstring txtUpdatingLink_      {_("Updating symbolic link %x")};
    const std::wstring txtVerifyingFile_     {_("Verifying file %x"        )};
    const std::wstring txtUpdatingAttributes_{_("Updating attributes of %x")};
    const std::wstring txtMovingFileXtoY_    {_("Moving file %x to %y"     )};
    const std::wstring txtSourceItemNotExist_{_("Source item %x is not existing")};
};

//===================================================================================================
//===================================================================================================
/* ___________________________
   |                         |
   | Multithreaded File Copy |
   |_________________________|

           ----------------     =================
           |Async Callback| <-- |Worker Thread 1|
           ----------------     ====================
                 /|\               |Worker Thread 2|
                  |                =================
             =============           |   ...    |
  GUI    <-- |Main Thread|          \|/        \|/
Callback     =============       --------------------
                                 |     Workload     |
                                 --------------------

Notes: - All threads share a single mutex, unlocked only during file I/O => do NOT require file_hierarchy.cpp classes to be thread-safe (i.e. internally synchronized)!
       - Workload holds (folder-level-) items in buckets associated with each worker thread (FTP scenario: avoid CWDs)
       - If a worker is idle, its Workload bucket is empty and no more pending buckets available: steal from other threads (=> take half of largest bucket)
       - Maximize opportunity for parallelization ASAP: Workload buckets serve folder-items *before* files/symlinks => reduce risk of work-stealing
       - Memory consumption: work items may grow indefinitely; however: test case "C:\" ~80MB per 1 million work items
*/

void FolderPairSyncer::runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, PhaseCallback& cb) //throw X
{
    std::mutex singleThread; //only a single worker thread may run at a time, except for parallel file I/O

    AsyncCallback acb;                                //
    FolderPairSyncer fps(syncCtx, singleThread, acb); //manage life time: enclose InterruptibleThread's!!!
    Workload workload(1, acb);
    workload.addWorkItems(fps.getFolderLevelWorkItems(pass, baseFolder, workload)); //initial workload: set *before* threads get access!

    std::vector<InterruptibleThread> worker;
    ZEN_ON_SCOPE_EXIT( for (InterruptibleThread& wt : worker) wt.requestStop(); ); //stop *all* at the same time before join!

    size_t threadIdx = 0;
    Zstring threadName = Zstr("Sync");
        worker.emplace_back([threadIdx, &singleThread, &acb, &workload, threadName = std::move(threadName)]
        {
            setCurrentThreadName(threadName);

            while (/*blocking call:*/ std::function<void()> workItem = workload.getNext(threadIdx)) //throw ThreadStopRequest
            {
                acb.notifyTaskBegin(0 /*prio*/); //same prio, while processing only one folder pair at a time
                ZEN_ON_SCOPE_EXIT(acb.notifyTaskEnd());

                std::lock_guard dummy(singleThread); //protect ALL accesses to "fps" and workItem execution!
                workItem(); //throw ThreadStopRequest
            }
        });
    acb.waitUntilDone(UI_UPDATE_INTERVAL / 2 /*every ~50 ms*/, cb); //throw X
}


//thread-safe thanks to std::mutex singleThread
RingBuffer<Workload::WorkItems> FolderPairSyncer::getFolderLevelWorkItems(PassNo pass, ContainerObject& parentFolder, Workload& workload)
{
    RingBuffer<Workload::WorkItems> buckets;

    RingBuffer<ContainerObject*> foldersToInspect;
    foldersToInspect.push_back(&parentFolder);

    while (!foldersToInspect.empty())
    {
        ContainerObject& hierObj = *foldersToInspect.    front();
        /**/                        foldersToInspect.pop_front();

        RingBuffer<std::function<void()>> workItems;

        if (pass == PassNo::zero)
        {
            //create folders as required by file move targets:
            for (FolderPair& folder : hierObj.refSubFolders())
                if (needZeroPass(folder) &&
                    !haveNameClash(folder.getItemNameAny(), folder.parent().refSubFiles()) && //name clash with files/symlinks? obscure => skip folder creation
                    !haveNameClash(folder.getItemNameAny(), folder.parent().refSubLinks()))   // => move: fall back to delete + copy
                    workItems.push_back([this, &folder, &workload, pass]
                {
                    tryReportingError([&] { synchronizeFolder(folder); }, acb_); //throw ThreadStopRequest
                    //error? => still process move targets (for delete + copy fall back!)
                    workload.addWorkItems(getFolderLevelWorkItems(pass, folder, workload));
                });
            else
                foldersToInspect.push_back(&folder);

            for (FilePair& file : hierObj.refSubFiles())
                if (needZeroPass(file))
                    workItems.push_back([this, &file] { executeFileMove(file); /*throw ThreadStopRequest*/ });
        }
        else
        {
            //synchronize folders *first* (see comment above "Multithreaded File Copy")
            for (FolderPair& folder : hierObj.refSubFolders())
                if (pass == getPass(folder))
                    workItems.push_back([this, &folder, &workload, pass]
                {
                    tryReportingError([&]{ synchronizeFolder(folder); }, acb_); //throw ThreadStopRequest

                    workload.addWorkItems(getFolderLevelWorkItems(pass, folder, workload));
                });
            else
                foldersToInspect.push_back(&folder);

            //synchronize files:
            for (FilePair& file : hierObj.refSubFiles())
                if (pass == getPass(file))
                    workItems.push_back([this, &file]
                {
                    tryReportingError([&]{ synchronizeFile(file); }, acb_); //throw ThreadStopRequest
                });

            //synchronize symbolic links:
            for (SymlinkPair& symlink : hierObj.refSubLinks())
                if (pass == getPass(symlink))
                    workItems.push_back([this, &symlink]
                {
                    tryReportingError([&] { synchronizeLink(symlink); }, acb_); //throw ThreadStopRequest
                });
        }

        if (!workItems.empty())
            buckets.push_back(std::move(workItems));
    }

    return buckets;
}


/* __________________________
   |Move algorithm, 0th pass|
   --------------------------
    1. loop over hierarchy and find "move targets" => remember required parent folders

    2. create required folders hierarchically:
        - name-clash with other file/symlink (=> obscure!): fall back to delete and copy
        - source folder missing:                            child items already deleted by synchronizeFolder()
        - ignored error:                                    fall back to delete and copy (in phases 1 and 2)

    3. start file move (via targets)
        - name-clash with other folder/symlink (=> obscure!): fall back to delete and copy
        - ErrorMoveUnsupported:                               fall back to delete and copy
        - ignored error:                                      fall back to delete and copy

  __________________
  |killer-scenarios|
  ------------------
    propagate the following move sequences:
    I) a -> a/a      caveat syncing parent directory first leads to circular dependency!

    II) a/a -> a     caveat: fixing name clash will remove source!

    III) c -> d      caveat: move-sequence needs to be processed in correct order!
         b -> c/b
         a -> b/a                                                                               */

template <SelectSide side>
void FolderPairSyncer::executeFileMoveImpl(FilePair& fileFrom, FilePair& fileTo) //throw ThreadStopRequest
{
    const bool fallBackCopyDelete = [&]
    {
        //creation of parent folder has failed earlier? => fall back to delete + copy
        const FolderPair* parentMissing = nullptr; //let's be more specific: go up in hierarchy until first missing parent folder
        for (const FolderPair* f = dynamic_cast<const FolderPair*>(&fileTo.parent()); f && f->isEmpty<side>(); f = dynamic_cast<const FolderPair*>(&f->parent()))
            parentMissing = f;

        if (parentMissing)
        {
            reportItemInfo(_("Cannot move file %x to %y.") + L"\n\n" +
                           replaceCpy(_("Parent folder %x is not existing."), L"%x", fmtPath(AFS::getDisplayPath(parentMissing->getAbstractPath<side>()))),
                           fileFrom.getAbstractPath<side>(),
                           fileTo  .getAbstractPath<side>()); //throw ThreadStopRequest
            return true;
        }

        //name clash with folders/symlinks? obscure => fall back to delete + copy
        if (haveNameClash(fileTo.getItemNameAny(), fileTo.parent().refSubFolders()) ||
            haveNameClash(fileTo.getItemNameAny(), fileTo.parent().refSubLinks  ()))
        {
            reportItemInfo(_("Cannot move file %x to %y.") + L"\n\n" +
                           replaceCpy(_("The name %x is already used by another item."), L"%x", fmtPath(fileTo.getItemNameAny())),
                           fileFrom.getAbstractPath<side>(),
                           fileTo  .getAbstractPath<side>()); //throw ThreadStopRequest
            return true;
        }

        bool moveSupported = true;
        const std::wstring errMsg = tryReportingError([&] //throw ThreadStopRequest
        {
            try
            {
                synchronizeFile(fileTo); //throw FileError, ErrorMoveUnsupported, ThreadStopRequest
            }
            catch (const ErrorMoveUnsupported& e)
            {
                acb_.logMessage(e.toString(), PhaseCallback::MsgType::info); //let user know that move operation is not supported, then fall back:
                moveSupported = false;
            }
        }, acb_);

        return !errMsg.empty() || !moveSupported; //move failed? We cannot allow to continue and have move source's parent directory deleted, messing up statistics!
    }();

    if (fallBackCopyDelete)
    {
        auto getStats = [&]() -> std::pair<int, int64_t>
        {
            SyncStatistics statSrc(fileFrom);
            SyncStatistics statTrg(fileTo);
            return {getCUD(statSrc) + getCUD(statTrg), statSrc.getBytesToProcess() + statTrg.getBytesToProcess()};
        };
        const auto [itemsBefore, bytesBefore] = getStats();
        fileFrom.setMoveRef(nullptr);
        fileTo  .setMoveRef(nullptr);
        const auto [itemsAfter, bytesAfter] = getStats();

        //fix statistics total to match "copy + delete"
        acb_.updateDataTotal(itemsAfter - itemsBefore, bytesAfter - bytesBefore); //noexcept
    }
}


void FolderPairSyncer::executeFileMove(FilePair& file) //throw ThreadStopRequest
{
    const SyncOperation syncOp = file.getSyncOperation();
    switch (syncOp)
    {
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            if (FilePair* fileFrom = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                assert(fileFrom->getMoveRef() == file.getId());

                if (syncOp == SO_MOVE_LEFT_TO)
                    executeFileMoveImpl<SelectSide::left>(*fileFrom, file); //throw ThreadStopRequest
                else
                    executeFileMoveImpl<SelectSide::right>(*fileFrom, file); //throw ThreadStopRequest
            }
            else assert(false);
            break;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        case SO_MOVE_LEFT_FROM:  //don't try to move more than *once* per pair
        case SO_MOVE_RIGHT_FROM: //
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::needZeroPass()
            break;
    }
}

//---------------------------------------------------------------------------------------------------------------

bool FolderPairSyncer::containsMoveTarget(const FolderPair& parent)
{
    for (const FilePair& file : parent.refSubFiles())
        if (needZeroPass(file))
            return true;

    for (const FolderPair& subFolder : parent.refSubFolders())
        if (containsMoveTarget(subFolder))
            return true;
    return false;
}


//0th pass: execute file moves (+ optional fallback to delete/copy in passes 1 and 2)
bool FolderPairSyncer::needZeroPass(const FolderPair& folder)
{
    switch (folder.getSyncOperation())
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
            return containsMoveTarget(folder); //recursive! watch perf!

        case SO_DO_NOTHING:          //implies !isEmpty<side>(); see FolderPair::getSyncOperation()
        case SO_UNRESOLVED_CONFLICT: //
        case SO_EQUAL:
        case SO_OVERWRITE_LEFT:  //possible: e.g. manually-resolved dir-traversal conflict
        case SO_OVERWRITE_RIGHT: //
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            assert((!folder.isEmpty<SelectSide::left>() && !folder.isEmpty<SelectSide::right>()) || !containsMoveTarget(folder));
            //we're good to move contained items
            break;
        case SO_DELETE_LEFT:  //not possible in the context of planning to move a child item, see FolderPair::getSyncOperation()
        case SO_DELETE_RIGHT: //
            assert(!containsMoveTarget(folder));
            break;
        case SO_MOVE_LEFT_FROM:  //
        case SO_MOVE_RIGHT_FROM: //status not possible for folder
        case SO_MOVE_LEFT_TO:    //
        case SO_MOVE_RIGHT_TO:   //
            assert(false);
            break;
    }
    return false;
}


inline
bool FolderPairSyncer::needZeroPass(const FilePair& file)
{
    switch (file.getSyncOperation())
    {
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            return true;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        case SO_MOVE_LEFT_FROM:  //don't try to move more than *once* per pair
        case SO_MOVE_RIGHT_FROM: //
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            break;
    }
    return false;
}


//1st, 2nd pass benefits:
// - avoid disk space shortage: 1. delete files, 2. overwrite big with small files first
// - support change in type: overwrite file by directory, symlink by file, etc.

inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FilePair& file)
{
    switch (file.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PassNo::one;

        case SO_OVERWRITE_LEFT:
            return file.getFileSize<SelectSide::left>() > file.getFileSize<SelectSide::right>() ? PassNo::one : PassNo::two;

        case SO_OVERWRITE_RIGHT:
            return file.getFileSize<SelectSide::left>() < file.getFileSize<SelectSide::right>() ? PassNo::one : PassNo::two;

        case SO_MOVE_LEFT_FROM:  //
        case SO_MOVE_RIGHT_FROM: // [!]
            return PassNo::never;
        case SO_MOVE_LEFT_TO:  //
        case SO_MOVE_RIGHT_TO: //make sure 2-step move is processed in second pass, after move *target* parent directory was created!
            return PassNo::two;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PassNo::two;

        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PassNo::never;
    }
    assert(false);
    return PassNo::never;
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const SymlinkPair& symlink)
{
    switch (symlink.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PassNo::one; //make sure to delete symlinks in first pass, and equally named file or dir in second pass: usecase "overwrite symlink with regular file"!

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PassNo::two;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
            [[fallthrough]];
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PassNo::never;
    }
    assert(false);
    return PassNo::never;
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FolderPair& folder)
{
    switch (folder.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PassNo::one;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PassNo::two;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
            [[fallthrough]];
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PassNo::never;
    }
    assert(false);
    return PassNo::never;
}

//---------------------------------------------------------------------------------------------------------------

inline
void FolderPairSyncer::synchronizeFile(FilePair& file) //throw FileError, ErrorMoveUnsupported, ThreadStopRequest
{
    assert(isLocked(singleThread_));
    const SyncOperation syncOp = file.getSyncOperation();

    if (std::optional<SelectSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == SelectSide::left)
            synchronizeFileInt<SelectSide::left>(file, syncOp);
        else
            synchronizeFileInt<SelectSide::right>(file, syncOp);
    }
}


template <SelectSide sideTrg>
void FolderPairSyncer::synchronizeFileInt(FilePair& file, SyncOperation syncOp) //throw FileError, ErrorMoveUnsupported, ThreadStopRequest
{
    constexpr SelectSide sideSrc = getOtherSide<sideTrg>;
    DeletionHandler& delHandlerTrg = selectParam<sideTrg>(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&file.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = file.getAbstractPath<sideTrg>();

            const std::wstring& statusMsg = replaceCpy(txtCreatingFile_, L"%x", fmtPath(AFS::getDisplayPath(targetPath)));
            reportInfo(std::wstring(statusMsg), acb_); //throw ThreadStopRequest

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), acb_);
            try
            {
                const AFS::FileCopyResult result = copyFileWithCallback({file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>()},
                                                                        targetPath,
                                                                        nullptr, //onDeleteTargetFile: nothing to delete
                                                                        //if existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
                                                                        statReporter,
                                                                        statusMsg); //throw FileError, ThreadStopRequest
                statReporter.reportDelta(1, 0);

                //update FilePair
                file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                          result.modTime, //target time set from source
                                          result.modTime,
                                          result.targetFilePrint,
                                          result.sourceFilePrint,
                                          false, file.isFollowedSymlink<sideSrc>());

                if (result.errorModTime) //log only; no popup
                    acb_.logMessage(result.errorModTime->toString(), PhaseCallback::MsgType::warning); //throw ThreadStopRequest
            }
            catch (const FileError& e)
            {
                bool sourceExists = true;
                try { sourceExists = parallel::itemExists(file.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                //abstract context => unclear which exception is more relevant/useless:
                //e could be "item not found": doh; e2 devoid of any details after SFTP error: https://freefilesync.org/forum/viewtopic.php?t=7138#p24064
                catch (const FileError& e2) { throw FileError(replaceCpy(e.toString(), L"\n\n", L'\n'), replaceCpy(e2.toString(), L"\n\n", L'\n')); }

                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!
                if (!sourceExists)
                {
                    reportItemInfo(txtSourceItemNotExist_, file.getAbstractPath<sideSrc>()); //throw ThreadStopRequest

                    statReporter.reportDelta(1, 0);
                    //even if the source item does not exist anymore, significant I/O work was done => report
                    file.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)
                    //remove only *after* evaluating "file, sideSrc"!
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        {
            AsyncItemStatReporter statReporter(1, 0, acb_);

            if (file.isFollowedSymlink<sideTrg>())
                delHandlerTrg.removeLinkWithCallback(file.getAbstractPath<sideTrg>(), file.getRelativePath<sideTrg>(),
                                                     false /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest
            else
                delHandlerTrg.removeFileWithCallback({file.getAbstractPath<sideTrg>(), file.getAttributes<sideTrg>()}, file.getRelativePath<sideTrg>(),
                                                     false /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest

            file.removeObject<sideTrg>(); //update FilePair
        }
        break;

        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            if (FilePair* fileFrom = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                FilePair* fileTo = &file;
                assert(fileFrom->getMoveRef() == fileTo->getId());

                assert((fileFrom->getSyncOperation() == SO_MOVE_LEFT_FROM  && fileTo->getSyncOperation() == SO_MOVE_LEFT_TO  && sideTrg == SelectSide::left) ||
                       (fileFrom->getSyncOperation() == SO_MOVE_RIGHT_FROM && fileTo->getSyncOperation() == SO_MOVE_RIGHT_TO && sideTrg == SelectSide::right));

                const AbstractPath pathFrom = fileFrom->getAbstractPath<sideTrg>();
                const AbstractPath pathTo   = fileTo  ->getAbstractPath<sideTrg>();

                reportItemInfo(txtMovingFileXtoY_, pathFrom, pathTo); //throw ThreadStopRequest

                AsyncItemStatReporter statReporter(1, 0, acb_);

                //already existing: undefined behavior! (e.g. fail/overwrite)
                parallel::moveAndRenameItem(pathFrom, pathTo, singleThread_); //throw FileError, ErrorMoveUnsupported

                statReporter.reportDelta(1, 0);

                //update FilePair
                assert(fileFrom->getFileSize<sideTrg>() == fileTo->getFileSize<sideSrc>());
                fileTo->setSyncedTo<sideTrg>(fileTo  ->getItemName<sideSrc>(),
                                             fileTo  ->getFileSize<sideSrc>(),
                                             fileFrom->getLastWriteTime<sideTrg>(),
                                             fileTo  ->getLastWriteTime<sideSrc>(),
                                             fileFrom->getFilePrint<sideTrg>(),
                                             fileTo  ->getFilePrint<sideSrc>(),
                                             fileFrom->isFollowedSymlink<sideTrg>(),
                                             fileTo  ->isFollowedSymlink<sideSrc>());
                fileFrom->removeObject<sideTrg>(); //remove only *after* evaluating "fileFrom, sideTrg"!
            }
            else (assert(false));
            break;

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        {
            //respect differences in case of source object:
            const AbstractPath targetPathLogical = AFS::appendRelPath(file.parent().getAbstractPath<sideTrg>(), file.getItemName<sideSrc>());

            AbstractPath targetPathResolvedOld = file.getAbstractPath<sideTrg>(); //support change in case when syncing to case-sensitive SFTP on Windows!
            AbstractPath targetPathResolvedNew = targetPathLogical;
            if (file.isFollowedSymlink<sideTrg>()) //follow link when updating file rather than delete it and replace with regular file!!!
                targetPathResolvedOld = targetPathResolvedNew = parallel::getSymlinkResolvedPath(file.getAbstractPath<sideTrg>(), singleThread_); //throw FileError

            const std::wstring& statusMsg = replaceCpy(txtUpdatingFile_, L"%x", fmtPath(AFS::getDisplayPath(targetPathResolvedOld)));
            reportInfo(std::wstring(statusMsg), acb_); //throw ThreadStopRequest

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), acb_);

            if (file.isFollowedSymlink<sideTrg>()) //since we follow the link, we need to sync case sensitivity of the link manually!
                if (getUnicodeNormalForm(file.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(file.getItemName<sideSrc>())) //have difference in case?
                    //already existing: undefined behavior! (e.g. fail/overwrite)
                    parallel::moveAndRenameItem(file.getAbstractPath<sideTrg>(), targetPathLogical, singleThread_); //throw FileError, (ErrorMoveUnsupported)

            auto onDeleteTargetFile = [&] //delete target at appropriate time
            {
                assert(isLocked(singleThread_));
                FileAttributes followedTargetAttr = file.getAttributes<sideTrg>();
                followedTargetAttr.isFollowedSymlink = false;

                if (file.isFollowedSymlink<sideTrg>())
                    delHandlerTrg.removeLinkWithCallback(targetPathResolvedOld, file.getRelativePath<sideTrg>(),
                                                         true /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest
                else
                    delHandlerTrg.removeFileWithCallback({targetPathResolvedOld, followedTargetAttr}, file.getRelativePath<sideTrg>(),
                true /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest

                //file.removeObject<sideTrg>(); -> doesn't make sense for isFollowedSymlink(); "file, sideTrg" evaluated below!
            };

            const AFS::FileCopyResult result = copyFileWithCallback({file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>()},
                                                                    targetPathResolvedNew,
                                                                    onDeleteTargetFile,
                                                                    statReporter,
                                                                    statusMsg); //throw FileError, ThreadStopRequest
            statReporter.reportDelta(1, 0);
            //we model "delete + copy" as ONE logical operation

            //update FilePair
            file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                      result.modTime, //target time set from source
                                      result.modTime,
                                      result.targetFilePrint,
                                      result.sourceFilePrint,
                                      file.isFollowedSymlink<sideTrg>(),
                                      file.isFollowedSymlink<sideSrc>());

            if (result.errorModTime) //log only; no popup
                acb_.logMessage(result.errorModTime->toString(), PhaseCallback::MsgType::warning); //throw ThreadStopRequest
        }
        break;

        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            //harmonize with file_hierarchy.cpp::getSyncOpDescription!!
            reportItemInfo(txtUpdatingAttributes_, file.getAbstractPath<sideTrg>()); //throw ThreadStopRequest
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(file.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(file.getItemName<sideSrc>())) //have difference in case?
                    //already existing: undefined behavior! (e.g. fail/overwrite)
                    parallel::moveAndRenameItem(file.getAbstractPath<sideTrg>(), //throw FileError, (ErrorMoveUnsupported)
                                                AFS::appendRelPath(file.parent().getAbstractPath<sideTrg>(), file.getItemName<sideSrc>()), singleThread_);
                else
                    assert(false);

#if 0 //changing file time without copying content is not justified after CompareVariant::size finds "equal" files! similar issue with CompareVariant::timeSize and FileTimeTolerance == -1
                //Bonus: some devices don't support setting (precise) file times anyway, e.g. FAT or MTP!
                if (file.getLastWriteTime<sideTrg>() != file.getLastWriteTime<sideSrc>())
                    //- no need to call sameFileTime() or respect 2 second FAT/FAT32 precision in this comparison
                    //- do NOT read *current* source file time, but use buffered value which corresponds to time of comparison!
                    parallel::setModTime(file.getAbstractPath<sideTrg>(), file.getLastWriteTime<sideSrc>()); //throw FileError
#endif
                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                assert(file.getFileSize<sideTrg>() == file.getFileSize<sideSrc>());
                file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), file.getFileSize<sideSrc>(),
                                          file.getLastWriteTime <sideTrg>(),
                                          file.getLastWriteTime <sideSrc>(),
                                          file.getFilePrint     <sideTrg>(),
                                          file.getFilePrint     <sideSrc>(),
                                          file.isFollowedSymlink<sideTrg>(),
                                          file.isFollowedSymlink<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:  //use SO_MOVE_LEFT_TO/SO_MOVE_RIGHT_TO to execute move:
        case SO_MOVE_RIGHT_FROM: //=> makes sure parent directory has been created
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }
}


inline
void FolderPairSyncer::synchronizeLink(SymlinkPair& symlink) //throw FileError, ThreadStopRequest
{
    assert(isLocked(singleThread_));
    const SyncOperation syncOp = symlink.getSyncOperation();

    if (std::optional<SelectSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == SelectSide::left)
            synchronizeLinkInt<SelectSide::left>(symlink, syncOp);
        else
            synchronizeLinkInt<SelectSide::right>(symlink, syncOp);
    }
}


template <SelectSide sideTrg>
void FolderPairSyncer::synchronizeLinkInt(SymlinkPair& symlink, SyncOperation syncOp) //throw FileError, ThreadStopRequest
{
    constexpr SelectSide sideSrc = getOtherSide<sideTrg>;
    DeletionHandler& delHandlerTrg = selectParam<sideTrg>(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&symlink.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = symlink.getAbstractPath<sideTrg>();
            reportItemInfo(txtCreatingLink_, targetPath); //throw ThreadStopRequest

            AsyncItemStatReporter statReporter(1, 0, acb_);
            try
            {
                parallel::copySymlink(symlink.getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError

                statReporter.reportDelta(1, 0);

                //update SymlinkPair
                symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                             symlink.getLastWriteTime<sideSrc>(), //target time set from source
                                             symlink.getLastWriteTime<sideSrc>());

            }
            catch (const FileError& e)
            {
                bool sourceExists = true;
                try { sourceExists = parallel::itemExists(symlink.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                //abstract context => unclear which exception is more relevant/useless:
                catch (const FileError& e2) { throw FileError(replaceCpy(e.toString(), L"\n\n", L'\n'), replaceCpy(e2.toString(), L"\n\n", L'\n')); }

                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!
                if (!sourceExists)
                {
                    reportItemInfo(txtSourceItemNotExist_, symlink.getAbstractPath<sideSrc>()); //throw ThreadStopRequest

                    //even if the source item does not exist anymore, significant I/O work was done => report
                    statReporter.reportDelta(1, 0);
                    symlink.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        {
            AsyncItemStatReporter statReporter(1, 0, acb_);

            delHandlerTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getRelativePath<sideTrg>(),
                                                 false /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest

            symlink.removeObject<sideTrg>(); //update SymlinkPair
        }
        break;

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        {
            reportItemInfo(txtUpdatingLink_, symlink.getAbstractPath<sideTrg>()); //throw ThreadStopRequest

            AsyncItemStatReporter statReporter(1, 0, acb_);

            delHandlerTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getRelativePath<sideTrg>(),
                                                 true /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest

            //symlink.removeObject<sideTrg>(); -> "symlink, sideTrg" evaluated below!

            parallel::copySymlink(symlink.getAbstractPath<sideSrc>(),
                                  AFS::appendRelPath(symlink.parent().getAbstractPath<sideTrg>(), symlink.getItemName<sideSrc>()), //respect differences in case of source object
                                  copyFilePermissions_, singleThread_); //throw FileError

            statReporter.reportDelta(1, 0); //we model "delete + copy" as ONE logical operation

            //update SymlinkPair
            symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                         symlink.getLastWriteTime<sideSrc>(), //target time set from source
                                         symlink.getLastWriteTime<sideSrc>());
        }
        break;

        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            reportItemInfo(txtUpdatingAttributes_, symlink.getAbstractPath<sideTrg>()); //throw ThreadStopRequest
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(symlink.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(symlink.getItemName<sideSrc>())) //have difference in case?
                    //already existing: undefined behavior! (e.g. fail/overwrite)
                    parallel::moveAndRenameItem(symlink.getAbstractPath<sideTrg>(), //throw FileError, (ErrorMoveUnsupported)
                                                AFS::appendRelPath(symlink.parent().getAbstractPath<sideTrg>(), symlink.getItemName<sideSrc>()), singleThread_);
                else
                    assert(false);

                //if (symlink.getLastWriteTime<sideTrg>() != symlink.getLastWriteTime<sideSrc>())
                //    //- no need to call sameFileTime() or respect 2 second FAT/FAT32 precision in this comparison
                //    //- do NOT read *current* source file time, but use buffered value which corresponds to time of comparison!
                //    parallel::setModTimeSymlink(symlink.getAbstractPath<sideTrg>(), symlink.getLastWriteTime<sideSrc>()); //throw FileError

                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                symlink.setSyncedTo<sideTrg>(symlink.getItemName<sideSrc>(),
                                             symlink.getLastWriteTime<sideTrg>(), //target time set from source
                                             symlink.getLastWriteTime<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }
}


inline
void FolderPairSyncer::synchronizeFolder(FolderPair& folder) //throw FileError, ThreadStopRequest
{
    assert(isLocked(singleThread_));
    const SyncOperation syncOp = folder.getSyncOperation();

    if (std::optional<SelectSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == SelectSide::left)
            synchronizeFolderInt<SelectSide::left>(folder, syncOp);
        else
            synchronizeFolderInt<SelectSide::right>(folder, syncOp);
    }
}


template <SelectSide sideTrg>
void FolderPairSyncer::synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp) //throw FileError, ThreadStopRequest
{
    constexpr SelectSide sideSrc = getOtherSide<sideTrg>;
    DeletionHandler& delHandlerTrg = selectParam<sideTrg>(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&folder.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = folder.getAbstractPath<sideTrg>();
            reportItemInfo(txtCreatingFolder_, targetPath); //throw ThreadStopRequest

            //shallow-"copying" a folder might not fail if source is missing, so we need to check this first:
            if (parallel::itemExists(folder.getAbstractPath<sideSrc>(), singleThread_)) //throw FileError
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);
                try
                {
                    //already existing: fail
                    AFS::FolderCopyResult result = parallel::copyNewFolder(folder.getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError

                    if (result.errorAttribs) //log only; no popup
                        acb_.logMessage(result.errorAttribs->toString(), PhaseCallback::MsgType::warning); //throw ThreadStopRequest
                }
                catch (FileError&)
                {
                    bool folderAlreadyExists = false;
                    try { folderAlreadyExists = parallel::getItemType(targetPath, singleThread_) == AFS::ItemType::folder; } /*throw FileError*/ catch (FileError&) {}
                    //previous exception is more relevant; good enough? https://freefilesync.org/forum/viewtopic.php?t=5266

                    if (!folderAlreadyExists)
                        throw;
                }

                statReporter.reportDelta(1, 0);

                //update FolderPair
                folder.setSyncedTo<sideTrg>(folder.getItemName<sideSrc>(),
                                            false, //isSymlinkTrg
                                            folder.isFollowedSymlink<sideSrc>());
            }
            else //source deleted meanwhile...
            {
                reportItemInfo(txtSourceItemNotExist_, folder.getAbstractPath<sideSrc>()); //throw ThreadStopRequest

                //attention when fixing statistics due to missing folder: child items may be scheduled for move, so deletion will have move-references flip back to copy + delete!
                const SyncStatistics statsBefore(folder.base()); //=> don't bother considering individual move operations, just calculate over the whole tree
                folder.refSubFiles  ().clear(); //
                folder.refSubLinks  ().clear(); //update FolderPair
                folder.refSubFolders().clear(); //
                folder.removeObject<sideSrc>(); //
                const SyncStatistics statsAfter(folder.base());

                acb_.updateDataProcessed(1, 0); //even if the source item does not exist anymore, significant I/O work was done => report
                acb_.updateDataTotal(getCUD(statsAfter) - getCUD(statsBefore) + 1, statsAfter.getBytesToProcess() - statsBefore.getBytesToProcess()); //noexcept
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        {
            const SyncStatistics subStats(folder); //counts sub-objects only!
            AsyncItemStatReporter statReporter(1 + getCUD(subStats), subStats.getBytesToProcess(), acb_);

            if (folder.isFollowedSymlink<sideTrg>())
                delHandlerTrg.removeLinkWithCallback(folder.getAbstractPath<sideTrg>(), folder.getRelativePath<sideTrg>(),
                                                     false /*beforeOverwrite*/, statReporter, singleThread_); //throw FileError, ThreadStopRequest
            else
                delHandlerTrg.removeDirWithCallback(folder.getAbstractPath<sideTrg>(), folder.getRelativePath<sideTrg>(),
                                                    statReporter, singleThread_); //throw FileError, ThreadStopRequest

            //TODO: implement parallel folder deletion

            folder.refSubFiles  ().clear(); //
            folder.refSubLinks  ().clear(); //update FolderPair
            folder.refSubFolders().clear(); //
            folder.removeObject<sideTrg>(); //
        }
        break;

        case SO_OVERWRITE_LEFT:  //possible: e.g. manually-resolved dir-traversal conflict
        case SO_OVERWRITE_RIGHT: //
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            reportItemInfo(txtUpdatingAttributes_, folder.getAbstractPath<sideTrg>()); //throw ThreadStopRequest
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(folder.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(folder.getItemName<sideSrc>())) //have difference in case?
                    //already existing: undefined behavior! (e.g. fail/overwrite)
                    parallel::moveAndRenameItem(folder.getAbstractPath<sideTrg>(), //throw FileError, (ErrorMoveUnsupported)
                                                AFS::appendRelPath(folder.parent().getAbstractPath<sideTrg>(), folder.getItemName<sideSrc>()), singleThread_);
                else
                    assert(false);
                //copyFileTimes -> useless: modification time changes with each child-object creation/deletion

                statReporter.reportDelta(1, 0);

                //-> both sides *should* be completely equal now...
                folder.setSyncedTo<sideTrg>(folder.getItemName<sideSrc>(),
                                            folder.isFollowedSymlink<sideTrg>(),
                                            folder.isFollowedSymlink<sideSrc>());
            }
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            assert(false); //should have been filtered out by FolderPairSyncer::getPass()
            return; //no update on processed data!
    }
}

//###########################################################################################

//returns current attributes of source file
AFS::FileCopyResult FolderPairSyncer::copyFileWithCallback(const FileDescriptor& sourceDescr,
                                                           const AbstractPath& targetPath,
                                                           const std::function<void()>& onDeleteTargetFile /*throw X*/,
                                                           AsyncItemStatReporter& statReporter /*throw ThreadStopRequest*/,
                                                           const std::wstring& statusMsg) //throw FileError, ThreadStopRequest, X
{
    const AbstractPath& sourcePath = sourceDescr.path;
    const AFS::StreamAttributes sourceAttr{sourceDescr.attr.modTime, sourceDescr.attr.fileSize, sourceDescr.attr.filePrint};

    auto copyOperation = [&](const AbstractPath& sourcePathTmp)
    {
        PercentStatReporter percentReporter(statusMsg, sourceDescr.attr.fileSize, statReporter);

        //already existing + no onDeleteTargetFile: undefined behavior! (e.g. fail/overwrite/auto-rename)
        const AFS::FileCopyResult result = parallel::copyFileTransactional(sourcePathTmp, sourceAttr, //throw FileError, ErrorFileLocked, ThreadStopRequest, X
                                                                           targetPath,
                                                                           copyFilePermissions_,
                                                                           failSafeFileCopy_, [&]
        {
            if (onDeleteTargetFile) //running *outside* singleThread_ lock! => onDeleteTargetFile-callback expects lock being held:
            {
                std::lock_guard dummy(singleThread_);
                onDeleteTargetFile(); //throw X
            }
        },
        [&](int64_t bytesDelta) //callback runs *outside* singleThread_ lock! => fine
        {
            percentReporter.updateDeltaAndStatus(bytesDelta); //throw ThreadStopRequest
            interruptionPoint(); //throw ThreadStopRequest => not reliably covered by PercentStatReporter::updateDeltaAndStatus()!
        },
        singleThread_);

        //#################### Verification #############################
        if (verifyCopiedFiles_)
        {
            reportItemInfo(txtVerifyingFile_, targetPath); //throw ThreadStopRequest

            //delete target if verification fails
            ZEN_ON_SCOPE_FAIL(try { parallel::removeFilePlain(targetPath, singleThread_); }
            catch (const FileError& e) { statReporter.logMessage(e.toString(), PhaseCallback::MsgType::error); /*throw ThreadStopRequest*/ });

            //callback runs *outside* singleThread_ lock! => fine
            auto verifyCallback = [&](int64_t bytesDelta) { interruptionPoint(); }; //throw ThreadStopRequest

            parallel::verifyFiles(sourcePathTmp, targetPath, verifyCallback, singleThread_); //throw FileError, ThreadStopRequest
        }
        //#################### /Verification #############################

        return result;
    };

    return copyOperation(sourcePath); //throw FileError, (ErrorFileLocked), ThreadStopRequest
}

//###########################################################################################

template <SelectSide side>
bool checkBaseFolderStatus(BaseFolderPair& baseFolder, PhaseCallback& callback /*throw X*/)
{
    const AbstractPath folderPath = baseFolder.getAbstractPath<side>();

    switch (baseFolder.getFolderStatus<side>())
    {
        case BaseFolderStatus::existing:
        {
            const std::wstring errMsg = tryReportingError([&]
            {
                AFS::getItemType(folderPath); //throw FileError
            }, callback); //throw X
            if (!errMsg.empty())
                return false;
        }
        break;

        case BaseFolderStatus::notExisting:
        {
            bool folderExisting = false;

            const std::wstring errMsg = tryReportingError([&]
            {
                folderExisting = AFS::itemExists(folderPath); //throw FileError
            }, callback); //throw X
            if (!errMsg.empty())
                return false;
            if (folderExisting) //=> somebody else created it: problem?
            {
                /* Is it possible we're catching a "false positive" here, could FFS have created the directory indirectly after comparison?
                      1. deletion handling: recycler       -> no, temp directory created only at first deletion
                      2. deletion handling: versioning     -> "
                      3. log file creates containing folder -> no, log only created in batch mode, and only *before* comparison
                      4. yes, could be us! e.g. multiple folder pairs to non-yet-existing target folder => too obscure!?            */
                callback.reportFatalError(replaceCpy(_("The folder %x is already existing, but was not found earlier during comparison."),
                                                     L"%x", fmtPath(AFS::getDisplayPath(folderPath)))); //throw X
                return false;
            }
        }
        break;

        case BaseFolderStatus::failure:
            //e.g. TEMPORARY network drop! base directory not found during comparison
            //=> sync-directions are based on false assumptions! Abort.
            callback.reportFatalError(replaceCpy(_("Skipping folder pair because %x could not be accessed during comparison."),
                                                 L"%x", fmtPath(AFS::getDisplayPath(folderPath)))); //throw X
            return false;
    }
    return true;
}


template <SelectSide side> //create base directories first (if not yet existing) -> no symlink or attribute copying!
bool createBaseFolder(BaseFolderPair& baseFolder, bool copyFilePermissions, PhaseCallback& callback /*throw X*/) //return false if fatal error occurred
{
    switch (baseFolder.getFolderStatus<side>())
    {
        case BaseFolderStatus::existing:
            break;

        case BaseFolderStatus::notExisting:
        {
            //create target directory: user presumably ignored warning "dir not yet existing" in order to have it created automatically
            const AbstractPath folderPath = baseFolder.getAbstractPath<side>();
            static const SelectSide sideSrc = getOtherSide<side>;

            const std::wstring errMsg = tryReportingError([&]
            {
                if (baseFolder.getFolderStatus<sideSrc>() == BaseFolderStatus::existing) //copy file permissions
                {
                    if (const std::optional<AbstractPath> parentPath = AFS::getParentPath(folderPath))
                        AFS::createFolderIfMissingRecursion(*parentPath); //throw FileError

                    AFS::FolderCopyResult result = AFS::copyNewFolder(baseFolder.getAbstractPath<sideSrc>(), folderPath, copyFilePermissions); //throw FileError

                    if (result.errorAttribs) //log only; no popup
                        callback.logMessage(result.errorAttribs->toString(), PhaseCallback::MsgType::warning); //throw X
                }
                else
                    AFS::createFolderIfMissingRecursion(folderPath); //throw FileError
                assert(baseFolder.getFolderStatus<sideSrc>() != BaseFolderStatus::failure);

                baseFolder.setFolderStatus<side>(BaseFolderStatus::existing); //update our model!
            }, callback); //throw X

            return errMsg.empty();
        }

        case BaseFolderStatus::failure:
            assert(false); //already skipped after checkBaseFolderStatus()
            break;
    }
    return true;
}
}


void fff::synchronize(const std::chrono::system_clock::time_point& syncStartTime,
                      bool verifyCopiedFiles,
                      bool copyLockedFiles,
                      bool copyFilePermissions,
                      bool failSafeFileCopy,
                      bool runWithBackgroundPriority,
                      const std::vector<FolderPairSyncCfg>& syncConfig,
                      FolderComparison& folderCmp,
                      WarningDialogs& warnings,
                      ProcessCallback& callback /*throw X*/) //throw X
{
    //PERF_START;

    if (syncConfig.size() != folderCmp.size())
        throw std::logic_error(std::string(__FILE__) + '[' + numberTo<std::string>(__LINE__) + "] Contract violation!");

    //aggregate basic information
    std::vector<SyncStatistics> folderPairStats;
    {
        int     itemsTotal = 0;
        int64_t bytesTotal = 0;
        std::for_each(begin(folderCmp), end(folderCmp),
                      [&](const BaseFolderPair& baseFolder)
        {
            SyncStatistics fpStats(baseFolder);
            itemsTotal += getCUD(fpStats);
            bytesTotal += fpStats.getBytesToProcess();
            folderPairStats.push_back(fpStats);
        });

        //inform about the total amount of data that will be processed from now on
        //keep at beginning so that all gui elements are initialized properly
        callback.initNewPhase(itemsTotal, //throw X
                              bytesTotal,
                              ProcessPhase::synchronizing);
    }

    //-------------------------------------------------------------------------------

    //specify process and resource handling priorities
    std::unique_ptr<ScheduleForBackgroundProcessing> backgroundPrio;
    if (runWithBackgroundPriority)
        tryReportingError([&]
    {
        backgroundPrio = std::make_unique<ScheduleForBackgroundProcessing>(); //throw FileError
    }, callback); //throw X

    //prevent operating system going into sleep state
    std::unique_ptr<PreventStandby> noStandby;
    try
    {
        noStandby = std::make_unique<PreventStandby>(); //throw FileError
    }
    catch (const FileError& e) //failure is not critical => log only
    {
        callback.logMessage(e.toString(), PhaseCallback::MsgType::warning); //throw X
    }

    //-------------------execute basic checks all at once BEFORE starting sync--------------------------------------
    std::vector<unsigned char /*we really want bool*/> skipFolderPair(folderCmp.size(), false); //folder pairs may be skipped after fatal errors were found

    std::vector<std::tuple<const BaseFolderPair*, int /*conflict count*/, std::vector<SyncStatistics::ConflictInfo>>> checkUnresolvedConflicts;

    std::vector<std::tuple<const BaseFolderPair*, SelectSide, bool /*write access*/>> checkBaseFolderRaceCondition;

    std::vector<std::pair<AbstractPath, AbstractPath>> checkSignificantDiffPairs;

    std::vector<std::pair<AbstractPath, std::pair<int64_t, int64_t>>> checkDiskSpaceMissing; //base folder / space required / space available

    std::set<AbstractPath>                                  checkVersioningPaths;
    std::vector<std::pair<AbstractPath, const PathFilter*>> checkVersioningBasePaths; //hard filter creates new logical hierarchies for otherwise equal AbstractPath...

    std::set<AbstractPath> checkVersioningLimitPaths;

    //------------------- start checking folder pairs -------------------
    for (size_t folderIndex = 0; folderIndex < folderCmp.size(); ++folderIndex)
    {
        BaseFolderPair&          baseFolder     = folderCmp[folderIndex].ref();
        const FolderPairSyncCfg& folderPairCfg  = syncConfig[folderIndex];
        const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

        //exclude a few pathological cases, e.g. empty folder pair
        if (baseFolder.getAbstractPath<SelectSide::left >() ==
            baseFolder.getAbstractPath<SelectSide::right>())
        {
            skipFolderPair[folderIndex] = true;
            continue;
        }

        //prepare conflict preview:
        if (folderPairStat.conflictCount() > 0)
            checkUnresolvedConflicts.emplace_back(&baseFolder, folderPairStat.conflictCount(), folderPairStat.getConflictsPreview());

        //consider *all* paths that might be used during versioning limit at some time
        const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase);

        if (folderPairCfg.handleDeletion == DeletionVariant::versioning &&
            folderPairCfg.versioningStyle != VersioningStyle::replace)
            if (folderPairCfg.versionMaxAgeDays > 0 || folderPairCfg.versionCountMax > 0) //same check as in applyVersioningLimit()
                checkVersioningLimitPaths.insert(versioningFolderPath);

        const bool writeLeft = folderPairStat.createCount<SelectSide::left>() +
                               folderPairStat.updateCount<SelectSide::left>() +
                               folderPairStat.deleteCount<SelectSide::left>() > 0;

        const bool writeRight = folderPairStat.createCount<SelectSide::right>() +
                                folderPairStat.updateCount<SelectSide::right>() +
                                folderPairStat.deleteCount<SelectSide::right>() > 0;

        //prepare: check if some files are used by multiple pairs in read/write access
        checkBaseFolderRaceCondition.emplace_back(&baseFolder, SelectSide::left,  writeLeft);
        checkBaseFolderRaceCondition.emplace_back(&baseFolder, SelectSide::right, writeRight);

        //prepare: check if versioning path itself will be synchronized (and was not excluded via filter)
        if (folderPairCfg.handleDeletion == DeletionVariant::versioning)
            checkVersioningPaths.insert(versioningFolderPath);

        checkVersioningBasePaths.emplace_back(baseFolder.getAbstractPath<SelectSide::left >(), &baseFolder.getFilter());
        checkVersioningBasePaths.emplace_back(baseFolder.getAbstractPath<SelectSide::right>(), &baseFolder.getFilter());

        //===============================================================================
        //================ begin of checks that may SKIP folder pairs ===================
        //===============================================================================

        //skip folder pair if there is nothing to do (except when DB files need to be updated for two-way mode and move-detection)
        //=> avoid redundant errors in checkBaseFolderStatus() if base folder existence test failed during comparison
        if (getCUD(folderPairStat) == 0 && !folderPairCfg.saveSyncDB)
        {
            skipFolderPair[folderIndex] = true;
            continue;
        }

        //check for empty target folder paths: this only makes sense if empty field is source (and no DB files need to be created)
        if ((AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::left >()) && (writeLeft  || folderPairCfg.saveSyncDB)) ||
            (AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::right>()) && (writeRight || folderPairCfg.saveSyncDB)))
        {
            callback.reportFatalError(_("Target folder input field must not be empty."));
            skipFolderPair[folderIndex] = true;
            continue;
        }

        //check for network drops after comparison
        // - convenience: exit sync right here instead of showing tons of errors during file copy
        // - early failure! there's no point in evaluating subsequent warnings
        if (!checkBaseFolderStatus<SelectSide::left >(baseFolder, callback) ||
            !checkBaseFolderStatus<SelectSide::right>(baseFolder, callback))
        {
            skipFolderPair[folderIndex] = true;
            continue;
        }

        //allow propagation of deletions only from *empty* or *existing* source folder:
        auto sourceFolderMissing = [&](const AbstractPath& baseFolderPath, BaseFolderStatus folderStatus) //we need to evaluate existence status from time of comparison!
        {
            if (!AFS::isNullPath(baseFolderPath))
                //PERMANENT network drop: avoid data loss when source directory is not found AND user chose to ignore errors (else we wouldn't arrive here)
                if (folderPairStat.deleteCount() > 0) //check deletions only... (respect filtered items!)
                    //folderPairStat.conflictCount() == 0 && -> there COULD be conflicts for <Two way> variant if directory existence check fails, but loading sync.ffs_db succeeds
                    //https://sourceforge.net/tracker/?func=detail&atid=1093080&aid=3531351&group_id=234430 -> fixed, but still better not consider conflicts!
                    if (folderStatus != BaseFolderStatus::existing) //avoid race-condition: we need to evaluate existence status from time of comparison!
                    {
                        callback.reportFatalError(replaceCpy(_("Source folder %x not found."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath))));
                        return true;
                    }
            return false;
        };
        if (sourceFolderMissing(baseFolder.getAbstractPath<SelectSide::left >(), baseFolder.getFolderStatus<SelectSide:: left>()) ||
            sourceFolderMissing(baseFolder.getAbstractPath<SelectSide::right>(), baseFolder.getFolderStatus<SelectSide::right>()))
        {
            skipFolderPair[folderIndex] = true;
            continue;
        }

        //check if user-defined directory for deletion was specified
        if (folderPairCfg.handleDeletion == DeletionVariant::versioning)
            if (AFS::isNullPath(versioningFolderPath))
            {
                callback.reportFatalError(_("Please enter a target folder.")); //user should never see this: already checked in SyncCfgDialog
                skipFolderPair[folderIndex] = true;
                continue;
            }

        //check if more than 50% of total number of files/dirs are to be created/overwritten/deleted
        if (!AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::left >()) &&
            !AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::right>()))
            if (significantDifferenceDetected(folderPairStat))
                checkSignificantDiffPairs.emplace_back(baseFolder.getAbstractPath<SelectSide::left >(),
                                                       baseFolder.getAbstractPath<SelectSide::right>());

        //check for sufficient free diskspace (folderPath might not yet exist!)
        auto checkSpace = [&](const AbstractPath& baseFolderPath, int64_t minSpaceNeeded)
        {
            if (!AFS::isNullPath(baseFolderPath) && minSpaceNeeded > 0)
                try
                {
                    const int64_t freeSpace = AFS::getFreeDiskSpace(baseFolderPath); //throw FileError, returns < 0 if not available

                    if (0 <= freeSpace &&
                        freeSpace < minSpaceNeeded)
                        checkDiskSpaceMissing.push_back({baseFolderPath, {minSpaceNeeded, freeSpace}});
                }
                catch (const FileError& e) //not critical => log only
                {
                    callback.logMessage(e.toString(), PhaseCallback::MsgType::warning); //throw X
                }
        };
        const std::pair<int64_t, int64_t> spaceNeeded = MinimumDiskSpaceNeeded::calculate(baseFolder);

        if (baseFolder.getFolderStatus<SelectSide::left >() != BaseFolderStatus::failure) checkSpace(baseFolder.getAbstractPath<SelectSide::left >(), spaceNeeded.first);
        if (baseFolder.getFolderStatus<SelectSide::right>() != BaseFolderStatus::failure) checkSpace(baseFolder.getAbstractPath<SelectSide::right>(), spaceNeeded.second);
    }
    //--------------------------------------------------------------------------------------

    //check if unresolved conflicts exist
    if (!checkUnresolvedConflicts.empty())
    {
        //distribute CONFLICTS_PREVIEW_MAX over all pairs, not *per* pair, or else log size with many folder pairs can blow up!
        std::vector<std::vector<SyncStatistics::ConflictInfo>> conflictPreviewTrim(checkUnresolvedConflicts.size());

        size_t previewRemain = CONFLICTS_PREVIEW_MAX;
        for (size_t i = 0; ; ++i)
        {
            const size_t previewRemainOld = previewRemain;

            for (size_t j = 0; j < checkUnresolvedConflicts.size(); ++j)
            {
                const auto& [baseFolder, conflictCount, conflictPreview] = checkUnresolvedConflicts[j];

                if (i < conflictPreview.size())
                {
                    conflictPreviewTrim[j].push_back(conflictPreview[i]);
                    if (--previewRemain == 0)
                        goto break2; //sigh
                }
            }
            if (previewRemain == previewRemainOld)
                break;
        }
break2:

        std::wstring msg = _("The following items have unresolved conflicts and will not be synchronized:");

        auto itPrevi = conflictPreviewTrim.begin();
        for (const auto& [baseFolder, conflictCount, conflictPreview] : checkUnresolvedConflicts)
        {
            msg += L"\n\n" + _("Folder pair:") + L' ' +
                   AFS::getDisplayPath(baseFolder->getAbstractPath<SelectSide::left >()) + L" <-> " +
                   AFS::getDisplayPath(baseFolder->getAbstractPath<SelectSide::right>());

            for (const SyncStatistics::ConflictInfo& item : *itPrevi)
                msg += L'\n' + utfTo<std::wstring>(item.relPath) + L": " + item.msg;

            if (makeUnsigned(conflictCount) > itPrevi->size())
                msg += L"\n  [...]  " + replaceCpy(_P("Showing %y of 1 item", "Showing %y of %x items", conflictCount), //%x used as plural form placeholder!
                                                   L"%y", formatNumber(itPrevi->size()));
            ++itPrevi;
        }

        callback.reportWarning(msg, warnings.warnUnresolvedConflicts); //throw X
    }

    //check if user accidentally selected wrong directories for sync
    if (!checkSignificantDiffPairs.empty())
    {
        std::wstring msg = _("The following folders are significantly different. Please check that the correct folders are selected for synchronization.");

        for (const auto& [folderPathL, folderPathR] : checkSignificantDiffPairs)
            msg += L"\n\n" +
                   AFS::getDisplayPath(folderPathL) + L" <-> " + L'\n' +
                   AFS::getDisplayPath(folderPathR);

        callback.reportWarning(msg, warnings.warnSignificantDifference); //throw X
    }

    //check for sufficient free diskspace
    if (!checkDiskSpaceMissing.empty())
    {
        std::wstring msg = _("Not enough free disk space available in:");

        for (const auto& [folderPath, space] : checkDiskSpaceMissing)
            msg += L"\n\n" + AFS::getDisplayPath(folderPath) + L'\n' +
                   TAB_SPACE + _("Required:")  + L' ' + formatFilesizeShort(space.first)  + L'\n' +
                   TAB_SPACE + _("Available:") + L' ' + formatFilesizeShort(space.second);

        callback.reportWarning(msg, warnings.warnNotEnoughDiskSpace); //throw X
    }

    //check if folders are used by multiple pairs in read/write access
    {
        std::vector<PathRaceItem> pathRaceItems;

        //race condition := multiple accesses of which at least one is a write
        //=> use "writeAccess" to reduce list of - not necessarily conflicting - candidates to check (=> perf!)
        for (auto it = checkBaseFolderRaceCondition.begin(); it != checkBaseFolderRaceCondition.end(); ++it)
            if (const auto& [baseFolder1, side1, writeAccess1] = *it;
                writeAccess1)
                for (auto it2 = checkBaseFolderRaceCondition.begin(); it2 != checkBaseFolderRaceCondition.end(); ++it2)
                {
                    const auto& [baseFolder2, side2, writeAccess2] = *it2;

                    if (!writeAccess2 ||
                        it < it2) //avoid duplicate comparisons
                    {
                        //"The Things We Do for [Perf]"
                        /**/ if (side1 == SelectSide::left  && side2 == SelectSide::left ) checkPathRaceCondition<SelectSide::left,  SelectSide::left >(*baseFolder1, *baseFolder2, pathRaceItems);
                        else if (side1 == SelectSide::left  && side2 == SelectSide::right) checkPathRaceCondition<SelectSide::left,  SelectSide::right>(*baseFolder1, *baseFolder2, pathRaceItems);
                        else if (side1 == SelectSide::right && side2 == SelectSide::left ) checkPathRaceCondition<SelectSide::right, SelectSide::left >(*baseFolder1, *baseFolder2, pathRaceItems);
                        else                                                               checkPathRaceCondition<SelectSide::right, SelectSide::right>(*baseFolder1, *baseFolder2, pathRaceItems);
                    }
                }

        removeDuplicates(pathRaceItems);

        //create mapping table for folder pair positions
        std::unordered_map<const BaseFolderPair*, size_t> folderPairIdxs;
        for (size_t folderIndex = 0; folderIndex < folderCmp.size(); ++folderIndex)
            folderPairIdxs[&folderCmp[folderIndex].ref()] = folderIndex;

        std::partial_sort(pathRaceItems.begin(),
                          pathRaceItems.begin() + std::min(pathRaceItems.size(), CONFLICTS_PREVIEW_MAX),
                          pathRaceItems.end(), [&](const PathRaceItem& lhs, const PathRaceItem& rhs)
        {
            if (const std::weak_ordering cmp = comparePathNoCase(lhs, rhs);
                cmp != std::weak_ordering::equivalent)
                return cmp < 0; //1. order by device, and case-insensitive path

            return folderPairIdxs.find(&lhs.fsObj->base())->second < //2. order by folder pair position
                   folderPairIdxs.find(&rhs.fsObj->base())->second;
        });

        if (!pathRaceItems.empty())
        {
            std::wstring msg = _("Some files will be synchronized as part of multiple folder pairs.") + L'\n' +
                               _("To avoid conflicts, set up exclude filters so that each updated file is included by only one folder pair.") + L"\n\n";

            auto prevItem = pathRaceItems[0];
            std::for_each(pathRaceItems.begin(), pathRaceItems.begin() + std::min(pathRaceItems.size(), CONFLICTS_PREVIEW_MAX), [&](const PathRaceItem& item)
            {
                if (comparePathNoCase(item, prevItem) != std::weak_ordering::equivalent)
                    msg += L"\n"; //visually separate path groups

                msg += formatRaceItem(item) + L"\n";
                prevItem = item;
            });

            if (pathRaceItems.size() > CONFLICTS_PREVIEW_MAX)
                msg += L"\n[...]  " + replaceCpy(_P("Showing %y of 1 item", "Showing %y of %x items", pathRaceItems.size()), //%x used as plural form placeholder!
                                                 L"%y", formatNumber(CONFLICTS_PREVIEW_MAX));

            msg += L"\n💾: " + _("Write access") + L"  👓: " + _("Read access");

            callback.reportWarning(msg, warnings.warnDependentBaseFolders); //throw X
        }
    }

    //check if versioning folder itself will be synchronized (and was not excluded via filter)
    {
        std::wstring msg;
        bool shouldExclude = false;

        for (const AbstractPath& versioningFolderPath : checkVersioningPaths)
        {
            std::set<AbstractPath> foldersWithWarnings; //=> at most one msg per base folder (*and* per versioningFolderPath)

            for (const auto& [folderPath, filter] : checkVersioningBasePaths) //may contain duplicate paths, but with *different* hard filter!
                if (std::optional<PathDependency> pd = getPathDependency(versioningFolderPath, NullFilter(), folderPath, *filter))
                    if (const auto [it, inserted] = foldersWithWarnings.insert(folderPath);
                        inserted)
                    {
                        msg += L"\n\n" +
                               _("Selected folder:")   + L" \t" + AFS::getDisplayPath(folderPath) + L'\n' +
                               _("Versioning folder:") + L" \t" + AFS::getDisplayPath(versioningFolderPath);
                        if (pd->folderPathParent == folderPath) //else: probably fine? :>
                            if (!pd->relPath.empty())
                            {
                                shouldExclude = true;
                                msg += std::wstring() + L'\n' + L"⇒ " +
                                       _("Exclude:") + L" \t" + utfTo<std::wstring>(FILE_NAME_SEPARATOR + pd->relPath + FILE_NAME_SEPARATOR);
                            }
                        warn_static("else: ???")
                    }
        }
        if (!msg.empty())
            callback.reportWarning(_("The versioning folder is part of the synchronization.") +
                                   (shouldExclude ? L' ' + _("The folder should be excluded via filter.") : L"") +
                                   msg, warnings.warnVersioningFolderPartOfSync); //throw X
    }

    //warn if versioning folder paths differ only in case => possible pessimization for applyVersioningLimit()
    {
        std::map<std::pair<AfsDevice, ZstringNoCase>, std::set<AbstractPath>> ciPathAliases;

        for (const AbstractPath& folderPath : checkVersioningLimitPaths)
            ciPathAliases[std::pair(folderPath.afsDevice, folderPath.afsPath.value)].insert(folderPath);

        if (std::any_of(ciPathAliases.begin(), ciPathAliases.end(), [](const auto& item) { return item.second/*aliases*/.size() > 1; }))
        {
            std::wstring msg = _("The following folder paths differ in case. Please use a single form in order to avoid duplicate accesses.");
            for (const auto& [key, aliases] : ciPathAliases)
                if (aliases.size() > 1)
                {
                    msg += L'\n';
                    for (const AbstractPath& aliasPath : aliases)
                        msg += L'\n' + AFS::getDisplayPath(aliasPath);
                }
            callback.reportWarning(msg, warnings.warnFoldersDifferInCase); //throw X
        }
        //what about /folder and /Folder/subfolder? => yes, inconsistent, but doesn't matter for FFS
    }
    //-------------------end of basic checks------------------------------------------

    std::set<VersioningLimitFolder> versionLimitFolders;

    bool recyclerMissingReportOnce = false; //prompt user only *once* per sync, not per failed item!

    class PcbNoThrow : public PhaseCallback
    {
    public:
        explicit PcbNoThrow(ProcessCallback& cb) : cb_(cb) {}

        void updateDataProcessed(int itemsDelta, int64_t bytesDelta) override {} //sync DB/del-handler: logically not part of sync data, so let's ignore
        void updateDataTotal    (int itemsDelta, int64_t bytesDelta) override {} //

        void requestUiUpdate(bool force) override { try { cb_.requestUiUpdate(force); /*throw X*/} catch (...) {}; }

        void updateStatus(std::wstring&& msg) override { try { cb_.updateStatus(std::move(msg)); /*throw X*/} catch (...) {}; }
        void logMessage(const std::wstring& msg, MsgType type) override { try { cb_.logMessage(msg, type); /*throw X*/} catch (...) {}; }

        void reportWarning(const std::wstring& msg, bool& warningActive) override { logMessage(msg, MsgType::warning); /*ignore*/ }
        Response reportError     (const ErrorInfo& errorInfo)            override { logMessage(errorInfo.msg, MsgType::error); return Response::ignore; }
        void     reportFatalError(const std::wstring& msg)               override { logMessage(msg, MsgType::error); /*ignore*/ }

    private:
        ProcessCallback& cb_;
    } callbackNoThrow(callback);

    try
    {
        //loop through all directory pairs
        for (size_t folderIndex = 0; folderIndex < folderCmp.size(); ++folderIndex)
        {
            BaseFolderPair&          baseFolder     = folderCmp[folderIndex].ref();
            const FolderPairSyncCfg& folderPairCfg  = syncConfig[folderIndex];
            const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

            if (skipFolderPair[folderIndex]) //folder pairs may be skipped after fatal errors were found
                continue;

            //------------------------------------------------------------------------------------------
            callback.logMessage(_("Synchronizing folder pair:") + L' ' + getVariantNameWithSymbol(folderPairCfg.syncVar) + L'\n' + //throw X
                                TAB_SPACE + AFS::getDisplayPath(baseFolder.getAbstractPath<SelectSide::left >()) + L'\n' +
                                TAB_SPACE + AFS::getDisplayPath(baseFolder.getAbstractPath<SelectSide::right>()), PhaseCallback::MsgType::info);
            //------------------------------------------------------------------------------------------

            //checking a second time: 1. a long time may have passed since syncing the previous folder pairs!
            //                        2. expected to be run directly *before* createBaseFolder()!
            if (!checkBaseFolderStatus<SelectSide::left >(baseFolder, callback) ||
                !checkBaseFolderStatus<SelectSide::right>(baseFolder, callback))
                continue;

            //create base folders if not yet existing
            if (folderPairStat.createCount() > 0 || folderPairCfg.saveSyncDB) //else: temporary network drop leading to deletions already caught by "sourceFolderMissing" check!
                if (!createBaseFolder<SelectSide::left >(baseFolder, copyFilePermissions, callback) || //+ detect temporary network drop!!
                    !createBaseFolder<SelectSide::right>(baseFolder, copyFilePermissions, callback))   //
                    continue;

            //------------------------------------------------------------------------------------------
            //execute synchronization recursively

            //update database even when sync is cancelled:
            auto guardDbSave = makeGuard<ScopeGuardRunMode::onFail>([&]
            {
                if (folderPairCfg.saveSyncDB)
                    saveLastSynchronousState(baseFolder, failSafeFileCopy,
                                             callbackNoThrow);
            });

            //guarantee removal of invalid entries (where element is empty on both sides)
            ZEN_ON_SCOPE_EXIT(BaseFolderPair::removeEmpty(baseFolder));

            bool copyPermissionsFp = false;
            tryReportingError([&]
            {
                copyPermissionsFp = copyFilePermissions && //copy permissions only if asked for and supported by *both* sides!
                !AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::left >()) && //scenario: directory selected on one side only
                !AFS::isNullPath(baseFolder.getAbstractPath<SelectSide::right>()) && //
                AFS::supportPermissionCopy(baseFolder.getAbstractPath<SelectSide::left>(),
                                           baseFolder.getAbstractPath<SelectSide::right>()); //throw FileError
            }, callback); //throw X

            const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase);

            DeletionHandler delHandlerL(baseFolder.getAbstractPath<SelectSide::left>(),
                                        recyclerMissingReportOnce,
                                        warnings.warnRecyclerMissing,
                                        folderPairCfg.handleDeletion,
                                        versioningFolderPath,
                                        folderPairCfg.versioningStyle,
                                        std::chrono::system_clock::to_time_t(syncStartTime));

            DeletionHandler delHandlerR(baseFolder.getAbstractPath<SelectSide::right>(),
                                        recyclerMissingReportOnce,
                                        warnings.warnRecyclerMissing,
                                        folderPairCfg.handleDeletion,
                                        versioningFolderPath,
                                        folderPairCfg.versioningStyle,
                                        std::chrono::system_clock::to_time_t(syncStartTime));

            //always (try to) clean up, even if synchronization is aborted!
            auto guardDelCleanup = makeGuard<ScopeGuardRunMode::onFail>([&]
            {
                delHandlerL.tryCleanup(callbackNoThrow);
                delHandlerR.tryCleanup(callbackNoThrow);
            });


            FolderPairSyncer::SyncCtx syncCtx =
            {
                verifyCopiedFiles, copyPermissionsFp, failSafeFileCopy,
                delHandlerL, delHandlerR,
            };
            FolderPairSyncer::runSync(syncCtx, baseFolder, callback);

            //(try to gracefully) clean up temporary Recycle Bin folders and versioning
            delHandlerL.tryCleanup(callback); //throw X
            delHandlerR.tryCleanup(callback); //
            guardDelCleanup.dismiss();

            if (folderPairCfg.handleDeletion == DeletionVariant::versioning &&
                folderPairCfg.versioningStyle != VersioningStyle::replace)
                versionLimitFolders.insert(
            {
                versioningFolderPath,
                folderPairCfg.versionMaxAgeDays,
                folderPairCfg.versionCountMin,
                folderPairCfg.versionCountMax
            });

            //(try to gracefully) write database file
            if (folderPairCfg.saveSyncDB)
            {
                saveLastSynchronousState(baseFolder, failSafeFileCopy,
                                         callback /*throw X*/); //throw X
                guardDbSave.dismiss(); //[!] dismiss *after* "graceful" try: user might cancel during DB write: ensure DB is still written
            }
        }
        //-----------------------------------------------------------------------------------------------------

        applyVersioningLimit(versionLimitFolders,
                             callback /*throw X*/); //throw X
    }
    catch (const std::exception& e)
    {
        callback.reportFatalError(utfTo<std::wstring>(e.what()));
    }
}
