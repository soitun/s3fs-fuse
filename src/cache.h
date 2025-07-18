/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Randy Rizun <rrizun@gmail.com>
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

#ifndef S3FS_CACHE_H_
#define S3FS_CACHE_H_

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common.h"
#include "metaheader.h"

//-------------------------------------------------------------------
// Structure
//-------------------------------------------------------------------
//
// Struct for stats cache
//
struct stat_cache_entry {
    struct stat       stbuf = {};
    unsigned long     hit_count = 0;
    struct timespec   cache_date = {0, 0};
    headers_t         meta;
    bool              isforce = false;
    bool              noobjcache = false;  // Flag: cache is no object for no listing.
    unsigned long     notruncate = 0L;  // 0<:   not remove automatically at checking truncate
};

typedef std::map<std::string, stat_cache_entry> stat_cache_t; // key=path

//
// Struct for symbolic link cache
//
struct symlink_cache_entry {
    std::string       link;
    unsigned long     hit_count = 0;
    struct timespec   cache_date = {0, 0};  // The function that operates timespec uses the same as Stats
};

typedef std::map<std::string, symlink_cache_entry> symlink_cache_t;

//
// Typedefs for No truncate file name cache
//
typedef std::vector<std::string> notruncate_filelist_t;                    // untruncated file name list in dir
typedef std::map<std::string, notruncate_filelist_t> notruncate_dir_map_t; // key is parent dir path

//-------------------------------------------------------------------
// Class StatCache
//-------------------------------------------------------------------
// [NOTE] About Symbolic link cache
// The Stats cache class now also has a symbolic link cache.
// It is possible to take out the Symbolic link cache in another class,
// but the cache out etc. should be synchronized with the Stats cache
// and implemented in this class.
// Symbolic link cache size and timeout use the same settings as Stats
// cache. This simplifies user configuration, and from a user perspective,
// the symbolic link cache appears to be included in the Stats cache.
//
class StatCache
{
    private:
        static StatCache       singleton;
        static std::mutex      stat_cache_lock;
        stat_cache_t           stat_cache GUARDED_BY(stat_cache_lock);
        bool                   IsExpireTime;
        bool                   IsExpireIntervalType;    // if this flag is true, cache data is updated at last access time.
        time_t                 ExpireTime;
        unsigned long          CacheSize;
        bool                   UseNegativeCache;
        symlink_cache_t        symlink_cache GUARDED_BY(stat_cache_lock);
        notruncate_dir_map_t   notruncate_file_cache GUARDED_BY(stat_cache_lock);

        StatCache();
        ~StatCache();

        void Clear();
        bool GetStat(const std::string& key, struct stat* pst, headers_t* meta, bool overcheck, const char* petag, bool* pisforce);
        // Truncate stat cache
        bool TruncateCache(bool check_only_oversize_case = false) REQUIRES(StatCache::stat_cache_lock);
        // Truncate symbolic link cache
        bool TruncateSymlink(bool check_only_oversize_case = false) REQUIRES(StatCache::stat_cache_lock);

        bool AddNotruncateCache(const std::string& key) REQUIRES(stat_cache_lock);
        bool DelNotruncateCache(const std::string& key) REQUIRES(stat_cache_lock);

        bool DelStatHasLock(const std::string& key) REQUIRES(StatCache::stat_cache_lock);
        bool DelSymlinkHasLock(const std::string& key) REQUIRES(stat_cache_lock);

    public:
        StatCache(const StatCache&) = delete;
        StatCache(StatCache&&) = delete;
        StatCache& operator=(const StatCache&) = delete;
        StatCache& operator=(StatCache&&) = delete;

        // Reference singleton
        static StatCache* getStatCacheData()
        {
            return &singleton;
        }

        // Attribute
        unsigned long GetCacheSize() const;
        unsigned long SetCacheSize(unsigned long size);
        time_t GetExpireTime() const;
        time_t SetExpireTime(time_t expire, bool is_interval = false);
        time_t UnsetExpireTime();
        bool SetNegativeCache(bool flag);
        bool EnableNegativeCache()
        {
            return SetNegativeCache(true);
        }
        bool DisableNegativeCache()
        {
            return SetNegativeCache(false);
        }
        bool IsEnabledNegativeCache() const
        {
            return UseNegativeCache;
        }

        // Get stat cache
        bool GetStat(const std::string& key, struct stat* pst, headers_t* meta, bool overcheck = true, bool* pisforce = nullptr)
        {
            return GetStat(key, pst, meta, overcheck, nullptr, pisforce);
        }
        bool GetStat(const std::string& key, struct stat* pst, bool overcheck = true)
        {
            return GetStat(key, pst, nullptr, overcheck, nullptr, nullptr);
        }
        bool GetStat(const std::string& key, headers_t* meta, bool overcheck = true)
        {
            return GetStat(key, nullptr, meta, overcheck, nullptr, nullptr);
        }
        bool HasStat(const std::string& key, bool overcheck = true)
        {
            return GetStat(key, nullptr, nullptr, overcheck, nullptr, nullptr);
        }
        bool HasStat(const std::string& key, const char* etag, bool overcheck = true)
        {
            return GetStat(key, nullptr, nullptr, overcheck, etag, nullptr);
        }
        bool HasStat(const std::string& key, struct stat* pst, const char* etag)
        {
            return GetStat(key, pst, nullptr, true, etag, nullptr);
        }

        // Cache For no object
        bool IsNoObjectCache(const std::string& key, bool overcheck = true);
        bool AddNoObjectCache(const std::string& key);

        // Add stat cache
        bool AddStat(const std::string& key, const headers_t& meta, bool forcedir = false, bool no_truncate = false);

        // Update meta stats
        bool UpdateMetaStats(const std::string& key, const headers_t& meta);

        // Change no truncate flag
        void ChangeNoTruncateFlag(const std::string& key, bool no_truncate);

        // Delete stat cache
        bool DelStat(const std::string& key)
        {
            const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);
            return DelStatHasLock(key);
        }

        // Cache for symbolic link
        bool GetSymlink(const std::string& key, std::string& value);
        bool AddSymlink(const std::string& key, const std::string& value);
        bool DelSymlink(const std::string& key) {
            const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);
            return DelSymlinkHasLock(key);
        }

        // Cache for Notruncate file
        bool GetNotruncateCache(const std::string& parentdir, notruncate_filelist_t& list);
};

#endif // S3FS_CACHE_H_

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
