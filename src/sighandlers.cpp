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

#include <cstdio>
#include <csignal>
#include <memory>
#include <thread>
#include <utility>

#include "psemaphore.h"
#include "s3fs_logger.h"
#include "sighandlers.h"
#include "fdcache.h"

//-------------------------------------------------------------------
// Class S3fsSignals
//-------------------------------------------------------------------
std::unique_ptr<S3fsSignals> S3fsSignals::pSingleton;
bool S3fsSignals::enableUsr1         = false;

//-------------------------------------------------------------------
// Class methods
//-------------------------------------------------------------------
bool S3fsSignals::Initialize()
{
    if(!S3fsSignals::pSingleton){
        S3fsSignals::pSingleton = std::make_unique<S3fsSignals>();
    }
    return true;
}

bool S3fsSignals::Destroy()
{
    S3fsSignals::pSingleton.reset();
    return true;
}

void S3fsSignals::HandlerUSR1(int sig)
{
    if(SIGUSR1 != sig){
        S3FS_PRN_ERR("The handler for SIGUSR1 received signal(%d)", sig);
        return;
    }

    S3fsSignals* pSigobj = S3fsSignals::get();
    if(!pSigobj){
        S3FS_PRN_ERR("S3fsSignals object is not initialized.");
        return;
    }

    if(!pSigobj->WakeupUsr1Thread()){
        S3FS_PRN_ERR("Failed to wakeup the thread for SIGUSR1.");
        return;
    }
}

bool S3fsSignals::SetUsr1Handler(const char* path)
{
    if(!FdManager::HaveLseekHole()){
        S3FS_PRN_ERR("Could not set SIGUSR1 for checking cache, because this system does not support SEEK_DATA/SEEK_HOLE in lseek function.");
        return false;
    }

    // set output file
    if(!FdManager::SetCacheCheckOutput(path)){
        S3FS_PRN_ERR("Could not set output file(%s) for checking cache.", path ? path : "null(stdout)");
        return false;
    }

    S3fsSignals::enableUsr1 = true;

    return true;
}

void S3fsSignals::CheckCacheWorker(Semaphore* pSem)
{
    if(!pSem){
        return;
    }
    if(!S3fsSignals::enableUsr1){
        return;
    }

    // wait and loop
    while(S3fsSignals::enableUsr1){
        // wait
        pSem->acquire();

        // cppcheck-suppress unmatchedSuppression
        // cppcheck-suppress knownConditionTrueFalse
        if(!S3fsSignals::enableUsr1){
            break;  // asap
        }

        // check all cache
        if(!FdManager::get()->CheckAllCache()){
            S3FS_PRN_ERR("Processing failed due to some problem.");
        }

        // do not allow request queuing
        while(pSem->try_acquire());
    }
}

void S3fsSignals::HandlerUSR2(int sig)
{
    if(SIGUSR2 == sig){
        S3fsLog::BumpupLogLevel();
    }else{
        S3FS_PRN_ERR("The handler for SIGUSR2 received signal(%d)", sig);
    }
}

bool S3fsSignals::InitUsr2Handler()
{
    struct sigaction sa{};
    sa.sa_handler = S3fsSignals::HandlerUSR2;
    sa.sa_flags   = SA_RESTART;
    if(0 != sigaction(SIGUSR2, &sa, nullptr)){
        return false;
    }
    return true;
}

void S3fsSignals::HandlerHUP(int sig)
{
    if(SIGHUP == sig){
        S3fsLog::ReopenLogfile();
    }else{
        S3FS_PRN_ERR("The handler for SIGHUP received signal(%d)", sig);
    }
}

bool S3fsSignals::InitHupHandler()
{
    struct sigaction sa{};
    sa.sa_handler = S3fsSignals::HandlerHUP;
    sa.sa_flags   = SA_RESTART;
    if(0 != sigaction(SIGHUP, &sa, nullptr)){
        return false;
    }
    return true;
}

//-------------------------------------------------------------------
// Methods
//-------------------------------------------------------------------
S3fsSignals::S3fsSignals()
{
    if(S3fsSignals::enableUsr1){
        if(!InitUsr1Handler()){
            S3FS_PRN_ERR("failed creating thread for SIGUSR1 handler, but continue...");
        }
    }
    if(!S3fsSignals::InitUsr2Handler()){
        S3FS_PRN_ERR("failed to initialize SIGUSR2 handler for bumping log level, but continue...");
    }
    if(!S3fsSignals::InitHupHandler()){
        S3FS_PRN_ERR("failed to initialize SIGHUP handler for reopen log file, but continue...");
    }
}

S3fsSignals::~S3fsSignals()
{
    if(S3fsSignals::enableUsr1){
        if(!DestroyUsr1Handler()){
            S3FS_PRN_ERR("failed stopping thread for SIGUSR1 handler, but continue...");
        }
    }
}

bool S3fsSignals::InitUsr1Handler()
{
    if(pThreadUsr1 || pSemUsr1){
        S3FS_PRN_ERR("Already run thread for SIGUSR1");
        return false;
    }

    // create thread
    auto pSemUsr1_tmp = std::make_unique<Semaphore>(0);
    pThreadUsr1 = std::make_unique<std::thread>(S3fsSignals::CheckCacheWorker, pSemUsr1_tmp.get());
    pSemUsr1 = std::move(pSemUsr1_tmp);

    // set handler
    struct sigaction sa{};
    sa.sa_handler = S3fsSignals::HandlerUSR1;
    sa.sa_flags   = SA_RESTART;
    if(0 != sigaction(SIGUSR1, &sa, nullptr)){
        S3FS_PRN_ERR("Could not set signal handler for SIGUSR1");
        DestroyUsr1Handler();
        return false;
    }

    return true;
}

bool S3fsSignals::DestroyUsr1Handler()
{
    if(!pThreadUsr1 || !pSemUsr1){
        return false;
    }
    // for thread exit
    S3fsSignals::enableUsr1 = false;

    // wakeup thread
    pSemUsr1->release();

    // wait for thread exiting
    pThreadUsr1->join();
    pSemUsr1.reset();
    pThreadUsr1.reset();

    return true;
}

bool S3fsSignals::WakeupUsr1Thread()
{
    if(!pThreadUsr1 || !pSemUsr1){
        S3FS_PRN_ERR("The thread for SIGUSR1 is not setup.");
        return false;
    }
    pSemUsr1->release();
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
