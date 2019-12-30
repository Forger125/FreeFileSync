// *****************************************************************************
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
#include "dir_exist_async.h"
#include "status_handler_impl.h"
#include "versioning.h"
#include "binary.h"
#include "../fs/concrete.h"
#include "../fs/native.h"

    #include <unistd.h> //fsync
    #include <fcntl.h>  //open

using namespace zen;
using namespace fff;


namespace
{
inline
int getCUD(const SyncStatistics& stat)
{
    return stat.createCount() +
           stat.updateCount() +
           stat.deleteCount();
}
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
    for (const SymlinkPair& link : hierObj.refSubLinks())
        processLink(link);
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
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_MOVE_LEFT_TO:
            ++updateLeft_;
            //physicalDeleteLeft_ ? -> usually, no; except when falling back to "copy + delete"
            break;

        case SO_MOVE_RIGHT_TO:
            ++updateRight_;
            break;

        case SO_MOVE_LEFT_FROM:  //ignore; already counted
        case SO_MOVE_RIGHT_FROM: //
            break;

        case SO_OVERWRITE_LEFT:
            ++updateLeft_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
            physicalDeleteLeft_ = true;
            break;

        case SO_OVERWRITE_RIGHT:
            ++updateRight_;
            bytesToProcess_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ file.getRelativePathAny(), file.getSyncOpConflict() });
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
void SyncStatistics::processLink(const SymlinkPair& link)
{
    switch (link.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_CREATE_NEW_LEFT:
            ++createLeft_;
            break;

        case SO_CREATE_NEW_RIGHT:
            ++createRight_;
            break;

        case SO_DELETE_LEFT:
            ++deleteLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
            ++updateLeft_;
            physicalDeleteLeft_ = true;
            break;

        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
            ++updateRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ link.getRelativePathAny(), link.getSyncOpConflict() });
            break;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
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
            physicalDeleteLeft_ = true;
            break;

        case SO_DELETE_RIGHT:
            ++deleteRight_;
            physicalDeleteRight_ = true;
            break;

        case SO_UNRESOLVED_CONFLICT:
            conflictMsgs_.push_back({ folder.getRelativePathAny(), folder.getSyncOpConflict() });
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
        case SO_DO_NOTHING:
        case SO_EQUAL:
            break;
    }

    recurse(folder); //since we model logical stats, we recurse, even if deletion variant is "recycler" or "versioning + same volume", which is a single physical operation!
}


/*
  DeletionPolicy::PERMANENT:  deletion frees space
  DeletionPolicy::RECYCLER:   won't free space until recycler is full, but then frees space
  DeletionPolicy::VERSIONING: depends on whether versioning folder is on a different volume
-> if deleted item is a followed symlink, no space is freed
-> created/updated/deleted item may be on a different volume than base directory: consider symlinks, junctions!

=> generally assume deletion frees space; may avoid false-positive disk space warnings for recycler and versioning
*/
class MinimumDiskSpaceNeeded
{
public:
    static std::pair<int64_t, int64_t> calculate(const BaseFolderPair& baseFolder)
    {
        MinimumDiskSpaceNeeded inst;
        inst.recurse(baseFolder);
        return { inst.spaceNeededLeft_, inst.spaceNeededRight_ };
    }

private:
    void recurse(const ContainerObject& hierObj)
    {
        //process files
        for (const FilePair& file : hierObj.refSubFiles())
            switch (file.getSyncOperation()) //evaluate comparison result and sync direction
            {
                case SO_CREATE_NEW_LEFT:
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_CREATE_NEW_RIGHT:
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    break;

                case SO_DELETE_LEFT:
                    if (!file.isFollowedSymlink<LEFT_SIDE>())
                        spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    break;

                case SO_DELETE_RIGHT:
                    if (!file.isFollowedSymlink<RIGHT_SIDE>())
                        spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_OVERWRITE_LEFT:
                    if (!file.isFollowedSymlink<LEFT_SIDE>())
                        spaceNeededLeft_ -= static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
                    spaceNeededLeft_ += static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    break;

                case SO_OVERWRITE_RIGHT:
                    if (!file.isFollowedSymlink<RIGHT_SIDE>())
                        spaceNeededRight_ -= static_cast<int64_t>(file.getFileSize<RIGHT_SIDE>());
                    spaceNeededRight_ += static_cast<int64_t>(file.getFileSize<LEFT_SIDE>());
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
                    if (!folder.isFollowedSymlink<LEFT_SIDE>())
                        recurse(folder); //not 100% correct: in fact more that what our model contains may be deleted (consider file filter!)
                    break;
                case SO_DELETE_RIGHT:
                    if (!folder.isFollowedSymlink<RIGHT_SIDE>())
                        recurse(folder);
                    break;

                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                    assert(false);
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
    std::vector<LocalPairConfig> localCfgs = { mainCfg.firstPair };
    append(localCfgs, mainCfg.additionalPairs);

    std::vector<FolderPairSyncCfg> output;

    for (const LocalPairConfig& lpc : localCfgs)
    {
        //const CompConfig cmpCfg  = lpc.localCmpCfg  ? *lpc.localCmpCfg  : mainCfg.cmpCfg;
        const SyncConfig syncCfg = lpc.localSyncCfg ? *lpc.localSyncCfg : mainCfg.syncCfg;

        output.push_back(
        {
            syncCfg.directionCfg.var,
            syncCfg.directionCfg.var == DirectionConfig::TWO_WAY || detectMovedFilesEnabled(syncCfg.directionCfg),

            syncCfg.handleDeletion,
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
std::optional<SelectedSide> getTargetDirection(SyncOperation syncOp)
{
    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_DELETE_LEFT:
        case SO_OVERWRITE_LEFT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_LEFT_TO:
            return LEFT_SIDE;

        case SO_CREATE_NEW_RIGHT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_RIGHT:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_RIGHT_TO:
            return RIGHT_SIDE;

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
    if ((folderPairStat.createCount< LEFT_SIDE>() == 0 ||
         folderPairStat.createCount<RIGHT_SIDE>() == 0) &&
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

//#################################################################################################################

//--------------------- data verification -------------------------
void flushFileBuffers(const Zstring& nativeFilePath) //throw FileError
{
    const int fileHandle = ::open(nativeFilePath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fileHandle == -1)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot open file %x."), L"%x", fmtPath(nativeFilePath)), L"open");
    ZEN_ON_SCOPE_EXIT(::close(fileHandle));

    if (::fsync(fileHandle) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(nativeFilePath)), L"fsync");
}


void verifyFiles(const AbstractPath& sourcePath, const AbstractPath& targetPath, const IOCallback& notifyUnbufferedIO) //throw FileError
{
    try
    {
        //do like "copy /v": 1. flush target file buffers, 2. read again as usual (using OS buffers)
        // => it seems OS buffers are not invalidated by this: snake oil???
        if (std::optional<Zstring> nativeTargetPath = AFS::getNativeItemPath(targetPath))
            flushFileBuffers(*nativeTargetPath); //throw FileError

        if (!filesHaveSameContent(sourcePath, targetPath, notifyUnbufferedIO)) //throw FileError
            throw FileError(replaceCpy(replaceCpy(_("%x and %y have different content."),
                                                  L"%x", L"\n" + fmtPath(AFS::getDisplayPath(sourcePath))),
                                       L"%y", L"\n" + fmtPath(AFS::getDisplayPath(targetPath))));
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
AFS::ItemType getItemType(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::getItemType(ap); /*throw FileError*/ }, singleThread); }

inline
std::optional<AFS::ItemType> itemStillExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::itemStillExists(ap); /*throw FileError*/ }, singleThread); }

inline
bool removeFileIfExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::removeFileIfExists(ap); /*throw FileError*/ }, singleThread); }

inline
bool removeSymlinkIfExists(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::removeSymlinkIfExists(ap); /*throw FileError*/ }, singleThread); }

inline
void moveAndRenameItem(const AbstractPath& apSource, const AbstractPath& apTarget, std::mutex& singleThread) //throw FileError, ErrorDifferentVolume
{ parallelScope([apSource, apTarget] { AFS::moveAndRenameItem(apSource, apTarget); /*throw FileError, ErrorDifferentVolume*/ }, singleThread); }

inline
AbstractPath getSymlinkResolvedPath(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ return parallelScope([ap] { return AFS::getSymlinkResolvedPath(ap); /*throw FileError*/ }, singleThread); }

inline
void copySymlink(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ parallelScope([apSource, apTarget, copyFilePermissions] { AFS::copySymlink(apSource, apTarget, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
void copyNewFolder(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions, std::mutex& singleThread) //throw FileError
{ parallelScope([apSource, apTarget, copyFilePermissions] { AFS::copyNewFolder(apSource, apTarget, copyFilePermissions); /*throw FileError*/ }, singleThread); }

inline
void removeFilePlain(const AbstractPath& ap, std::mutex& singleThread) //throw FileError
{ parallelScope([ap] { AFS::removeFilePlain(ap); /*throw FileError*/ }, singleThread); }

//--------------------------------------------------------------
//ATTENTION CALLBACKS: they also run asynchronously *outside* the singleThread lock!
//--------------------------------------------------------------
inline
void removeFolderIfExistsRecursion(const AbstractPath& ap, //throw FileError
                                   const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion, //optional
                                   const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion, //one call for each object!
                                   std::mutex& singleThread)
{ parallelScope([ap, onBeforeFileDeletion, onBeforeFolderDeletion] { AFS::removeFolderIfExistsRecursion(ap, onBeforeFileDeletion, onBeforeFolderDeletion); /*throw FileError*/ }, singleThread); }


inline
AFS::FileCopyResult copyFileTransactional(const AbstractPath& apSource, const AFS::StreamAttributes& attrSource, //throw FileError, ErrorFileLocked
                                          const AbstractPath& apTarget,
                                          bool copyFilePermissions,
                                          bool transactionalCopy,
                                          const std::function<void()>& onDeleteTargetFile,
                                          const IOCallback& notifyUnbufferedIO,
                                          std::mutex& singleThread)
{
    return parallelScope([=]
    {
        return AFS::copyFileTransactional(apSource, attrSource, apTarget, copyFilePermissions, transactionalCopy, onDeleteTargetFile, notifyUnbufferedIO); //throw FileError, ErrorFileLocked
    }, singleThread);
}

inline //RecycleSession::recycleItem() is internally synchronized!
bool recycleItem(AFS::RecycleSession& recyclerSession, const AbstractPath& ap, const Zstring& logicalRelPath, std::mutex& singleThread) //throw FileError
{ return parallelScope([=, &recyclerSession] { return recyclerSession.recycleItem(ap, logicalRelPath); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionFile() is internally synchronized!
void revisionFile(FileVersioner& versioner, const FileDescriptor& fileDescr, const Zstring& relativePath, const IOCallback& notifyUnbufferedIO, std::mutex& singleThread) //throw FileError
{ parallelScope([=, &versioner] { return versioner.revisionFile(fileDescr, relativePath, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionSymlink() is internally synchronized!
void revisionSymlink(FileVersioner& versioner, const AbstractPath& linkPath, const Zstring& relativePath, std::mutex& singleThread) //throw FileError
{ parallelScope([=, &versioner] { return versioner.revisionSymlink(linkPath, relativePath); /*throw FileError*/ }, singleThread); }

inline //FileVersioner::revisionFolder() is internally synchronized!
void revisionFolder(FileVersioner& versioner,
                    const AbstractPath& folderPath, const Zstring& relativePath, //throw FileError
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFileMove,
                    const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFolderMove,
                    const IOCallback& notifyUnbufferedIO,
                    std::mutex& singleThread)
{ parallelScope([=, &versioner] { versioner.revisionFolder(folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

inline
void verifyFiles(const AbstractPath& apSource, const AbstractPath& apTarget, const IOCallback& notifyUnbufferedIO, std::mutex& singleThread) //throw FileError
{ parallelScope([=] { ::verifyFiles(apSource, apTarget, notifyUnbufferedIO); /*throw FileError*/ }, singleThread); }

}

//#################################################################################################################
//#################################################################################################################

class DeletionHandler //abstract deletion variants: permanently, recycle bin, user-defined directory
{
public:
    DeletionHandler(const AbstractPath& baseFolderPath, //nothrow!
                    DeletionPolicy deletionPolicy,
                    const AbstractPath& versioningFolderPath,
                    VersioningStyle versioningStyle,
                    time_t syncStartTime);

    //clean-up temporary directory (recycle bin optimization)
    void tryCleanup(ProcessCallback& cb /*throw X*/, bool allowCallbackException); //throw FileError -> call this in non-exceptional code path, i.e. somewhere after sync!

    void removeDirWithCallback (const AbstractPath&   dirPath,   const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //
    void removeFileWithCallback(const FileDescriptor& fileDescr, const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //throw FileError, ThreadInterruption
    void removeLinkWithCallback(const AbstractPath&   linkPath,  const Zstring& relativePath, AsyncItemStatReporter& statReporter, std::mutex& singleThread); //

    const std::wstring& getTxtRemovingFile   () const { return txtRemovingFile_;    } //
    const std::wstring& getTxtRemovingFolder () const { return txtRemovingFolder_;  } //buffered status texts
    const std::wstring& getTxtRemovingSymLink() const { return txtRemovingSymlink_; } //

private:
    DeletionHandler           (const DeletionHandler&) = delete;
    DeletionHandler& operator=(const DeletionHandler&) = delete;

    AFS::RecycleSession& getOrCreateRecyclerSession() //throw FileError => dont create in constructor!!!
    {
        assert(deletionPolicy_ == DeletionPolicy::RECYCLER);
        if (!recyclerSession_)
            recyclerSession_ =  AFS::createRecyclerSession(baseFolderPath_); //throw FileError
        return *recyclerSession_;
    }

    FileVersioner& getOrCreateVersioner() //throw FileError => dont create in constructor!!!
    {
        assert(deletionPolicy_ == DeletionPolicy::VERSIONING);
        if (!versioner_)
            versioner_ = std::make_unique<FileVersioner>(versioningFolderPath_, versioningStyle_, syncStartTime_); //throw FileError
        return *versioner_;
    }

    const DeletionPolicy deletionPolicy_; //keep it invariant! e.g. consider getOrCreateVersioner() one-time construction!

    const AbstractPath baseFolderPath_;
    std::unique_ptr<AFS::RecycleSession> recyclerSession_;

    //used only for DeletionPolicy::VERSIONING:
    const AbstractPath versioningFolderPath_;
    const VersioningStyle versioningStyle_;
    const time_t syncStartTime_;
    std::unique_ptr<FileVersioner> versioner_;

    //buffer status texts:
    const std::wstring txtRemovingFile_;
    const std::wstring txtRemovingSymlink_;
    const std::wstring txtRemovingFolder_;
    const std::wstring txtMovingFileXtoY_   = _("Moving file %x to %y");
    const std::wstring txtMovingFolderXtoY_ = _("Moving folder %x to %y");
};


DeletionHandler::DeletionHandler(const AbstractPath& baseFolderPath, //nothrow!
                                 DeletionPolicy deletionPolicy,
                                 const AbstractPath& versioningFolderPath,
                                 VersioningStyle versioningStyle,
                                 time_t syncStartTime) :
    deletionPolicy_(deletionPolicy),
    baseFolderPath_(baseFolderPath),
    versioningFolderPath_(versioningFolderPath),
    versioningStyle_(versioningStyle),
    syncStartTime_(syncStartTime),
    txtRemovingFile_([&]
{
    switch (deletionPolicy)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting file %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving file %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving file %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()),
txtRemovingSymlink_([&]
{
    switch (deletionPolicy)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting symbolic link %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving symbolic link %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving symbolic link %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()),
txtRemovingFolder_([&]
{
    switch (deletionPolicy)
    {
        case DeletionPolicy::PERMANENT:
            return _("Deleting folder %x");
        case DeletionPolicy::RECYCLER:
            return _("Moving folder %x to the recycle bin");
        case DeletionPolicy::VERSIONING:
            return replaceCpy(_("Moving folder %x to %y"), L"%y", fmtPath(AFS::getDisplayPath(versioningFolderPath_)));
    }
    return std::wstring();
}()) {}


void DeletionHandler::tryCleanup(ProcessCallback& cb /*throw X*/, bool allowCallbackException) //throw FileError
{
    assert(runningMainThread());
    switch (deletionPolicy_)
    {
        case DeletionPolicy::RECYCLER:
            if (recyclerSession_)
            {
                auto notifyDeletionStatus = [&](const std::wstring& displayPath)
                {
                    try
                    {
                        if (!displayPath.empty())
                            cb.reportStatus(replaceCpy(txtRemovingFile_, L"%x", fmtPath(displayPath))); //throw X
                        else
                            cb.requestUiRefresh(); //throw X
                    }
                    catch (...)
                    {
                        if (allowCallbackException)
                            throw;
                    }
                };

                //move content of temporary directory to recycle bin in a single call
                recyclerSession_->tryCleanup(notifyDeletionStatus); //throw FileError
            }
            break;

        case DeletionPolicy::PERMANENT:
        case DeletionPolicy::VERSIONING:
            break;
    }
}


void DeletionHandler::removeDirWithCallback(const AbstractPath& folderPath,//throw FileError, ThreadInterruption
                                            const Zstring& relativePath,
                                            AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{
    switch (deletionPolicy_)
    {
        case DeletionPolicy::PERMANENT:
        {
            //callbacks run *outside* singleThread_ lock! => fine
            auto notifyDeletion = [&statReporter](const std::wstring& statusText, const std::wstring& displayPath)
            {
                statReporter.reportStatus(replaceCpy(statusText, L"%x", fmtPath(displayPath))); //throw ThreadInterruption
                statReporter.reportDelta(1, 0); //it would be more correct to report *after* work was done!
                //OTOH: ThreadInterruption must not happen after last deletion was successful: allow for transactional file model update!
                warn_static("=> indeed; fix!?")
            };
            static_assert(std::is_const_v<decltype(txtRemovingFile_)>, "callbacks better be thread-safe!");
            auto onBeforeFileDeletion = [&](const std::wstring& displayPath) { notifyDeletion(txtRemovingFile_,   displayPath); };
            auto onBeforeDirDeletion  = [&](const std::wstring& displayPath) { notifyDeletion(txtRemovingFolder_, displayPath); };

            parallel::removeFolderIfExistsRecursion(folderPath, onBeforeFileDeletion, onBeforeDirDeletion, singleThread); //throw FileError
        }
        break;

        case DeletionPolicy::RECYCLER:
            parallel::recycleItem(getOrCreateRecyclerSession(), folderPath, relativePath, singleThread); //throw FileError
            statReporter.reportDelta(1, 0); //moving to recycler is ONE logical operation, irrespective of the number of child elements!
            break;

        case DeletionPolicy::VERSIONING:
        {
            //callbacks run *outside* singleThread_ lock! => fine
            auto notifyMove = [&statReporter](const std::wstring& statusText, const std::wstring& displayPathFrom, const std::wstring& displayPathTo)
            {
                statReporter.reportStatus(replaceCpy(replaceCpy(statusText, L"%x", L"\n" + fmtPath(displayPathFrom)), L"%y", L"\n" + fmtPath(displayPathTo))); //throw ThreadInterruption
                statReporter.reportDelta(1, 0); //it would be more correct to report *after* work was done!
                warn_static("=> indeed; fix!?")
            };
            static_assert(std::is_const_v<decltype(txtMovingFileXtoY_)>, "callbacks better be thread-safe!");
            auto onBeforeFileMove   = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFileXtoY_,   displayPathFrom, displayPathTo); };
            auto onBeforeFolderMove = [&](const std::wstring& displayPathFrom, const std::wstring& displayPathTo) { notifyMove(txtMovingFolderXtoY_, displayPathFrom, displayPathTo); };
            auto notifyUnbufferedIO = [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); interruptionPoint(); }; //throw ThreadInterruption

            parallel::revisionFolder(getOrCreateVersioner(), folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO, singleThread); //throw FileError, ThreadInterruption
        }
        break;
    }
}


void DeletionHandler::removeFileWithCallback(const FileDescriptor& fileDescr, //throw FileError, ThreadInterruption
                                             const Zstring& relativePath,
                                             AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{

    if (endsWith(relativePath, AFS::TEMP_FILE_ENDING)) //special rule for .ffs_tmp files: always delete permanently!
        parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
    else
        switch (deletionPolicy_)
        {
            case DeletionPolicy::PERMANENT:
                parallel::removeFileIfExists(fileDescr.path, singleThread); //throw FileError
                break;
            case DeletionPolicy::RECYCLER:
                parallel::recycleItem(getOrCreateRecyclerSession(), fileDescr.path, relativePath, singleThread); //throw FileError
                break;
            case DeletionPolicy::VERSIONING:
            {
                //callback runs *outside* singleThread_ lock! => fine
                auto notifyUnbufferedIO = [&](int64_t bytesDelta) { statReporter.reportDelta(0, bytesDelta); interruptionPoint(); }; //throw ThreadInterruption

                parallel::revisionFile(getOrCreateVersioner(), fileDescr, relativePath, notifyUnbufferedIO, singleThread); //throw FileError
            }
            break;
        }

    //even if the source item does not exist anymore, significant I/O work was done => report
    //-> also consider unconditional statReporter.reportDelta(-1, 0) when overwriting a file
    statReporter.reportDelta(1, 0);
}


void DeletionHandler::removeLinkWithCallback(const AbstractPath& linkPath, //throw FileError, throw ThreadInterruption
                                             const Zstring& relativePath,
                                             AsyncItemStatReporter& statReporter, std::mutex& singleThread)
{
    switch (deletionPolicy_)
    {
        case DeletionPolicy::PERMANENT:
            parallel::removeSymlinkIfExists(linkPath, singleThread); //throw FileError
            break;
        case DeletionPolicy::RECYCLER:
            parallel::recycleItem(getOrCreateRecyclerSession(), linkPath, relativePath, singleThread); //throw FileError
            break;
        case DeletionPolicy::VERSIONING:
            parallel::revisionSymlink(getOrCreateVersioner(), linkPath, relativePath, singleThread); //throw FileError
            break;
    }
    //remain transactional as much as possible => no more callbacks that can throw after successful deletion! (next: update file model!)

    //report unconditionally, see removeFileWithCallback()
    statReporter.reportDelta(1, 0);
}

//===================================================================================================
//===================================================================================================

class Workload
{
public:
    Workload(size_t threadCount, AsyncCallback& acb) : acb_(acb), workload_(threadCount) { assert(threadCount > 0); }

    using WorkItem  = std::function<void() /*throw ThreadInterruption*/>;
    using WorkItems = RingBuffer<WorkItem>; //FIFO!

    //blocking call: context of worker thread
    WorkItem getNext(size_t threadIdx) //throw ThreadInterruption
    {
        interruptionPoint(); //throw ThreadInterruption

        std::unique_lock<std::mutex> dummy(lockWork_);
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
                    const size_t sz = items.size(); //[!] variable during loop!
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

                    interruptibleWait(conditionNewWork_, dummy, [&] { return haveNewWork(); }); //throw ThreadInterruption
                    //it's sufficient to notify condition in addWorkItems() only (as long as we use std::condition_variable::notify_all())
                }
            }
        }
    }

    void addWorkItems(RingBuffer<WorkItems>&& buckets)
    {
        {
            std::lock_guard<std::mutex> dummy(lockWork_);
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


class FolderPairSyncer
{
public:
    struct SyncCtx
    {
        bool verifyCopiedFiles;
        bool copyFilePermissions;
        bool failSafeFileCopy;
        std::vector<FileError>& errorsModTime;
        DeletionHandler& delHandlerLeft;
        DeletionHandler& delHandlerRight;
        size_t threadCount;
    };

    static void runSync(SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb)
    {
        runPass(PASS_ZERO, syncCtx, baseFolder, cb); //prepare file moves
        runPass(PASS_ONE,  syncCtx, baseFolder, cb); //delete files (or overwrite big ones with smaller ones)
        runPass(PASS_TWO,  syncCtx, baseFolder, cb); //copy rest
    }

private:
    friend class Workload;
    enum PassNo
    {
        PASS_ZERO, //prepare file moves
        PASS_ONE,  //delete files
        PASS_TWO,  //create, modify
        PASS_NEVER //skip item
    };

    FolderPairSyncer(SyncCtx& syncCtx, std::mutex& singleThread, AsyncCallback& acb) :
        errorsModTime_      (syncCtx.errorsModTime),
        delHandlerLeft_     (syncCtx.delHandlerLeft),
        delHandlerRight_    (syncCtx.delHandlerRight),
        verifyCopiedFiles_  (syncCtx.verifyCopiedFiles),
        copyFilePermissions_(syncCtx.copyFilePermissions),
        failSafeFileCopy_   (syncCtx.failSafeFileCopy),
        singleThread_(singleThread),
        acb_(acb) {}

    static PassNo getPass(const FilePair&    file);
    static PassNo getPass(const SymlinkPair& link);
    static PassNo getPass(const FolderPair&  folder);
    static bool needZeroPass(const FilePair& file);

    static void runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb); //throw X

    RingBuffer<Workload::WorkItems> getFolderLevelWorkItems(PassNo pass, ContainerObject& parentFolder, Workload& workload);

    enum class CmtfStatus //CreateMoveTargetFolderStatus
    {
        AVAILABLE,
        NAME_CLASH,
        SOURCE_MISSING
    };
    template <SelectedSide side> void setup2StepMove(FilePair& sourceFile, FilePair& targetFile); //throw FileError, ThreadInterruption
    template <SelectedSide side> CmtfStatus createMoveTargetFolder(FileSystemObject& fsObj); //throw FileError, ThreadInterruption
    template <SelectedSide side> void resolveMoveConflicts(FilePair& sourceFile, FilePair& targetFile); //throw FileError, ThreadInterruption

    void prepareFileMove(FilePair& file); //throw ThreadInterruption

    void synchronizeFile(FilePair& file);                                                       //
    template <SelectedSide side> void synchronizeFileInt(FilePair& file, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void synchronizeLink(SymlinkPair& link);                                                          //
    template <SelectedSide sideTrg> void synchronizeLinkInt(SymlinkPair& link, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void synchronizeFolder(FolderPair& folder);                                                          //
    template <SelectedSide sideTrg> void synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp); //throw FileError, ThreadInterruption

    void reportInfo(const std::wstring& rawText, const std::wstring& displayPath) { acb_.reportInfo(replaceCpy(rawText, L"%x", fmtPath(displayPath))); }
    void reportInfo(const std::wstring& rawText, const std::wstring& displayPath1, const std::wstring& displayPath2) //throw ThreadInterruption
    {
        acb_.reportInfo(replaceCpy(replaceCpy(rawText, L"%x", L"\n" + fmtPath(displayPath1)), L"%y", L"\n" + fmtPath(displayPath2))); //throw ThreadInterruption
    }

    //target existing after onDeleteTargetFile(): undefined behavior! (fail/overwrite/auto-rename)
    AFS::FileCopyResult copyFileWithCallback(const FileDescriptor& sourceDescr, //throw FileError, ThreadInterruption
                                             const AbstractPath& targetPath,
                                             const std::function<void()>& onDeleteTargetFile, //optional!
                                             AsyncItemStatReporter& statReporter);
    std::vector<FileError>& errorsModTime_;

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
    const std::wstring txtSourceItemNotFound_{_("Source item %x not found" )};
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

void FolderPairSyncer::runPass(PassNo pass, SyncCtx& syncCtx, BaseFolderPair& baseFolder, ProcessCallback& cb) //throw X
{
    const size_t threadCount = std::max<size_t>(syncCtx.threadCount, 1);

    std::mutex singleThread; //only a single worker thread may run at a time, except for parallel file I/O

    AsyncCallback acb;                                //
    FolderPairSyncer fps(syncCtx, singleThread, acb); //manage life time: enclose InterruptibleThread's!!!
    Workload workload(threadCount, acb);              //
    workload.addWorkItems(fps.getFolderLevelWorkItems(pass, baseFolder, workload)); //initial workload: set *before* threads get access!

    std::vector<InterruptibleThread> worker;
    ZEN_ON_SCOPE_EXIT( for (InterruptibleThread& wt : worker) wt.join     (); ); //
    ZEN_ON_SCOPE_EXIT( for (InterruptibleThread& wt : worker) wt.interrupt(); ); //interrupt all first, then join

    for (size_t threadIdx = 0; threadIdx < threadCount; ++threadIdx)
        worker.emplace_back([threadIdx, &singleThread, &acb, &workload]
    {
        setCurrentThreadName(("Sync Worker[" + numberTo<std::string>(threadIdx) + "]").c_str());

        while (/*blocking call:*/ std::function<void()> workItem = workload.getNext(threadIdx)) //throw ThreadInterruption
        {
            acb.notifyTaskBegin(0 /*prio*/); //same prio, while processing only one folder pair at a time
            ZEN_ON_SCOPE_EXIT(acb.notifyTaskEnd());

            std::lock_guard<std::mutex> dummy(singleThread); //protect ALL accesses to "fps" and workItem execution!
            workItem(); //throw ThreadInterruption
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

        //synchronize folders:
        for (FolderPair& folder : hierObj.refSubFolders())
            if (pass == getPass(folder))
                workItems.push_back([this, &folder, &workload, pass]
            {
                tryReportingError([&] { synchronizeFolder(folder); }, acb_); //throw ThreadInterruption

                workload.addWorkItems(getFolderLevelWorkItems(pass, folder, workload));
            });
        else
            foldersToInspect.push_back(&folder);

        //synchronize files:
        for (FilePair& file : hierObj.refSubFiles())
            if (pass == PASS_ZERO)
            {
                if (needZeroPass(file))
                    workItems.push_back([this, &file] { prepareFileMove(file); /*throw ThreadInterruption*/ });
            }
            else if (pass == getPass(file))
                workItems.push_back([this, &file]
            {
                tryReportingError([&] { synchronizeFile(file); }, acb_); //throw ThreadInterruption
            });

        //synchronize symbolic links:
        for (SymlinkPair& symlink : hierObj.refSubLinks())
            if (pass == getPass(symlink))
                workItems.push_back([this, &symlink]
            {
                tryReportingError([&] { synchronizeLink(symlink); }, acb_); //throw ThreadInterruption
            });

        if (!workItems.empty())
            buckets.push_back(std::move(workItems));
    }

    return buckets;
}


/*
__________________________
|Move algorithm, 0th pass|
--------------------------
1. loop over hierarchy and find "move source"

2. check whether parent directory of "move source" is going to be deleted or location of "move source" may lead to name clash with other dir/symlink
   -> no:  delay move until 2nd pass

3. create move target's parent directory recursively + execute move
   do we have name clash?
   -> prepare a 2-step move operation: 1. move source to base and update "move target" accordingly 2. delay move until 2nd pass

4. If any of the operations above did not succeed (even after retry), update statistics and revert to "copy + delete"
   Note: first pass may delete "move source"!!!

__________________
|killer-scenarios|
------------------
propagate the following move sequences:
I) a -> a/a      caveat sync'ing parent directory first leads to circular dependency!

II) a/a -> a     caveat: fixing name clash will remove source!

III) c -> d      caveat: move-sequence needs to be processed in correct order!
     b -> c/b
     a -> b/a
*/

template <class List> inline
bool haveNameClash(const Zstring& itemName, const List& m)
{
    return std::any_of(m.begin(), m.end(),
    [&](const typename List::value_type& obj) { return equalNoCase(obj.getItemNameAny(), itemName); }); //equalNoCase: when in doubt => assume name clash!
}


template <SelectedSide side>
void FolderPairSyncer::setup2StepMove(FilePair& sourceFile, //throw FileError, ThreadInterruption
                                      FilePair& targetFile)
{
    //generate (hopefully) unique file name to avoid clashing with some remnant ffs_tmp file
    const Zstring shortGuid = printNumber<Zstring>(Zstr("%04x"), static_cast<unsigned int>(getCrc16(generateGUID())));
    const Zstring fileName = sourceFile.getItemName<side>();
    auto it = findLast(fileName.begin(), fileName.end(), Zstr('.')); //gracefully handle case of missing "."

    const Zstring sourceRelPathTmp = Zstring(fileName.begin(), it) + Zstr('.') + shortGuid + AFS::TEMP_FILE_ENDING;
    //-------------------------------------------------------------------------------------------
    //this could still lead to a name-clash in obscure cases, if some file exists on the other side with
    //the very same (.ffs_tmp) name and is copied before the second step of the move is executed
    //good news: even in this pathologic case, this may only prevent the copy of the other file, but not this move

    const AbstractPath sourcePathTmp = AFS::appendRelPath(sourceFile.base().getAbstractPath<side>(), sourceRelPathTmp);

    reportInfo(txtMovingFileXtoY_, //ThreadInterruption
               AFS::getDisplayPath(sourceFile.getAbstractPath<side>()),
               AFS::getDisplayPath(sourcePathTmp));

    parallel::moveAndRenameItem(sourceFile.getAbstractPath<side>(), sourcePathTmp, singleThread_); //throw FileError, (ErrorDifferentVolume)

    //TODO: prepare2StepMove: consider ErrorDifferentVolume! e.g. symlink aliasing!

    //update file hierarchy
    FilePair& tempFile = sourceFile.base().addSubFile<side>(afterLast(sourceRelPathTmp, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_ALL), sourceFile.getAttributes<side>());
    static_assert(std::is_same_v<ContainerObject::FileList, std::list<FilePair>>,
                  "ATTENTION: we're adding to the file list WHILE looping over it! This is only working because std::list iterators are not invalidated by insertion!");
    sourceFile.removeObject<side>(); //remove only *after* evaluating "sourceFile, side"!
    //note: this new item is *not* considered at the end of 0th pass because "!sourceWillBeDeleted && !haveNameClash"

    //prepare move in second pass
    tempFile.setSyncDir(side == LEFT_SIDE ? SyncDirection::LEFT : SyncDirection::RIGHT);

    targetFile.setMoveRef(tempFile  .getId());
    tempFile  .setMoveRef(targetFile.getId());

    //NO statistics update!
}


//returns: CmtfStatus::AVAILABLE
//         CmtfStatus::NAME_CLASH
//         CmtfStatus::SOURCE_MISSING
template <SelectedSide side>
auto FolderPairSyncer::createMoveTargetFolder(FileSystemObject& fsObj) -> CmtfStatus //throw FileError, ThreadInterruption
{
    if (auto parentFolder = dynamic_cast<FolderPair*>(&fsObj.parent()))
    {
        const CmtfStatus cmtfs = createMoveTargetFolder<side>(*parentFolder);
        if (cmtfs != CmtfStatus::AVAILABLE)
            return cmtfs;

        //detect (and try to resolve) file type conflicts: 1. symlinks 2. files
        const Zstring& folderName = parentFolder->getItemNameAny();
        if (haveNameClash(folderName, parentFolder->parent().refSubLinks()) ||
            haveNameClash(folderName, parentFolder->parent().refSubFiles()))
            return CmtfStatus::NAME_CLASH;

        //-------- create parent folder if needed --------------
        constexpr SelectedSide sideSrc = OtherSide<side>::value;
        assert(!parentFolder->isEmpty<sideSrc>());

        switch (parentFolder->getSyncOperation())
        {
            case SO_CREATE_NEW_LEFT:
            case SO_CREATE_NEW_RIGHT:
            {
                const AbstractPath targetPath = parentFolder->getAbstractPath<side>();
                reportInfo(txtCreatingFolder_, AFS::getDisplayPath(targetPath)); //throw ThreadInterruption

                //shallow-"copying" a folder might not fail if source is missing, so we need to check this first:
                if (parallel::itemStillExists(parentFolder->getAbstractPath<sideSrc>(), singleThread_)) //throw FileError
                {
                    AsyncItemStatReporter statReporter(1, 0, acb_);
                    try //target existing: undefined behavior! (fail/overwrite)
                    {
                        parallel::copyNewFolder(parentFolder->getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError
                    }
                    catch (FileError&)
                    {
                        bool folderAlreadyExists = false;
                        try { folderAlreadyExists = parallel::getItemType(targetPath, singleThread_) == AFS::ItemType::FOLDER; } /*throw FileError*/ catch (FileError&) {}
                        if (!folderAlreadyExists) //previous exception is more relevant; good enough? https://freefilesync.org/forum/viewtopic.php?t=5266
                            throw;
                    }
                    statReporter.reportDelta(1, 0);

                    parentFolder->setSyncedTo<side>(parentFolder->getItemName<sideSrc>(),
                                                    false /*isSymlinkTrg*/,
                                                    parentFolder->isFollowedSymlink<sideSrc>());
                }
                else //source deleted meanwhile...
                {
                    //attention when fixing statistics due to missing folder: child items may be scheduled for move, so deletion will have move-references flip back to copy + delete!
                    const SyncStatistics statsBefore(parentFolder->base()); //=> don't bother considering move operations, just calculate over the whole tree
                    parentFolder->removeObject<sideSrc>(); //DON'T physically delete child objects while we're still evaluating them, e.g. fsObj, and in caller code!!!
                    const SyncStatistics statsAfter(parentFolder->base());

                    acb_.updateDataProcessed(1, 0); //even if the source item does not exist anymore, significant I/O work was done => report
                    acb_.updateDataTotal(getCUD(statsAfter) - getCUD(statsBefore) + 1, statsAfter.getBytesToProcess() - statsBefore.getBytesToProcess()); //noexcept

                    reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(parentFolder->getAbstractPath<sideSrc>())); //throw ThreadInterruption
                    return CmtfStatus::SOURCE_MISSING;
                }
            }
            break;

            case SO_DO_NOTHING:          //!isEmpty<side>(); see FolderPair::getSyncOperation()
            case SO_UNRESOLVED_CONFLICT: //
            case SO_OVERWRITE_LEFT:  //possible: e.g. manually-resolved dir-traversal conflict
            case SO_OVERWRITE_RIGHT: //
            case SO_COPY_METADATA_TO_LEFT:
            case SO_COPY_METADATA_TO_RIGHT:
            case SO_EQUAL:
                assert(!parentFolder->isEmpty<side>());
                break; //we're good

            case SO_DELETE_LEFT:  //not possible in the context of planning to move a child item, see FolderPair::getSyncOperation()
            case SO_DELETE_RIGHT: //
            case SO_MOVE_LEFT_FROM:  //status not possible for folder
            case SO_MOVE_RIGHT_FROM: //
            case SO_MOVE_LEFT_TO:    //
            case SO_MOVE_RIGHT_TO:   //
                assert(false);
                break;
        }
    }
    return CmtfStatus::AVAILABLE;
}


template <SelectedSide side>
void FolderPairSyncer::resolveMoveConflicts(FilePair& sourceFile, //throw FileError, ThreadInterruption
                                            FilePair& targetFile)
{
    assert((sourceFile.getSyncOperation() == SO_MOVE_LEFT_FROM  && targetFile.getSyncOperation() == SO_MOVE_LEFT_TO  && side == LEFT_SIDE) ||
           (sourceFile.getSyncOperation() == SO_MOVE_RIGHT_FROM && targetFile.getSyncOperation() == SO_MOVE_RIGHT_TO && side == RIGHT_SIDE));

    const bool sourceWillBeDeleted = [&]
    {
        if (auto parentFolder = dynamic_cast<const FolderPair*>(&sourceFile.parent()))
            switch (parentFolder->getSyncOperation()) //evaluate comparison result and sync direction
            {
                case SO_DELETE_LEFT:
                case SO_DELETE_RIGHT:
                    return true; //we need to do something about this!
                case SO_MOVE_LEFT_FROM:
                case SO_MOVE_RIGHT_FROM:
                case SO_MOVE_LEFT_TO:
                case SO_MOVE_RIGHT_TO:
                case SO_OVERWRITE_LEFT:
                case SO_OVERWRITE_RIGHT:
                case SO_CREATE_NEW_LEFT:
                case SO_CREATE_NEW_RIGHT:
                case SO_DO_NOTHING:
                case SO_EQUAL:
                case SO_UNRESOLVED_CONFLICT:
                case SO_COPY_METADATA_TO_LEFT:
                case SO_COPY_METADATA_TO_RIGHT:
                    break;
            }
        return false;
    }();

    auto haveNameClash = [](const FilePair& file)
    {
        return ::haveNameClash(file.getItemNameAny(), file.parent().refSubLinks()) ||
               ::haveNameClash(file.getItemNameAny(), file.parent().refSubFolders());
    };

    if (sourceWillBeDeleted || haveNameClash(sourceFile))
    {
        //prepare for move now: - revert to 2-step move on name clashes
        if (haveNameClash(targetFile) ||
            createMoveTargetFolder<side>(targetFile) == CmtfStatus::NAME_CLASH) //throw FileError, ThreadInterruption
            return setup2StepMove<side>(sourceFile, targetFile); //throw FileError, ThreadInterruption

        //finally start move! this should work now:
        synchronizeFile(targetFile); //throw FileError, ThreadInterruption
        //- FolderPairSyncer::synchronizeFileInt() is *not* expecting SO_MOVE_LEFT_FROM/SO_MOVE_RIGHT_FROM => start move from targetFile, not sourceFile!
        //- function call will be NOOP if CmtfStatus::SOURCE_MISSING
    }
    //else: sourceFile will not be deleted, and is not standing in the way => delay to second pass
    //note: this also applies for new "move sources" from two-step sub-routine!!!
}


void FolderPairSyncer::prepareFileMove(FilePair& file) //throw ThreadInterruption
{
    const SyncOperation syncOp = file.getSyncOperation();
    switch (syncOp) //evaluate comparison result and sync direction
    {
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
            if (FilePair* targetObj = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                FilePair* sourceObj = &file;
                assert(targetObj->getMoveRef() == sourceObj->getId());

                const std::wstring errMsg = tryReportingError([&] //throw ThreadInterruption
                {
                    if (syncOp == SO_MOVE_LEFT_FROM)
                        resolveMoveConflicts<LEFT_SIDE>(*sourceObj, *targetObj); //throw FileError, ThreadInterruption
                    else
                        resolveMoveConflicts<RIGHT_SIDE>(*sourceObj, *targetObj); //
                }, acb_); //throw ThreadInterruption

                if (!errMsg.empty())
                {
                    //move operation has failed! We cannot allow to continue and have move source's parent directory deleted, messing up statistics!
                    // => revert to ordinary "copy + delete"
                    auto getStats = [&]() -> std::pair<int, int64_t>
                    {
                        SyncStatistics statSrc(*sourceObj);
                        SyncStatistics statTrg(*targetObj);
                        return { getCUD(statSrc) + getCUD(statTrg), statSrc.getBytesToProcess() + statTrg.getBytesToProcess() };
                    };
                    const auto [itemsBefore, bytesBefore] = getStats();
                    sourceObj->setMoveRef(nullptr);
                    targetObj->setMoveRef(nullptr);
                    const auto [itemsAfter, bytesAfter] = getStats();

                    //fix statistics total to match "copy + delete"
                    acb_.updateDataTotal(itemsAfter - itemsBefore, bytesAfter - bytesBefore); //noexcept
                }
            }
            else assert(false);
            break;

        case SO_MOVE_LEFT_TO:  //it's enough to try each move-pair *once*
        case SO_MOVE_RIGHT_TO: //
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            break;
    }
}

//---------------------------------------------------------------------------------------------------------------

inline
bool FolderPairSyncer::needZeroPass(const FilePair& file)
{
    switch (file.getSyncOperation())
    {
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
            return true;

        case SO_MOVE_LEFT_TO:  //it's enough to try each move-pair *once*
        case SO_MOVE_RIGHT_TO: //
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            break;
    }
    return false;
}

//1st, 2nd pass requirements:
// - avoid disk space shortage: 1. delete files, 2. overwrite big with small files first
// - support change in type: overwrite file by directory, symlink by file, etc.

inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FilePair& file)
{
    switch (file.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE;

        case SO_OVERWRITE_LEFT:
            return file.getFileSize<LEFT_SIDE>() > file.getFileSize<RIGHT_SIDE>() ? PASS_ONE : PASS_TWO;

        case SO_OVERWRITE_RIGHT:
            return file.getFileSize<LEFT_SIDE>() < file.getFileSize<RIGHT_SIDE>() ? PASS_ONE : PASS_TWO;

        case SO_MOVE_LEFT_FROM:  //
        case SO_MOVE_RIGHT_FROM: // [!]
            return PASS_NEVER;
        case SO_MOVE_LEFT_TO:  //
        case SO_MOVE_RIGHT_TO: //make sure 2-step move is processed in second pass, after move *target* parent directory was created!
            return PASS_TWO;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const SymlinkPair& link)
{
    switch (link.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE; //make sure to delete symlinks in first pass, and equally named file or dir in second pass: usecase "overwrite symlink with regular file"!

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}


inline
FolderPairSyncer::PassNo FolderPairSyncer::getPass(const FolderPair& folder)
{
    switch (folder.getSyncOperation()) //evaluate comparison result and sync direction
    {
        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            return PASS_ONE;

        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            return PASS_TWO;

        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            assert(false);
        case SO_DO_NOTHING:
        case SO_EQUAL:
        case SO_UNRESOLVED_CONFLICT:
            return PASS_NEVER;
    }
    assert(false);
    return PASS_NEVER; //dummy
}

//---------------------------------------------------------------------------------------------------------------

inline
void FolderPairSyncer::synchronizeFile(FilePair& file) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = file.getSyncOperation();

    if (std::optional<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeFileInt<LEFT_SIDE>(file, syncOp);
        else
            synchronizeFileInt<RIGHT_SIDE>(file, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeFileInt(FilePair& file, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    constexpr SelectedSide sideSrc = OtherSide<sideTrg>::value;
    DeletionHandler& delHandlerTrg = SelectParam<sideTrg>::ref(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&file.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = file.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingFile_, AFS::getDisplayPath(targetPath)); //throw ThreadInterruption

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), acb_);
            try
            {
                const AFS::FileCopyResult result = copyFileWithCallback({ file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>() },
                                                                        targetPath,
                                                                        nullptr, //onDeleteTargetFile: nothing to delete; if existing: undefined behavior! (fail/overwrite/auto-rename)
                                                                        statReporter); //throw FileError, ThreadInterruption
                if (result.errorModTime)
                    errorsModTime_.push_back(*result.errorModTime); //show all warnings later as a single message

                statReporter.reportDelta(1, 0);

                //update FilePair
                file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                          result.modTime, //target time set from source
                                          result.modTime,
                                          result.targetFileId,
                                          result.sourceFileId,
                                          false, file.isFollowedSymlink<sideSrc>());
            }
            catch (const FileError& e)
            {
                bool sourceWasDeleted = false;
                try { sourceWasDeleted = !parallel::itemStillExists(file.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                catch (const FileError& e2) { throw FileError(e.toString(), e2.toString()); } //unclear which exception is more relevant
                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!

                if (sourceWasDeleted)
                {
                    statReporter.reportDelta(1, 0); //even if the source item does not exist anymore, significant I/O work was done => report
                    file.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)

                    reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(file.getAbstractPath<sideSrc>())); //throw ThreadInterruption
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlerTrg.getTxtRemovingFile(), AFS::getDisplayPath(file.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                delHandlerTrg.removeFileWithCallback({ file.getAbstractPath<sideTrg>(), file.getAttributes<sideTrg>() },
                                                     file.getRelativePath<sideTrg>(), statReporter, singleThread_); //throw FileError, X
                file.removeObject<sideTrg>(); //update FilePair
            }
            break;

        case SO_MOVE_LEFT_TO:
        case SO_MOVE_RIGHT_TO:
            if (FilePair* moveFrom = dynamic_cast<FilePair*>(FileSystemObject::retrieve(file.getMoveRef())))
            {
                FilePair* moveTo = &file;
                assert(moveFrom->getMoveRef() == moveTo->getId());

                assert((moveFrom->getSyncOperation() == SO_MOVE_LEFT_FROM  && moveTo->getSyncOperation() == SO_MOVE_LEFT_TO  && sideTrg == LEFT_SIDE) ||
                       (moveFrom->getSyncOperation() == SO_MOVE_RIGHT_FROM && moveTo->getSyncOperation() == SO_MOVE_RIGHT_TO && sideTrg == RIGHT_SIDE));

                const AbstractPath pathFrom = moveFrom->getAbstractPath<sideTrg>();
                const AbstractPath pathTo   = moveTo  ->getAbstractPath<sideTrg>();

                reportInfo(txtMovingFileXtoY_, AFS::getDisplayPath(pathFrom), AFS::getDisplayPath(pathTo)); //throw ThreadInterruption

                AsyncItemStatReporter statReporter(1, 0, acb_);

                //TODO: synchronizeFileInt: consider ErrorDifferentVolume! e.g. symlink aliasing!

                parallel::moveAndRenameItem(pathFrom, pathTo, singleThread_); //throw FileError, (ErrorDifferentVolume)

                statReporter.reportDelta(1, 0);

                //update FilePair
                assert(moveFrom->getFileSize<sideTrg>() == moveTo->getFileSize<sideSrc>());
                moveTo->setSyncedTo<sideTrg>(moveTo->getItemName<sideSrc>(), moveTo->getFileSize<sideSrc>(),
                                             moveFrom->getLastWriteTime<sideTrg>(), //awkward naming! moveFrom is renamed on "sideTrg" side!
                                             moveTo  ->getLastWriteTime<sideSrc>(),
                                             moveFrom->getFileId<sideTrg>(),
                                             moveTo  ->getFileId<sideSrc>(),
                                             moveFrom->isFollowedSymlink<sideTrg>(),
                                             moveTo  ->isFollowedSymlink<sideSrc>());
                moveFrom->removeObject<sideTrg>(); //remove only *after* evaluating "moveFrom, sideTrg"!
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

            reportInfo(txtUpdatingFile_, AFS::getDisplayPath(targetPathResolvedOld)); //throw ThreadInterruption

            AsyncItemStatReporter statReporter(1, file.getFileSize<sideSrc>(), acb_);

            if (file.isFollowedSymlink<sideTrg>()) //since we follow the link, we need to sync case sensitivity of the link manually!
                if (getUnicodeNormalForm(file.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(file.getItemName<sideSrc>())) //have difference in case?
                    parallel::moveAndRenameItem(file.getAbstractPath<sideTrg>(), targetPathLogical, singleThread_); //throw FileError, (ErrorDifferentVolume)

            auto onDeleteTargetFile = [&] //delete target at appropriate time
            {
                //reportStatus(this->delHandlerTrg.getTxtRemovingFile(), AFS::getDisplayPath(targetPathResolvedOld)); -> superfluous/confuses user

                FileAttributes followedTargetAttr = file.getAttributes<sideTrg>();
                followedTargetAttr.isFollowedSymlink = false;

                delHandlerTrg.removeFileWithCallback({ targetPathResolvedOld, followedTargetAttr }, file.getRelativePath<sideTrg>(), statReporter, singleThread_); //throw FileError, X
                //no (logical) item count update desired - but total byte count may change, e.g. move(copy) old file to versioning dir
                statReporter.reportDelta(-1, 0); //undo item stats reporting within DeletionHandler::removeFileWithCallback()

                //file.removeObject<sideTrg>(); -> doesn't make sense for isFollowedSymlink(); "file, sideTrg" evaluated below!

                //if fail-safe file copy is active, then the next operation will be a simple "rename"
                //=> don't risk reportStatus() throwing ThreadInterruption() leaving the target deleted rather than updated!
                //=> if failSafeFileCopy_ : don't run callbacks that could throw
            };

            const AFS::FileCopyResult result = copyFileWithCallback({ file.getAbstractPath<sideSrc>(), file.getAttributes<sideSrc>() },
                                                                    targetPathResolvedNew,
                                                                    onDeleteTargetFile,
                                                                    statReporter); //throw FileError, ThreadInterruption
            if (result.errorModTime)
                errorsModTime_.push_back(*result.errorModTime); //show all warnings later as a single message

            statReporter.reportDelta(1, 0); //we model "delete + copy" as ONE logical operation

            //update FilePair
            file.setSyncedTo<sideTrg>(file.getItemName<sideSrc>(), result.fileSize,
                                      result.modTime, //target time set from source
                                      result.modTime,
                                      result.targetFileId,
                                      result.sourceFileId,
                                      file.isFollowedSymlink<sideTrg>(),
                                      file.isFollowedSymlink<sideSrc>());
        }
        break;

        case SO_COPY_METADATA_TO_LEFT:
        case SO_COPY_METADATA_TO_RIGHT:
            //harmonize with file_hierarchy.cpp::getSyncOpDescription!!
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(file.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(file.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(file.getItemName<sideSrc>())) //have difference in case?
                    parallel::moveAndRenameItem(file.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
                                                AFS::appendRelPath(file.parent().getAbstractPath<sideTrg>(), file.getItemName<sideSrc>()), singleThread_);
                else
                    assert(false);

#if 0 //changing file time without copying content is not justified after CompareVariant::SIZE finds "equal" files! similar issue with CompareVariant::TIME_SIZE and FileTimeTolerance == -1
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
                                          file.getLastWriteTime<sideTrg>(),
                                          file.getLastWriteTime<sideSrc>(),
                                          file.getFileId       <sideTrg>(),
                                          file.getFileId       <sideSrc>(),
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
void FolderPairSyncer::synchronizeLink(SymlinkPair& link) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = link.getSyncOperation();

    if (std::optional<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeLinkInt<LEFT_SIDE>(link, syncOp);
        else
            synchronizeLinkInt<RIGHT_SIDE>(link, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeLinkInt(SymlinkPair& symlink, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    constexpr SelectedSide sideSrc = OtherSide<sideTrg>::value;
    DeletionHandler& delHandlerTrg = SelectParam<sideTrg>::ref(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&symlink.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = symlink.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingLink_, AFS::getDisplayPath(targetPath)); //throw ThreadInterruption

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
                try { sourceExists = !!parallel::itemStillExists(symlink.getAbstractPath<sideSrc>(), singleThread_); /*throw FileError*/ }
                catch (const FileError& e2) { throw FileError(e.toString(), e2.toString()); } //unclear which exception is more relevant
                //do not check on type (symlink, file, folder) -> if there is a type change, FFS should not be quiet about it!

                if (!sourceExists)
                {
                    //even if the source item does not exist anymore, significant I/O work was done => report
                    statReporter.reportDelta(1, 0);
                    symlink.removeObject<sideSrc>(); //source deleted meanwhile...nothing was done (logical point of view!)

                    reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(symlink.getAbstractPath<sideSrc>())); //throw ThreadInterruption
                }
                else
                    throw;
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlerTrg.getTxtRemovingSymLink(), AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                delHandlerTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getRelativePath<sideTrg>(), statReporter, singleThread_); //throw FileError, X

                symlink.removeObject<sideTrg>(); //update SymlinkPair
            }
            break;

        case SO_OVERWRITE_LEFT:
        case SO_OVERWRITE_RIGHT:
            reportInfo(txtUpdatingLink_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                //reportStatus(delHandlerTrg.getTxtRemovingSymLink(), AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>()));
                delHandlerTrg.removeLinkWithCallback(symlink.getAbstractPath<sideTrg>(), symlink.getRelativePath<sideTrg>(), statReporter, singleThread_); //throw FileError, X
                statReporter.reportDelta(-1, 0); //undo item stats reporting within DeletionHandler::removeLinkWithCallback()

                //symlink.removeObject<sideTrg>(); -> "symlink, sideTrg" evaluated below!

                //=> don't risk reportStatus() throwing ThreadInterruption() leaving the target deleted rather than updated:
                //reportStatus(txtUpdatingLink_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>())); //restore status text

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
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(symlink.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(symlink.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(symlink.getItemName<sideSrc>())) //have difference in case?
                    parallel::moveAndRenameItem(symlink.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
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
void FolderPairSyncer::synchronizeFolder(FolderPair& folder) //throw FileError, ThreadInterruption
{
    const SyncOperation syncOp = folder.getSyncOperation();

    if (std::optional<SelectedSide> sideTrg = getTargetDirection(syncOp))
    {
        if (*sideTrg == LEFT_SIDE)
            synchronizeFolderInt<LEFT_SIDE>(folder, syncOp);
        else
            synchronizeFolderInt<RIGHT_SIDE>(folder, syncOp);
    }
}


template <SelectedSide sideTrg>
void FolderPairSyncer::synchronizeFolderInt(FolderPair& folder, SyncOperation syncOp) //throw FileError, ThreadInterruption
{
    constexpr SelectedSide sideSrc = OtherSide<sideTrg>::value;
    DeletionHandler& delHandlerTrg = SelectParam<sideTrg>::ref(delHandlerLeft_, delHandlerRight_);

    switch (syncOp)
    {
        case SO_CREATE_NEW_LEFT:
        case SO_CREATE_NEW_RIGHT:
        {
            if (auto parentFolder = dynamic_cast<const FolderPair*>(&folder.parent()))
                if (parentFolder->isEmpty<sideTrg>()) //BaseFolderPair OTOH is always non-empty and existing in this context => else: fatal error in zen::synchronize()
                    return; //if parent directory creation failed, there's no reason to show more errors!

            const AbstractPath targetPath = folder.getAbstractPath<sideTrg>();
            reportInfo(txtCreatingFolder_, AFS::getDisplayPath(targetPath)); //throw ThreadInterruption

            //shallow-"copying" a folder might not fail if source is missing, so we need to check this first:
            if (parallel::itemStillExists(folder.getAbstractPath<sideSrc>(), singleThread_)) //throw FileError
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);
                try
                {
                    //target existing: undefined behavior! (fail/overwrite)
                    parallel::copyNewFolder(folder.getAbstractPath<sideSrc>(), targetPath, copyFilePermissions_, singleThread_); //throw FileError
                }
                catch (FileError&)
                {
                    bool folderAlreadyExists = false;
                    try { folderAlreadyExists = parallel::getItemType(targetPath, singleThread_) == AFS::ItemType::FOLDER; } /*throw FileError*/ catch (FileError&) {}
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
                //attention when fixing statistics due to missing folder: child items may be scheduled for move, so deletion will have move-references flip back to copy + delete!
                const SyncStatistics statsBefore(folder.base()); //=> don't bother considering move operations, just calculate over the whole tree
                folder.refSubFiles  ().clear(); //
                folder.refSubLinks  ().clear(); //update FolderPair
                folder.refSubFolders().clear(); //
                folder.removeObject<sideSrc>(); //
                const SyncStatistics statsAfter(folder.base());

                acb_.updateDataProcessed(1, 0); //even if the source item does not exist anymore, significant I/O work was done => report
                acb_.updateDataTotal(getCUD(statsAfter) - getCUD(statsBefore) + 1, statsAfter.getBytesToProcess() - statsBefore.getBytesToProcess()); //noexcept

                reportInfo(txtSourceItemNotFound_, AFS::getDisplayPath(folder.getAbstractPath<sideSrc>())); //throw ThreadInterruption
            }
        }
        break;

        case SO_DELETE_LEFT:
        case SO_DELETE_RIGHT:
            reportInfo(delHandlerTrg.getTxtRemovingFolder(), AFS::getDisplayPath(folder.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                const SyncStatistics subStats(folder); //counts sub-objects only!
                AsyncItemStatReporter statReporter(1 + getCUD(subStats), subStats.getBytesToProcess(), acb_);

                delHandlerTrg.removeDirWithCallback(folder.getAbstractPath<sideTrg>(), folder.getRelativePath<sideTrg>(), statReporter, singleThread_); //throw FileError, X

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
            reportInfo(txtUpdatingAttributes_, AFS::getDisplayPath(folder.getAbstractPath<sideTrg>())); //throw ThreadInterruption
            {
                AsyncItemStatReporter statReporter(1, 0, acb_);

                if (getUnicodeNormalForm(folder.getItemName<sideTrg>()) !=
                    getUnicodeNormalForm(folder.getItemName<sideSrc>())) //have difference in case?
                    parallel::moveAndRenameItem(folder.getAbstractPath<sideTrg>(), //throw FileError, (ErrorDifferentVolume)
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
AFS::FileCopyResult FolderPairSyncer::copyFileWithCallback(const FileDescriptor& sourceDescr, //throw FileError, ThreadInterruption
                                                           const AbstractPath& targetPath,
                                                           const std::function<void()>& onDeleteTargetFile, //optional!
                                                           AsyncItemStatReporter& statReporter)
{
    const AbstractPath& sourcePath = sourceDescr.path;
    const AFS::StreamAttributes sourceAttr{ sourceDescr.attr.modTime, sourceDescr.attr.fileSize, sourceDescr.attr.fileId };

    auto copyOperation = [this, &sourceAttr, &targetPath, &onDeleteTargetFile, &statReporter](const AbstractPath& sourcePathTmp)
    {
        //target existing after onDeleteTargetFile(): undefined behavior! (fail/overwrite/auto-rename)
        const AFS::FileCopyResult result = parallel::copyFileTransactional(sourcePathTmp, sourceAttr, //throw FileError, ErrorFileLocked
                                                                           targetPath,
                                                                           copyFilePermissions_,
                                                                           failSafeFileCopy_, [&]
        {
            if (onDeleteTargetFile) //running *outside* singleThread_ lock! => onDeleteTargetFile-callback expects lock being held:
            {
                std::lock_guard<std::mutex> dummy(singleThread_);
                onDeleteTargetFile();
            }
        },
        [&](int64_t bytesDelta) //callback runs *outside* singleThread_ lock! => fine
        {
            statReporter.reportDelta(0, bytesDelta);
            interruptionPoint(); //throw ThreadInterruption
        },
        singleThread_);

        //#################### Verification #############################
        if (verifyCopiedFiles_)
        {
            ZEN_ON_SCOPE_FAIL(try { parallel::removeFilePlain(targetPath, singleThread_); }
            catch (FileError&) {}); //delete target if verification fails

            reportInfo(txtVerifyingFile_, AFS::getDisplayPath(targetPath)); //throw ThreadInterruption

            //callback runs *outside* singleThread_ lock! => fine
            auto verifyCallback = [&](int64_t bytesDelta) { interruptionPoint(); }; //throw ThreadInterruption

            parallel::verifyFiles(sourcePathTmp, targetPath, verifyCallback, singleThread_); //throw FileError
        }
        //#################### /Verification #############################

        return result;
    };

    return copyOperation(sourcePath); //throw FileError, (ErrorFileLocked), ThreadInterruption
}

//###########################################################################################

template <SelectedSide side>
bool baseFolderDrop(BaseFolderPair& baseFolder, ProcessCallback& callback)
{
    const AbstractPath folderPath = baseFolder.getAbstractPath<side>();

    if (baseFolder.isAvailable<side>())
    {
        const std::wstring errMsg = tryReportingError([&]
        {
            const FolderStatus status = getFolderStatusNonBlocking({ folderPath }, {} /*deviceParallelOps*/,
            false /*allowUserInteraction*/, callback);

            static_assert(std::is_same_v<decltype(status.failedChecks.begin()->second), FileError>);
            if (!status.failedChecks.empty())
                throw status.failedChecks.begin()->second;

            if (status.existing.find(folderPath) == status.existing.end())
                throw FileError(replaceCpy(_("Cannot find folder %x."), L"%x", fmtPath(AFS::getDisplayPath(folderPath))));
            //should really be logged as a "fatal error" if ignored by the user...
        }, callback); //throw X
        if (!errMsg.empty())
            return true;
    }

    return false;
}


template <SelectedSide side> //create base directories first (if not yet existing) -> no symlink or attribute copying!
bool createBaseFolder(BaseFolderPair& baseFolder, bool copyFilePermissions, ProcessCallback& callback) //return false if fatal error occurred
{
    static const SelectedSide sideSrc = OtherSide<side>::value;
    const AbstractPath baseFolderPath = baseFolder.getAbstractPath<side>();

    if (AFS::isNullPath(baseFolderPath))
        return true;

    if (!baseFolder.isAvailable<side>()) //create target directory: user presumably ignored error "dir existing" in order to have it created automatically
    {
        bool temporaryNetworkDrop = false;
        const std::wstring errMsg = tryReportingError([&]
        {
            const FolderStatus status = getFolderStatusNonBlocking({ baseFolderPath },  {} /*deviceParallelOps*/,
            false /*allowUserInteraction*/, callback);

            static_assert(std::is_same_v<decltype(status.failedChecks.begin()->second), FileError>);
            if (!status.failedChecks.empty())
                throw status.failedChecks.begin()->second;

            if (status.notExisting.find(baseFolderPath) != status.notExisting.end())
            {
                if (baseFolder.isAvailable<sideSrc>()) //copy file permissions
                {
                    if (std::optional<AbstractPath> parentPath = AFS::getParentPath(baseFolderPath))
                        AFS::createFolderIfMissingRecursion(*parentPath); //throw FileError

                    AFS::copyNewFolder(baseFolder.getAbstractPath<sideSrc>(), baseFolderPath, copyFilePermissions); //throw FileError
                }
                else
                    AFS::createFolderIfMissingRecursion(baseFolderPath); //throw FileError

                baseFolder.setAvailable<side>(true); //update our model!
            }
            else
            {
                //TEMPORARY network drop! base directory not found during comparison, but reappears during synchronization
                //=> sync-directions are based on false assumptions! Abort.
                callback.reportFatalError(replaceCpy(_("Target folder %x already existing."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath))));
                temporaryNetworkDrop = true;

                //Is it possible we're catching a "false positive" here, could FFS have created the directory indirectly after comparison?
                //  1. deletion handling: recycler       -> no, temp directory created only at first deletion
                //  2. deletion handling: versioning     -> "
                //  3. log file creates containing folder -> no, log only created in batch mode, and only *before* comparison
            }
        }, callback); //throw X
        return errMsg.empty() && !temporaryNetworkDrop;
    }
    return true;
}


enum class FolderPairJobType
{
    PROCESS,
    ALREADY_IN_SYNC,
    SKIP,
};
}


void fff::synchronize(const std::chrono::system_clock::time_point& syncStartTime,
                      bool verifyCopiedFiles,
                      bool copyLockedFiles,
                      bool copyFilePermissions,
                      bool failSafeFileCopy,
                      bool runWithBackgroundPriority,
                      const std::vector<FolderPairSyncCfg>& syncConfig,
                      FolderComparison& folderCmp,
                      const std::map<AfsDevice, size_t>& deviceParallelOps,
                      WarningDialogs& warnings,
                      ProcessCallback& callback)
{
    //PERF_START;

    if (syncConfig.size() != folderCmp.size())
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ":" + numberTo<std::string>(__LINE__));

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
                              ProcessCallback::PHASE_SYNCHRONIZING);
    }

    //-------------------------------------------------------------------------------

    //specify process and resource handling priorities
    std::unique_ptr<ScheduleForBackgroundProcessing> backgroundPrio;
    if (runWithBackgroundPriority)
        try
        {
            backgroundPrio = std::make_unique<ScheduleForBackgroundProcessing>(); //throw FileError
        }
        catch (const FileError& e) //not an error in this context
        {
            callback.reportInfo(e.toString()); //throw X
        }

    //prevent operating system going into sleep state
    std::unique_ptr<PreventStandby> noStandby;
    try
    {
        noStandby = std::make_unique<PreventStandby>(); //throw FileError
    }
    catch (const FileError& e) //not an error in this context
    {
        callback.reportInfo(e.toString()); //throw X
    }

    //-------------------execute basic checks all at once before starting sync--------------------------------------

    std::vector<FolderPairJobType> jobType(folderCmp.size(), FolderPairJobType::PROCESS); //folder pairs may be skipped after fatal errors were found

    std::vector<SyncStatistics::ConflictInfo> unresolvedConflicts;

    std::vector<std::tuple<AbstractPath, const PathFilter*, bool /*write access*/>> readWriteCheckBaseFolders;

    std::vector<std::pair<AbstractPath, AbstractPath>> significantDiffPairs;

    std::vector<std::pair<AbstractPath, std::pair<int64_t, int64_t>>> diskSpaceMissing; //base folder / space required / space available

    //status of base directories which are set to DeletionPolicy::RECYCLER (and contain actual items to be deleted)
    std::map<AbstractPath, bool> recyclerSupported; //expensive to determine on Win XP => buffer + check recycle bin existence only once per base folder!

    std::set<AbstractPath>                                  verCheckVersioningPaths;
    std::vector<std::pair<AbstractPath, const PathFilter*>> verCheckBaseFolderPaths; //hard filter creates new logical hierarchies for otherwise equal AbstractPath...

    //start checking folder pairs
    for (auto itBase = begin(folderCmp); itBase != end(folderCmp); ++itBase)
    {
        BaseFolderPair& baseFolder = *itBase;
        const size_t folderIndex = itBase - begin(folderCmp);
        const FolderPairSyncCfg& folderPairCfg  = syncConfig     [folderIndex];
        const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

        //aggregate all conflicts:
        append(unresolvedConflicts, folderPairStat.getConflicts());

        //exclude a few pathological cases (including empty left, right folders)
        if (baseFolder.getAbstractPath< LEFT_SIDE>() ==
            baseFolder.getAbstractPath<RIGHT_SIDE>())
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //skip folder pair if there is nothing to do (except for two-way mode and move-detection, where DB files need to be updated)
        //-> skip creating (not yet existing) base directories in particular if there's no need
        if (getCUD(folderPairStat) == 0)
        {
            jobType[folderIndex] = FolderPairJobType::ALREADY_IN_SYNC;
            continue;
        }

        const bool writeLeft = folderPairStat.createCount<LEFT_SIDE>() +
                               folderPairStat.updateCount<LEFT_SIDE>() +
                               folderPairStat.deleteCount<LEFT_SIDE>() > 0;

        const bool writeRight = folderPairStat.createCount<RIGHT_SIDE>() +
                                folderPairStat.updateCount<RIGHT_SIDE>() +
                                folderPairStat.deleteCount<RIGHT_SIDE>() > 0;

        //check for empty target folder paths: this only makes sense if empty field is source (and no DB files need to be created)
        if ((AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) && (writeLeft  || folderPairCfg.saveSyncDB)) ||
            (AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()) && (writeRight || folderPairCfg.saveSyncDB)))
        {
            callback.reportFatalError(_("Target folder input field must not be empty."));
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //check for network drops after comparison
        // - convenience: exit sync right here instead of showing tons of errors during file copy
        // - early failure! there's no point in evaluating subsequent warnings
        if (baseFolderDrop< LEFT_SIDE>(baseFolder, callback) ||
            baseFolderDrop<RIGHT_SIDE>(baseFolder, callback))
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        //allow propagation of deletions only from *null-* or *existing* source folder:
        auto sourceFolderMissing = [&](const AbstractPath& baseFolderPath, bool wasAvailable) //we need to evaluate existence status from time of comparison!
        {
            if (!AFS::isNullPath(baseFolderPath))
                //PERMANENT network drop: avoid data loss when source directory is not found AND user chose to ignore errors (else we wouldn't arrive here)
                if (folderPairStat.deleteCount() > 0) //check deletions only... (respect filtered items!)
                    //folderPairStat.conflictCount() == 0 && -> there COULD be conflicts for <Two way> variant if directory existence check fails, but loading sync.ffs_db succeeds
                    //https://sourceforge.net/tracker/?func=detail&atid=1093080&aid=3531351&group_id=234430 -> fixed, but still better not consider conflicts!
                    if (!wasAvailable) //avoid race-condition: we need to evaluate existence status from time of comparison!
                    {
                        callback.reportFatalError(replaceCpy(_("Source folder %x not found."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath))));
                        return true;
                    }
            return false;
        };
        if (sourceFolderMissing(baseFolder.getAbstractPath< LEFT_SIDE>(), baseFolder.isAvailable< LEFT_SIDE>()) ||
            sourceFolderMissing(baseFolder.getAbstractPath<RIGHT_SIDE>(), baseFolder.isAvailable<RIGHT_SIDE>()))
        {
            jobType[folderIndex] = FolderPairJobType::SKIP;
            continue;
        }

        if (folderPairCfg.handleDeletion == DeletionPolicy::VERSIONING)
        {
            const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase);

            //check if user-defined directory for deletion was specified
            if (AFS::isNullPath(versioningFolderPath))
            {
                //should never arrive here: already checked in SyncCfgDialog
                callback.reportFatalError(_("Please enter a target folder for versioning."));
                jobType[folderIndex] = FolderPairJobType::SKIP;
                continue;
            }
            //===============================================================================================
            //================ end of checks that may skip folder pairs => begin of warnings ================
            //===============================================================================================

            //prepare: check if versioning path itself will be synchronized (and was not excluded via filter)
            verCheckVersioningPaths.insert(versioningFolderPath);
            verCheckBaseFolderPaths.emplace_back(baseFolder.getAbstractPath< LEFT_SIDE>(), &baseFolder.getFilter());
            verCheckBaseFolderPaths.emplace_back(baseFolder.getAbstractPath<RIGHT_SIDE>(), &baseFolder.getFilter());
        }

        //prepare: check if folders are used by multiple pairs in read/write access
        readWriteCheckBaseFolders.emplace_back(baseFolder.getAbstractPath< LEFT_SIDE>(), &baseFolder.getFilter(), writeLeft);
        readWriteCheckBaseFolders.emplace_back(baseFolder.getAbstractPath<RIGHT_SIDE>(), &baseFolder.getFilter(), writeRight);

        //check if more than 50% of total number of files/dirs are to be created/overwritten/deleted
        if (!AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) &&
            !AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()))
            if (significantDifferenceDetected(folderPairStat))
                significantDiffPairs.emplace_back(baseFolder.getAbstractPath< LEFT_SIDE>(),
                                                  baseFolder.getAbstractPath<RIGHT_SIDE>());

        //check for sufficient free diskspace
        auto checkSpace = [&](const AbstractPath& baseFolderPath, int64_t minSpaceNeeded)
        {
            if (!AFS::isNullPath(baseFolderPath))
                try
                {
                    const int64_t freeSpace = AFS::getFreeDiskSpace(baseFolderPath); //throw FileError, returns 0 if not available

                    if (0 < freeSpace && //zero means "request not supported" (e.g. see WebDav)
                        freeSpace < minSpaceNeeded)
                        diskSpaceMissing.push_back({ baseFolderPath, { minSpaceNeeded, freeSpace } });
                }
                catch (const FileError& e) //for warning only => no need for tryReportingError(), but at least log it!
                {
                    callback.reportInfo(e.toString()); //throw X
                }
        };
        const std::pair<int64_t, int64_t> spaceNeeded = MinimumDiskSpaceNeeded::calculate(baseFolder);
        checkSpace(baseFolder.getAbstractPath< LEFT_SIDE>(), spaceNeeded.first);
        checkSpace(baseFolder.getAbstractPath<RIGHT_SIDE>(), spaceNeeded.second);

        //windows: check if recycle bin really exists; if not, Windows will silently delete, which is wrong
        auto checkRecycler = [&](const AbstractPath& baseFolderPath)
        {
            assert(!AFS::isNullPath(baseFolderPath));
            if (!AFS::isNullPath(baseFolderPath))
                if (recyclerSupported.find(baseFolderPath) == recyclerSupported.end()) //perf: avoid duplicate checks!
                {
                    callback.reportStatus(replaceCpy(_("Checking recycle bin availability for folder %x..."), L"%x", //throw X
                                                     fmtPath(AFS::getDisplayPath(baseFolderPath))));
                    bool recSupported = false;
                    tryReportingError([&]
                    {
                        recSupported = AFS::supportsRecycleBin(baseFolderPath, [&]{ callback.requestUiRefresh(); /*may throw*/ }); //throw FileError
                    }, callback); //throw X

                    recyclerSupported.emplace(baseFolderPath, recSupported);
                }
        };
        if (folderPairCfg.handleDeletion == DeletionPolicy::RECYCLER)
        {
            if (folderPairStat.expectPhysicalDeletion<LEFT_SIDE>())
                checkRecycler(baseFolder.getAbstractPath<LEFT_SIDE>());

            if (folderPairStat.expectPhysicalDeletion<RIGHT_SIDE>())
                checkRecycler(baseFolder.getAbstractPath<RIGHT_SIDE>());
        }
    }

    //check if unresolved conflicts exist
    if (!unresolvedConflicts.empty())
    {
        std::wstring msg = _("The following items have unresolved conflicts and will not be synchronized:");

        for (const SyncStatistics::ConflictInfo& item : unresolvedConflicts) //show *all* conflicts in warning message
            msg += L"\n\n" + fmtPath(item.relPath) + L": " + item.msg;

        callback.reportWarning(msg, warnings.warnUnresolvedConflicts);
    }

    //check if user accidentally selected wrong directories for sync
    if (!significantDiffPairs.empty())
    {
        std::wstring msg = _("The following folders are significantly different. Please check that the correct folders are selected for synchronization.");

        for (const auto& [folderPathL, folderPathR] : significantDiffPairs)
            msg += L"\n\n" +
                   AFS::getDisplayPath(folderPathL) + L" <-> " + L"\n" +
                   AFS::getDisplayPath(folderPathR);

        callback.reportWarning(msg, warnings.warnSignificantDifference);
    }

    //check for sufficient free diskspace
    if (!diskSpaceMissing.empty())
    {
        std::wstring msg = _("Not enough free disk space available in:");

        for (const auto& [folderPath, space] : diskSpaceMissing)
            msg += L"\n\n" + AFS::getDisplayPath(folderPath) + L"\n" +
                   _("Required:")  + L" " + formatFilesizeShort(space.first)  + L"\n" +
                   _("Available:") + L" " + formatFilesizeShort(space.second);

        callback.reportWarning(msg, warnings.warnNotEnoughDiskSpace);
    }

    //windows: check if recycle bin really exists; if not, Windows will silently delete, which is wrong
    {
        std::wstring msg;
        for (const auto& [folderPath, supported] : recyclerSupported)
            if (!supported)
                msg += L"\n" + AFS::getDisplayPath(folderPath);

        if (!msg.empty())
            callback.reportWarning(_("The recycle bin is not supported by the following folders. Deleted or overwritten files will not be able to be restored:") + L"\n" + msg,
                                   warnings.warnRecyclerMissing);
    }

    //check if folders are used by multiple pairs in read/write access
    {
        std::set<AbstractPath> dependentFolders;

        //race condition := multiple accesses of which at least one is a write
        for (auto it = readWriteCheckBaseFolders.begin(); it != readWriteCheckBaseFolders.end(); ++it)
            if (std::get<bool>(*it)) //write access
                for (auto it2 = readWriteCheckBaseFolders.begin(); it2 != readWriteCheckBaseFolders.end(); ++it2)
                    if (!std::get<bool>(*it2) || it < it2) //avoid duplicate comparisons
                        if (std::optional<PathDependency> pd = getPathDependency(std::get<AbstractPath>(*it),  *std::get<const PathFilter*>(*it),
                                                                                 std::get<AbstractPath>(*it2), *std::get<const PathFilter*>(*it2)))
                        {
                            dependentFolders.insert(pd->basePathParent);
                            dependentFolders.insert(pd->basePathChild);
                        }

        if (!dependentFolders.empty())
        {
            std::wstring msg = _("Some files will be synchronized as part of multiple base folders.") + L"\n" +
                               _("To avoid conflicts, set up exclude filters so that each updated file is considered by only one base folder.") + L"\n";

            for (const AbstractPath& baseFolderPath : dependentFolders)
                msg += L"\n" + AFS::getDisplayPath(baseFolderPath);

            callback.reportWarning(msg, warnings.warnDependentBaseFolders);
        }
    }

    //check if versioning path itself will be synchronized (and was not excluded via filter)
    {
        std::wstring msg;
        for (const AbstractPath& versioningFolderPath : verCheckVersioningPaths)
        {
            std::map<AbstractPath, std::wstring> uniqueMsgs; //=> at most one msg per base folder (*and* per versioningFolderPath)

            for (const auto& [folderPath, filter] : verCheckBaseFolderPaths) //may contain duplicate paths, but with *different* hard filter!
                if (std::optional<PathDependency> pd = getPathDependency(versioningFolderPath, NullFilter(), folderPath, *filter))
                {
                    std::wstring line = L"\n\n" + _("Versioning folder:") + L" \t" + AFS::getDisplayPath(versioningFolderPath) +
                                        L"\n"   + _("Base folder:")       + L" \t" + AFS::getDisplayPath(folderPath);
                    if (pd->basePathParent == folderPath && !pd->relPath.empty())
                        line += L"\n" + _("Exclude:") + L" \t" + utfTo<std::wstring>(FILE_NAME_SEPARATOR + pd->relPath + FILE_NAME_SEPARATOR);

                    uniqueMsgs[folderPath] = line;
                }
            for (const auto& [folderPath, perFolderMsg] : uniqueMsgs)
                msg += perFolderMsg;
        }
        if (!msg.empty())
            callback.reportWarning(_("The versioning folder is contained in a base folder.") + L"\n" +
                                   _("The folder should be excluded from synchronization via filter.") + msg, warnings.warnVersioningFolderPartOfSync);
    }

    //-------------------end of basic checks------------------------------------------

    std::vector<FileError> errorsModTime; //show all warnings as a single message

    std::set<VersioningLimitFolder> versionLimitFolders;

    try
    {
        //loop through all directory pairs
        for (auto itBase = begin(folderCmp); itBase != end(folderCmp); ++itBase)
        {
            BaseFolderPair& baseFolder = *itBase;
            const size_t folderIndex = itBase - begin(folderCmp);
            const FolderPairSyncCfg& folderPairCfg  = syncConfig     [folderIndex];
            const SyncStatistics&    folderPairStat = folderPairStats[folderIndex];

            if (jobType[folderIndex] == FolderPairJobType::SKIP) //folder pairs may be skipped after fatal errors were found
                continue;

            //------------------------------------------------------------------------------------------
            callback.reportInfo(_("Synchronizing folder pair:") + L" " + getVariantNameForLog(folderPairCfg.syncVariant) + L"\n" + //throw X
                                L"    " + AFS::getDisplayPath(baseFolder.getAbstractPath< LEFT_SIDE>()) + L"\n" +
                                L"    " + AFS::getDisplayPath(baseFolder.getAbstractPath<RIGHT_SIDE>()));
            //------------------------------------------------------------------------------------------

            //checking a second time: (a long time may have passed since folder comparison!)
            if (baseFolderDrop< LEFT_SIDE>(baseFolder, callback) ||
                baseFolderDrop<RIGHT_SIDE>(baseFolder, callback))
                continue;

            //create base folders if not yet existing
            if (folderPairStat.createCount() > 0 || folderPairCfg.saveSyncDB) //else: temporary network drop leading to deletions already caught by "sourceFolderMissing" check!
                if (!createBaseFolder< LEFT_SIDE>(baseFolder, copyFilePermissions, callback) || //+ detect temporary network drop!!
                    !createBaseFolder<RIGHT_SIDE>(baseFolder, copyFilePermissions, callback))   //
                    continue;

            //------------------------------------------------------------------------------------------
            //execute synchronization recursively

            //update synchronization database in case of errors:
            auto guardDbSave = makeGuard<ScopeGuardRunMode::ON_FAIL>([&]
            {
                try
                {
                    if (folderPairCfg.saveSyncDB)
                        saveLastSynchronousState(baseFolder, //throw FileError
                        [&](const std::wstring& statusMsg) { try { callback.reportStatus(statusMsg); /*throw X*/} catch (...) {}});
                }
                catch (FileError&) {}
            });

            if (jobType[folderIndex] == FolderPairJobType::PROCESS)
            {
                //guarantee removal of invalid entries (where element is empty on both sides)
                ZEN_ON_SCOPE_EXIT(BaseFolderPair::removeEmpty(baseFolder));

                bool copyPermissionsFp = false;
                tryReportingError([&]
                {
                    copyPermissionsFp = copyFilePermissions && //copy permissions only if asked for and supported by *both* sides!
                    !AFS::isNullPath(baseFolder.getAbstractPath< LEFT_SIDE>()) && //scenario: directory selected on one side only
                    !AFS::isNullPath(baseFolder.getAbstractPath<RIGHT_SIDE>()) && //
                    AFS::supportPermissionCopy(baseFolder.getAbstractPath<LEFT_SIDE>(),
                                               baseFolder.getAbstractPath<RIGHT_SIDE>()); //throw FileError
                }, callback); //throw X


                auto getEffectiveDeletionPolicy = [&](const AbstractPath& baseFolderPath) -> DeletionPolicy
                {
                    if (folderPairCfg.handleDeletion == DeletionPolicy::RECYCLER)
                    {
                        auto it = recyclerSupported.find(baseFolderPath);
                        if (it != recyclerSupported.end()) //buffer filled during intro checks (but only if deletions are expected)
                            if (!it->second)
                                return DeletionPolicy::PERMANENT; //Windows' ::SHFileOperation() will do this anyway, but we have a better and faster deletion routine (e.g. on networks)
                    }
                    return folderPairCfg.handleDeletion;
                };
                const AbstractPath versioningFolderPath = createAbstractPath(folderPairCfg.versioningFolderPhrase);

                DeletionHandler delHandlerL(baseFolder.getAbstractPath<LEFT_SIDE>(),
                                            getEffectiveDeletionPolicy(baseFolder.getAbstractPath<LEFT_SIDE>()),
                                            versioningFolderPath,
                                            folderPairCfg.versioningStyle,
                                            std::chrono::system_clock::to_time_t(syncStartTime));

                DeletionHandler delHandlerR(baseFolder.getAbstractPath<RIGHT_SIDE>(),
                                            getEffectiveDeletionPolicy(baseFolder.getAbstractPath<RIGHT_SIDE>()),
                                            versioningFolderPath,
                                            folderPairCfg.versioningStyle,
                                            std::chrono::system_clock::to_time_t(syncStartTime));

                //always (try to) clean up, even if synchronization is aborted!
                ZEN_ON_SCOPE_EXIT(
                    //may block heavily, but still do not allow user callback:
                    //-> avoid throwing user cancel exception again, leading to incomplete clean-up!
                    try
                {
                    delHandlerL.tryCleanup(callback, false /*allowCallbackException*/); //throw FileError, (throw X)
                }
                catch (FileError&) {}
                catch (...) { assert(false); } //what is this?
                try
                {
                    delHandlerR.tryCleanup(callback, false /*allowCallbackException*/); //throw FileError, (throw X)
                }
                catch (FileError&) {}
                catch (...) { assert(false); } //what is this?
                );

                size_t parallelOps = std::max(getDeviceParallelOps(deviceParallelOps, baseFolder.getAbstractPath< LEFT_SIDE>().afsDevice),
                                              getDeviceParallelOps(deviceParallelOps, baseFolder.getAbstractPath<RIGHT_SIDE>().afsDevice));
                if (folderPairCfg.handleDeletion == DeletionPolicy::VERSIONING)
                    parallelOps = std::max(parallelOps, getDeviceParallelOps(deviceParallelOps, versioningFolderPath.afsDevice));

                FolderPairSyncer::SyncCtx syncCtx =
                {
                    verifyCopiedFiles, copyPermissionsFp, failSafeFileCopy,
                    errorsModTime,
                    delHandlerL, delHandlerR,
                    parallelOps
                };
                FolderPairSyncer::runSync(syncCtx, baseFolder, callback);

                //(try to gracefully) cleanup temporary Recycle Bin folders and versioning -> will be done in ~DeletionHandler anyway...
                tryReportingError([&] { delHandlerL.tryCleanup(callback, true /*allowCallbackException*/); /*throw FileError*/}, callback); //throw X
                tryReportingError([&] { delHandlerR.tryCleanup(callback, true                           ); /*throw FileError*/}, callback); //throw X

                if (folderPairCfg.handleDeletion == DeletionPolicy::VERSIONING &&
                    folderPairCfg.versioningStyle != VersioningStyle::REPLACE)
                    versionLimitFolders.insert(
                {
                    versioningFolderPath,
                    folderPairCfg.versionMaxAgeDays,
                    folderPairCfg.versionCountMin,
                    folderPairCfg.versionCountMax
                });
            }

            //(try to gracefully) write database file
            if (folderPairCfg.saveSyncDB)
            {
                callback.reportStatus(_("Generating database...")); //throw X
                callback.forceUiRefresh(); //throw X

                tryReportingError([&]
                {
                    saveLastSynchronousState(baseFolder, //throw FileError
                    [&](const std::wstring& statusMsg) { callback.reportStatus(statusMsg); /*throw X*/});
                }, callback); //throw X

                guardDbSave.dismiss(); //[!] after "graceful" try: user might have cancelled during DB write: ensure DB is still written
            }
        }

        //-----------------------------------------------------------------------------------------------------

        applyVersioningLimit(versionLimitFolders, deviceParallelOps, callback); //throw X

        //------------------- show warnings after end of synchronization --------------------------------------

        //TODO: mod time warnings are not shown if user cancelled sync before batch-reporting the warnings: problem?

        //show errors when setting modification time: warning, not an error
        if (!errorsModTime.empty())
        {
            std::wstring msg;
            for (const FileError& e : errorsModTime)
            {
                std::wstring singleMsg = replaceCpy(e.toString(), L"\n\n", L"\n");
                msg += singleMsg + L"\n\n";
            }
            msg.resize(msg.size() - 2);

            callback.reportWarning(msg, warnings.warnModificationTimeError); //throw X
        }
    }
    catch (const std::exception& e)
    {
        callback.reportFatalError(utfTo<std::wstring>(e.what()));
        callback.abortProcessNow(); //throw X
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ":" + numberTo<std::string>(__LINE__));
    }
}
