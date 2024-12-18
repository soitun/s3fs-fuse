/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Takeshi Nakatani <ggtakec.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "s3fs.h"
#include "s3fs_logger.h"
#include "s3fs_util.h"
#include "fdcache_fdinfo.h"
#include "fdcache_pseudofd.h"
#include "fdcache_entity.h"
#include "curl.h"
#include "string_util.h"
#include "threadpoolman.h"
#include "s3fs_threadreqs.h"

//------------------------------------------------
// PseudoFdInfo methods
//------------------------------------------------
PseudoFdInfo::PseudoFdInfo(int fd, int open_flags) : pseudo_fd(-1), physical_fd(fd), flags(0), upload_fd(-1), instruct_count(0), last_result(0), uploaded_sem(0)
{
    if(-1 != physical_fd){
        pseudo_fd = PseudoFdManager::Get();
        flags     = open_flags;
    }
}

PseudoFdInfo::~PseudoFdInfo()
{
    Clear();        // call before destroying the mutex
}

bool PseudoFdInfo::Clear()
{
    // cppcheck-suppress unmatchedSuppression
    // cppcheck-suppress knownConditionTrueFalse
    if(!CancelAllThreads()){
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(upload_list_lock);
        // cppcheck-suppress unmatchedSuppression
        // cppcheck-suppress knownConditionTrueFalse
        if(!ResetUploadInfo()){
            return false;
        }
    }
    CloseUploadFd();

    if(-1 != pseudo_fd){
        PseudoFdManager::Release(pseudo_fd);
    }
    pseudo_fd   = -1;
    physical_fd = -1;

    return true;
}

bool PseudoFdInfo::IsUploadingHasLock() const
{
    return !upload_id.empty();
}

bool PseudoFdInfo::IsUploading() const
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);
    return IsUploadingHasLock();
}

void PseudoFdInfo::CloseUploadFd()
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    if(-1 != upload_fd){
        close(upload_fd);
    }
}

bool PseudoFdInfo::OpenUploadFd()
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    if(-1 != upload_fd){
        // already initialized
        return true;
    }
    if(-1 == physical_fd){
        S3FS_PRN_ERR("physical_fd is not initialized yet.");
        return false;
    }

    // duplicate fd
    int fd;
    if(-1 == (fd = dup(physical_fd))){
        S3FS_PRN_ERR("Could not duplicate physical file descriptor(errno=%d)", errno);
        return false;
    }
    scope_guard guard([&]() { close(fd); });

    if(0 != lseek(fd, 0, SEEK_SET)){
        S3FS_PRN_ERR("Could not seek physical file descriptor(errno=%d)", errno);
        return false;
    }
    struct stat st;
    if(-1 == fstat(fd, &st)){
        S3FS_PRN_ERR("Invalid file descriptor for uploading(errno=%d)", errno);
        return false;
    }

    guard.dismiss();
    upload_fd = fd;
    return true;
}

bool PseudoFdInfo::Set(int fd, int open_flags)
{
    if(-1 == fd){
        return false;
    }
    Clear();
    physical_fd = fd;
    pseudo_fd   = PseudoFdManager::Get();
    flags       = open_flags;

    return true;
}

bool PseudoFdInfo::Writable() const
{
    if(-1 == pseudo_fd){
        return false;
    }
    if(0 == (flags & (O_WRONLY | O_RDWR))){
        return false;
    }
    return true;
}

bool PseudoFdInfo::Readable() const
{
    if(-1 == pseudo_fd){
        return false;
    }
    // O_RDONLY is 0x00, it means any pattern is readable.
    return true;
}

bool PseudoFdInfo::ClearUploadInfo(bool is_cancel_mp)
{
    if(is_cancel_mp){
        // cppcheck-suppress unmatchedSuppression
        // cppcheck-suppress knownConditionTrueFalse
        if(!CancelAllThreads()){
            return false;
        }
    }

    const std::lock_guard<std::mutex> lock(upload_list_lock);
    return ResetUploadInfo();
}

bool PseudoFdInfo::ResetUploadInfo()
{
    upload_id.clear();
    upload_list.clear();
    instruct_count  = 0;
    last_result     = 0;

    return true;
}

bool PseudoFdInfo::RowInitialUploadInfo(const std::string& id, bool is_cancel_mp)
{
    if(is_cancel_mp){
        // cppcheck-suppress unmatchedSuppression
        // cppcheck-suppress knownConditionTrueFalse
        if(!ClearUploadInfo(is_cancel_mp)){
            return false;
        }
    }else{
        const std::lock_guard<std::mutex> lock(upload_list_lock);
        // cppcheck-suppress unmatchedSuppression
        // cppcheck-suppress knownConditionTrueFalse
        if(!ResetUploadInfo()){
            return false;
        }
    }

    const std::lock_guard<std::mutex> lock(upload_list_lock);
    upload_id = id;
    return true;
}

void PseudoFdInfo::IncreaseInstructionCount()
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);
    ++instruct_count;
}

bool PseudoFdInfo::GetUploadInfo(std::string& id, int& fd) const
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    if(!IsUploadingHasLock()){
        S3FS_PRN_ERR("Multipart Upload has not started yet.");
        return false;
    }
    id = upload_id;
    fd = upload_fd;
    return true;
}

bool PseudoFdInfo::GetUploadId(std::string& id) const
{
    int fd = -1;
    return GetUploadInfo(id, fd);
}

bool PseudoFdInfo::GetEtaglist(etaglist_t& list) const
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    if(!IsUploadingHasLock()){
        S3FS_PRN_ERR("Multipart Upload has not started yet.");
        return false;
    }

    list.clear();
    for(auto iter = upload_list.cbegin(); iter != upload_list.cend(); ++iter){
        if(iter->petag){
            list.push_back(*(iter->petag));
        }else{
            S3FS_PRN_ERR("The pointer to the etag string is null(internal error).");
            return false;
        }
    }
    return !list.empty();
}

// [NOTE]
// This method adds a part for a multipart upload.
// The added new part must be an area that is exactly continuous with the
// immediately preceding part.
// An error will occur if it is discontinuous or if it overlaps with an
// existing area.
//
bool PseudoFdInfo::AppendUploadPart(off_t start, off_t size, bool is_copy, etagpair** ppetag)
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    if(!IsUploadingHasLock()){
        S3FS_PRN_ERR("Multipart Upload has not started yet.");
        return false;
    }

    off_t    next_start_pos = 0;
    if(!upload_list.empty()){
        next_start_pos = upload_list.back().startpos + upload_list.back().size;
    }
    if(start != next_start_pos){
        S3FS_PRN_ERR("The expected starting position for the next part is %lld, but %lld was specified.", static_cast<long long int>(next_start_pos), static_cast<long long int>(start));
        return false;
    }

    // make part number
    int partnumber = static_cast<int>(upload_list.size()) + 1;

    // add new part
    etagpair*   petag_entity = etag_entities.add(etagpair(nullptr, partnumber));              // [NOTE] Create the etag entity and register it in the list.
    upload_list.emplace_back(false, physical_fd, start, size, is_copy, petag_entity);

    // set etag pointer
    if(ppetag){
        *ppetag = petag_entity;
    }

    return true;
}

//
// Utility for sorting upload list
//
static bool filepart_partnum_compare(const filepart& src1, const filepart& src2)
{
    return src1.get_part_number() < src2.get_part_number();
}

bool PseudoFdInfo::InsertUploadPart(off_t start, off_t size, int part_num, bool is_copy, etagpair** ppetag)
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    //S3FS_PRN_DBG("[start=%lld][size=%lld][part_num=%d][is_copy=%s]", static_cast<long long int>(start), static_cast<long long int>(size), part_num, (is_copy ? "true" : "false"));

    if(!IsUploadingHasLock()){
        S3FS_PRN_ERR("Multipart Upload has not started yet.");
        return false;
    }
    if(start < 0 || size <= 0 || part_num < 0 || !ppetag){
        S3FS_PRN_ERR("Parameters are wrong.");
        return false;
    }

    // insert new part
    etagpair*   petag_entity = etag_entities.add(etagpair(nullptr, part_num));
    upload_list.emplace_back(false, physical_fd, start, size, is_copy, petag_entity);

    // sort by part number
    std::sort(upload_list.begin(), upload_list.end(), filepart_partnum_compare);

    // set etag pointer
    *ppetag = petag_entity;

    return true;
}

bool PseudoFdInfo::ParallelMultipartUpload(const char* path, const mp_part_list_t& mplist, bool is_copy)
{
    //S3FS_PRN_DBG("[path=%s][mplist(%zu)]", SAFESTRPTR(path), mplist.size());

    if(mplist.empty()){
        // nothing to do
        return true;
    }
    if(!OpenUploadFd()){
        return false;
    }

    // Get upload id/fd before loop
    std::string tmp_upload_id;
    int         tmp_upload_fd = -1;
    if(!GetUploadInfo(tmp_upload_id, tmp_upload_fd)){
        return false;
    }

    std::string strpath = SAFESTRPTR(path);

    for(auto iter = mplist.cbegin(); iter != mplist.cend(); ++iter){
        // Insert upload part
        etagpair* petag = nullptr;
        if(!InsertUploadPart(iter->start, iter->size, iter->part_num, is_copy, &petag)){
            S3FS_PRN_ERR("Failed to insert Multipart Upload Part to mplist [path=%s][start=%lld][size=%lld][part_num=%d][is_copy=%s]", strpath.c_str(), static_cast<long long int>(iter->start), static_cast<long long int>(iter->size), iter->part_num, (is_copy ? "true" : "false"));
            return false;
        }

        // setup instruction and request on another thread
        int result;
        if(0 != (result = multipart_upload_part_request(strpath, tmp_upload_fd, iter->start, iter->size, iter->part_num, tmp_upload_id, petag, is_copy, &uploaded_sem, &upload_list_lock, &last_result))){
            S3FS_PRN_ERR("failed setup instruction for Multipart Upload Part Request by erro(%d) [path=%s][start=%lld][size=%lld][part_num=%d][is_copy=%s]", result, strpath.c_str(), static_cast<long long int>(iter->start), static_cast<long long int>(iter->size), iter->part_num, (is_copy ? "true" : "false"));
            return false;
        }

        // Count up the number of internally managed threads
        IncreaseInstructionCount();
    }
    return true;
}

bool PseudoFdInfo::ParallelMultipartUploadAll(const char* path, const mp_part_list_t& to_upload_list, const mp_part_list_t& copy_list, int& result)
{
    S3FS_PRN_DBG("[path=%s][to_upload_list(%zu)][copy_list(%zu)]", SAFESTRPTR(path), to_upload_list.size(), copy_list.size());

    result = 0;

    if(!OpenUploadFd()){
        return false;
    }
    if(!ParallelMultipartUpload(path, to_upload_list, false) || !ParallelMultipartUpload(path, copy_list, true)){
        S3FS_PRN_ERR("Failed setup instruction for uploading(path=%s, to_upload_list=%zu, copy_list=%zu).", SAFESTRPTR(path), to_upload_list.size(), copy_list.size());
        return false;
    }

    // Wait for all thread exiting
    result = WaitAllThreadsExit();

    return true;
}

//
// Common method that calls S3fsCurl::PreMultipartUploadRequest via pre_multipart_upload_request
//
// [NOTE]
// If the request is successful, initialize upload_id.
//
int PseudoFdInfo::PreMultipartUploadRequest(const std::string& strpath, const headers_t& meta)
{
    // get upload_id
    std::string new_upload_id;
    int         result;
    if(0 != (result = pre_multipart_upload_request(strpath, meta, new_upload_id))){
        return result;
    }

    // reset upload_id
    if(!RowInitialUploadInfo(new_upload_id, false/* not need to cancel */)){
        S3FS_PRN_ERR("failed to setup multipart upload(set upload id to object)");
        return -EIO;
    }
    S3FS_PRN_DBG("succeed to setup multipart upload(set upload id to object)");

    return 0;
}

//
// Upload the last updated Untreated area
//
// [Overview]
// Uploads untreated areas with the maximum multipart upload size as the
// boundary.
//
// * The starting position of the untreated area is aligned with the maximum
//   multipart upload size as the boundary.
// * If there is an uploaded area that overlaps with the aligned untreated
//   area, that uploaded area is canceled and absorbed by the untreated area.
// * Upload only when the aligned untreated area exceeds the maximum multipart
//   upload size.
// * When the start position of the untreated area is changed to boundary
//   alignment(to backward), and if that gap area is remained, that area is
//   rest to untreated area.
//
ssize_t PseudoFdInfo::UploadBoundaryLastUntreatedArea(const char* path, headers_t& meta, FdEntity* pfdent)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d]", SAFESTRPTR(path), pseudo_fd, physical_fd);

    if(!path || -1 == physical_fd || -1 == pseudo_fd || !pfdent){
        S3FS_PRN_ERR("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not writable, or pfdent is nullptr.", pseudo_fd, physical_fd, path);
        return -EBADF;
    }

    //
    // Get last update untreated area
    //
    off_t last_untreated_start = 0;
    off_t last_untreated_size  = 0;
    if(!pfdent->GetLastUpdateUntreatedPart(last_untreated_start, last_untreated_size) || last_untreated_start < 0 || last_untreated_size <= 0){
        S3FS_PRN_WARN("Not found last update untreated area or it is empty, thus return without any error.");
        return 0;
    }

    //
    // Aligns the start position of the last updated raw area with the boundary
    //
    // * Align the last updated raw space with the maximum upload size boundary.
    // * The remaining size of the part before the boundary is will not be uploaded.
    //
    off_t max_mp_size     = S3fsCurl::GetMultipartSize();
    off_t aligned_start   = ((last_untreated_start / max_mp_size) + (0 < (last_untreated_start % max_mp_size) ? 1 : 0)) * max_mp_size;
    if((last_untreated_start + last_untreated_size) <= aligned_start){
        S3FS_PRN_INFO("After the untreated area(start=%lld, size=%lld) is aligned with the boundary, the aligned start(%lld) exceeds the untreated area, so there is nothing to do.", static_cast<long long int>(last_untreated_start), static_cast<long long int>(last_untreated_size), static_cast<long long int>(aligned_start));
        return 0;
    }

    off_t aligned_size    = (((last_untreated_start + last_untreated_size) - aligned_start) / max_mp_size) * max_mp_size;
    if(0 == aligned_size){
        S3FS_PRN_DBG("After the untreated area(start=%lld, size=%lld) is aligned with the boundary(start is %lld), the aligned size is empty, so nothing to do.", static_cast<long long int>(last_untreated_start), static_cast<long long int>(last_untreated_size), static_cast<long long int>(aligned_start));
        return 0;
    }

    off_t front_rem_start = last_untreated_start;                       // start of the remainder untreated area in front of the boundary
    off_t front_rem_size  = aligned_start - last_untreated_start;       // size of the remainder untreated area in front of the boundary

    //
    // Get the area for uploading, if last update treated area can be uploaded.
    //
    // [NOTE]
    // * Create the updoad area list, if the untreated area aligned with the boundary
    //   exceeds the maximum upload size.
    // * If it overlaps with an area that has already been uploaded(unloaded list),
    //   that area is added to the cancellation list and included in the untreated area.
    //
    mp_part_list_t  to_upload_list;
    filepart_list_t cancel_uploaded_list;
    if(!ExtractUploadPartsFromUntreatedArea(aligned_start, aligned_size, to_upload_list, cancel_uploaded_list, S3fsCurl::GetMultipartSize())){
        S3FS_PRN_ERR("Failed to extract upload parts from last untreated area.");
        return -EIO;
    }
    if(to_upload_list.empty()){
        S3FS_PRN_INFO("There is nothing to upload. In most cases, the untreated area does not meet the upload size.");
        return 0;
    }

    //
    // Has multipart uploading already started?
    //
    if(!IsUploading()){
        std::string strpath = SAFESTRPTR(path);
        int         result;
        if(0 != (result = PreMultipartUploadRequest(strpath, meta))){
            return result;
        }
    }

    //
    // Output debug level information
    //
    // When canceling(overwriting) a part that has already been uploaded, output it.
    //
    if(S3fsLog::IsS3fsLogDbg()){
        for(auto cancel_iter = cancel_uploaded_list.cbegin(); cancel_iter != cancel_uploaded_list.cend(); ++cancel_iter){
            S3FS_PRN_DBG("Cancel uploaded: start(%lld), size(%lld), part number(%d)", static_cast<long long int>(cancel_iter->startpos), static_cast<long long int>(cancel_iter->size), (cancel_iter->petag ? cancel_iter->petag->part_num : -1));
        }
    }

    //
    // Upload Multipart parts
    //
    if(!ParallelMultipartUpload(path, to_upload_list, false)){
        S3FS_PRN_ERR("Failed to upload multipart parts.");
        return -EIO;
    }

    //
    // Exclude the uploaded Untreated area and update the last Untreated area.
    //
    off_t behind_rem_start = aligned_start + aligned_size;
    off_t behind_rem_size  = (last_untreated_start + last_untreated_size) - behind_rem_start;

    if(!pfdent->ReplaceLastUpdateUntreatedPart(front_rem_start, front_rem_size, behind_rem_start, behind_rem_size)){
        S3FS_PRN_WARN("The last untreated area could not be detected and the uploaded area could not be excluded from it, but continue because it does not affect the overall processing.");
    }

    return 0;
}

int PseudoFdInfo::WaitAllThreadsExit()
{
    int  result;
    bool is_loop = true;
    {
        const std::lock_guard<std::mutex> lock(upload_list_lock);
        if(0 == instruct_count){
            result  = last_result;
            is_loop = false;
        }
    }

    while(is_loop){
        // need to wait the worker exiting
        uploaded_sem.acquire();
        {
            const std::lock_guard<std::mutex> lock(upload_list_lock);
            if(0 == --instruct_count){
                // break loop
                result  = last_result;
                is_loop = false;
            }
        }
    }

    return result;
}

bool PseudoFdInfo::CancelAllThreads()
{
    bool need_cancel = false;
    {
        const std::lock_guard<std::mutex> lock(upload_list_lock);
        if(0 < instruct_count){
            S3FS_PRN_INFO("The upload thread is running, so cancel them and wait for the end.");
            need_cancel = true;
            last_result = -ECANCELED;   // to stop thread running
        }
    }
    if(need_cancel){
        WaitAllThreadsExit();
    }
    return true;
}

//
// Extract the list for multipart upload from the Unteated Area
//
// The untreated_start parameter must be set aligning it with the boundaries
// of the maximum multipart upload size. This method expects it to be bounded.
//
// This method creates the upload area aligned from the untreated area by
// maximum size and creates the required list.
// If it overlaps with an area that has already been uploaded, the overlapped
// upload area will be canceled and absorbed by the untreated area.
// If the list creation process is complete and areas smaller than the maximum
// size remain, those area will be reset to untreated_start and untreated_size
// and returned to the caller.
// If the called untreated area is smaller than the maximum size of the
// multipart upload, no list will be created.
//
// [NOTE]
// Maximum multipart upload size must be uploading boundary.
//
bool PseudoFdInfo::ExtractUploadPartsFromUntreatedArea(off_t untreated_start, off_t untreated_size, mp_part_list_t& to_upload_list, filepart_list_t& cancel_upload_list, off_t max_mp_size)
{
    if(untreated_start < 0 || untreated_size <= 0){
        S3FS_PRN_ERR("Parameters are wrong(untreated_start=%lld, untreated_size=%lld).", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size));
        return false;
    }

    // Initialize lists
    to_upload_list.clear();
    cancel_upload_list.clear();

    //
    // Align start position with maximum multipart upload boundaries
    //
    off_t aligned_start = (untreated_start / max_mp_size) * max_mp_size;
    off_t aligned_size  = untreated_size + (untreated_start - aligned_start);

    //
    // Check aligned untreated size
    //
    if(aligned_size < max_mp_size){
        S3FS_PRN_INFO("untreated area(start=%lld, size=%lld) to aligned boundary(start=%lld, size=%lld) is smaller than max mp size(%lld), so nothing to do.", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size), static_cast<long long int>(aligned_start), static_cast<long long int>(aligned_size), static_cast<long long int>(max_mp_size));
        return true;    // successful termination
    }

    //
    // Check each unloaded area in list
    //
    // [NOTE]
    // The uploaded area must be to be aligned by boundary.
    // Also, it is assumed that it must not be a copy area.
    // So if the areas overlap, include uploaded area as an untreated area.
    //
    {
        const std::lock_guard<std::mutex> lock(upload_list_lock);

        for(auto cur_iter = upload_list.begin(); cur_iter != upload_list.end(); /* ++cur_iter */){
            // Check overlap
            if((cur_iter->startpos + cur_iter->size - 1) < aligned_start || (aligned_start + aligned_size - 1) < cur_iter->startpos){
                // Areas do not overlap
                ++cur_iter;

            }else{
                // The areas overlap
                //
                // Since the start position of the uploaded area is aligned with the boundary,
                // it is not necessary to check the start position.
                // If the uploaded area exceeds the untreated area, expand the untreated area.
                //
                if((aligned_start + aligned_size - 1) < (cur_iter->startpos + cur_iter->size - 1)){
                    aligned_size += (cur_iter->startpos + cur_iter->size) - (aligned_start + aligned_size);
                }

                //
                // Add this to cancel list
                //
                cancel_upload_list.push_back(*cur_iter);            // Copy and Push to cancel list
                cur_iter = upload_list.erase(cur_iter);
            }
        }
    }

    //
    // Add upload area to the list
    //
    while(max_mp_size <= aligned_size){
        int part_num = static_cast<int>((aligned_start / max_mp_size) + 1);
        to_upload_list.emplace_back(aligned_start, max_mp_size, part_num);

        aligned_start += max_mp_size;
        aligned_size  -= max_mp_size;
    }

    return true;
}

//
// Extract the area lists to be uploaded/downloaded for the entire file.
//
// [Parameters]
// to_upload_list       : A list of areas to upload in multipart upload.
// to_copy_list         : A list of areas for copy upload in multipart upload.
// to_download_list     : A list of areas that must be downloaded before multipart upload.
// cancel_upload_list   : A list of areas that have already been uploaded and will be canceled(overwritten).
// wait_upload_complete : If cancellation areas exist, this flag is set to true when it is necessary to wait until the upload of those cancellation areas is complete.
// file_size            : The size of the upload file.
// use_copy             : Specify true if copy multipart upload is available.
//
// [NOTE]
// The untreated_list in fdentity does not change, but upload_list is changed.
// (If you want to restore it, you can use cancel_upload_list.)
//
bool PseudoFdInfo::ExtractUploadPartsFromAllArea(UntreatedParts& untreated_list, mp_part_list_t& to_upload_list, mp_part_list_t& to_copy_list, mp_part_list_t& to_download_list, filepart_list_t& cancel_upload_list, bool& wait_upload_complete, off_t max_mp_size, off_t file_size, bool use_copy)
{
    const std::lock_guard<std::mutex> lock(upload_list_lock);

    // Initialize lists
    to_upload_list.clear();
    to_copy_list.clear();
    to_download_list.clear();
    cancel_upload_list.clear();
    wait_upload_complete = false;

    // Duplicate untreated list
    untreated_list_t dup_untreated_list;
    untreated_list.Duplicate(dup_untreated_list);

    // Initialize the iterator of each list first
    auto dup_untreated_iter = dup_untreated_list.begin();
    auto uploaded_iter      = upload_list.begin();

    //
    // Loop to extract areas to upload and download
    //
    // Check at the boundary of the maximum upload size from the beginning of the file
    //
    for(off_t cur_start = 0, cur_size = 0; cur_start < file_size; cur_start += cur_size){
        //
        // Set part size
        // (To avoid confusion, the area to be checked is called the "current area".)
        //
        cur_size = ((cur_start + max_mp_size) <= file_size ? max_mp_size : (file_size - cur_start));

        //
        // Extract the untreated erea that overlaps this current area.
        // (The extracted area is deleted from dup_untreated_list.)
        //
        untreated_list_t cur_untreated_list;
        for(cur_untreated_list.clear(); dup_untreated_iter != dup_untreated_list.end(); ){
            if((dup_untreated_iter->start < (cur_start + cur_size)) && (cur_start < (dup_untreated_iter->start + dup_untreated_iter->size))){
                // this untreated area is overlap
                off_t tmp_untreated_start;
                off_t tmp_untreated_size;
                if(dup_untreated_iter->start < cur_start){
                    // [NOTE]
                    // This untreated area overlaps with the current area, but starts
                    // in front of the target area.
                    // This state should not be possible, but if this state is detected,
                    // the part before the target area will be deleted.
                    //
                    tmp_untreated_start = cur_start;
                    tmp_untreated_size  = dup_untreated_iter->size - (cur_start - dup_untreated_iter->start);
                }else{
                    tmp_untreated_start = dup_untreated_iter->start;
                    tmp_untreated_size  = dup_untreated_iter->size;
                }

                //
                // Check the end of the overlapping untreated area.
                //
                if((tmp_untreated_start + tmp_untreated_size) <= (cur_start + cur_size)){
                    //
                    // All of untreated areas are within the current area
                    //
                    // - Add this untreated area to cur_untreated_list
                    // - Delete this from dup_untreated_list
                    //
                    cur_untreated_list.emplace_back(tmp_untreated_start, tmp_untreated_size);
                    dup_untreated_iter = dup_untreated_list.erase(dup_untreated_iter);
                }else{
                    //
                    // The untreated area exceeds the end of the current area
                    //

                    // Adjust untreated area
                    tmp_untreated_size  = (cur_start + cur_size) - tmp_untreated_start;

                    // Add adjusted untreated area to cur_untreated_list
                    cur_untreated_list.emplace_back(tmp_untreated_start, tmp_untreated_size);

                    // Remove this adjusted untreated area from the area pointed
                    // to by dup_untreated_iter.
                    dup_untreated_iter->size  = (dup_untreated_iter->start + dup_untreated_iter->size) - (cur_start + cur_size);
                    dup_untreated_iter->start = tmp_untreated_start + tmp_untreated_size;
                }

            }else if((cur_start + cur_size - 1) < dup_untreated_iter->start){
                // this untreated area is over the current area, thus break loop.
                break;
            }else{
                ++dup_untreated_iter;
            }
        }

        //
        // Check uploaded area
        //
        // [NOTE]
        // The uploaded area should be aligned with the maximum upload size boundary.
        // It also assumes that each size of uploaded area must be a maximum upload
        // size.
        //
        auto overlap_uploaded_iter = upload_list.end();
        for(; uploaded_iter != upload_list.end(); ++uploaded_iter){
            if((cur_start < (uploaded_iter->startpos + uploaded_iter->size)) && (uploaded_iter->startpos < (cur_start + cur_size))){
                if(overlap_uploaded_iter != upload_list.end()){
                    //
                    // Something wrong in this unloaded area.
                    //
                    // This area is not aligned with the boundary, then this condition
                    // is unrecoverable and return failure.
                    //
                    S3FS_PRN_ERR("The uploaded list may not be the boundary for the maximum multipart upload size. No further processing is possible.");
                    return false;
                }
                // Set this iterator to overlap iter
                overlap_uploaded_iter = uploaded_iter;

            }else if((cur_start + cur_size - 1) < uploaded_iter->startpos){
                break;
            }
        }

        //
        // Create upload/download/cancel/copy list for this current area
        //
        int part_num = static_cast<int>((cur_start / max_mp_size) + 1);
        if(cur_untreated_list.empty()){
            //
            // No untreated area was detected in this current area
            //
            if(overlap_uploaded_iter != upload_list.end()){
                //
                // This current area already uploaded, then nothing to add to lists.
                //
                S3FS_PRN_DBG("Already uploaded: start=%lld, size=%lld", static_cast<long long int>(cur_start), static_cast<long long int>(cur_size));

            }else{
                //
                // This current area has not been uploaded
                // (neither an uploaded area nor an untreated area.)
                //
                if(use_copy){
                    //
                    // Copy multipart upload available
                    //
                    S3FS_PRN_DBG("To copy: start=%lld, size=%lld", static_cast<long long int>(cur_start), static_cast<long long int>(cur_size));
                    to_copy_list.emplace_back(cur_start, cur_size, part_num);
                }else{
                    //
                    // This current area needs to be downloaded and uploaded
                    //
                    S3FS_PRN_DBG("To download and upload: start=%lld, size=%lld", static_cast<long long int>(cur_start), static_cast<long long int>(cur_size));
                    to_download_list.emplace_back(cur_start, cur_size);
                    to_upload_list.emplace_back(cur_start, cur_size, part_num);
                }
            }
        }else{
            //
            // Found untreated area in this current area
            //
            if(overlap_uploaded_iter != upload_list.end()){
                //
                // This current area is also the uploaded area
                //
                // [NOTE]
                // The uploaded area is aligned with boundary, there are all data in
                // this current area locally(which includes all data of untreated area).
                // So this current area only needs to be uploaded again.
                //
                S3FS_PRN_DBG("Cancel upload: start=%lld, size=%lld", static_cast<long long int>(overlap_uploaded_iter->startpos), static_cast<long long int>(overlap_uploaded_iter->size));

                if(!overlap_uploaded_iter->uploaded){
                    S3FS_PRN_DBG("This cancel upload area is still uploading, so you must wait for it to complete before starting any Stream uploads.");
                    wait_upload_complete = true;
                }
                cancel_upload_list.push_back(*overlap_uploaded_iter);               // add this uploaded area to cancel_upload_list
                uploaded_iter = upload_list.erase(overlap_uploaded_iter);           // remove it from upload_list

                S3FS_PRN_DBG("To upload: start=%lld, size=%lld", static_cast<long long int>(cur_start), static_cast<long long int>(cur_size));
                to_upload_list.emplace_back(cur_start, cur_size, part_num);   // add new uploading area to list

            }else{
                //
                // No uploaded area overlap this current area
                // (Areas other than the untreated area must be downloaded.)
                //
                // [NOTE]
                // Need to consider the case where there is a gap between the start
                // of the current area and the untreated area.
                // This gap is the area that should normally be downloaded.
                // But it is the area that can be copied if we can use copy multipart
                // upload. Then If we can use copy multipart upload and the previous
                // area is used copy multipart upload, this gap will be absorbed by
                // the previous area.
                // Unifying the copy multipart upload area can reduce the number of
                // upload requests.
                //
                off_t tmp_cur_start = cur_start;
                off_t tmp_cur_size  = cur_size;
                off_t changed_start = cur_start;
                off_t changed_size  = cur_size;
                bool  first_area    = true;
                for(auto tmp_cur_untreated_iter = cur_untreated_list.cbegin(); tmp_cur_untreated_iter != cur_untreated_list.cend(); ++tmp_cur_untreated_iter, first_area = false){
                    if(tmp_cur_start < tmp_cur_untreated_iter->start){
                        //
                        // Detected a gap at the start of area
                        //
                        bool include_prev_copy_part = false;
                        if(first_area && use_copy && !to_copy_list.empty()){
                            //
                            // Make sure that the area of the last item in to_copy_list
                            // is contiguous with this current area.
                            //
                            // [NOTE]
                            // Areas can be unified if the total size of the areas is
                            // within 5GB and the remaining area after unification is
                            // larger than the minimum multipart upload size.
                            //
                            auto copy_riter = to_copy_list.rbegin();

                            if( (copy_riter->start + copy_riter->size) == tmp_cur_start &&
                                (copy_riter->size + (tmp_cur_untreated_iter->start - tmp_cur_start)) <= FIVE_GB &&
                                ((tmp_cur_start + tmp_cur_size) - tmp_cur_untreated_iter->start) >= MIN_MULTIPART_SIZE )
                            {
                                //
                                // Unify to this area to previous copy area.
                                //
                                copy_riter->size += tmp_cur_untreated_iter->start - tmp_cur_start;
                                S3FS_PRN_DBG("Resize to copy: start=%lld, size=%lld", static_cast<long long int>(copy_riter->start), static_cast<long long int>(copy_riter->size));

                                changed_size  -= (tmp_cur_untreated_iter->start - changed_start);
                                changed_start  = tmp_cur_untreated_iter->start;
                                include_prev_copy_part = true;
                            }
                        }
                        if(!include_prev_copy_part){
                            //
                            // If this area is not unified, need to download this area
                            //
                            S3FS_PRN_DBG("To download: start=%lld, size=%lld", static_cast<long long int>(tmp_cur_start), static_cast<long long int>(tmp_cur_untreated_iter->start - tmp_cur_start));
                            to_download_list.emplace_back(tmp_cur_start, tmp_cur_untreated_iter->start - tmp_cur_start);
                        }
                    }
                    //
                    // Set next start position
                    //
                    tmp_cur_size  = (tmp_cur_start + tmp_cur_size) - (tmp_cur_untreated_iter->start + tmp_cur_untreated_iter->size);
                    tmp_cur_start = tmp_cur_untreated_iter->start + tmp_cur_untreated_iter->size;
                }

                //
                // Add download area to list, if remaining size
                //
                if(0 < tmp_cur_size){
                    S3FS_PRN_DBG("To download: start=%lld, size=%lld", static_cast<long long int>(tmp_cur_start), static_cast<long long int>(tmp_cur_size));
                    to_download_list.emplace_back(tmp_cur_start, tmp_cur_size);
                }

                //
                // Set upload area(whole of area) to list
                //
                S3FS_PRN_DBG("To upload: start=%lld, size=%lld", static_cast<long long int>(changed_start), static_cast<long long int>(changed_size));
                to_upload_list.emplace_back(changed_start, changed_size, part_num);
            }
        }
    }
    return true;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
