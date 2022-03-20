/**
* Tencent is pleased to support the open source community by making DCache available.
* Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
* Licensed under the BSD 3-Clause License (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of the License at
*
* https://opensource.org/licenses/BSD-3-Clause
*
* Unless required by applicable law or agreed to in writing, software distributed under
* the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
* either express or implied. See the License for the specific language governing permissions
* and limitations under the License.
*/
#include "SyncAllThread.h"
#include "CacheServer.h"

void SyncAllThread::init(const string &sConf)
{
    _config = sConf;
    _tcConf.parseFile(_config);
    _syncTime = TC_Common::strto<int>(_tcConf["/Main/Cache<SyncTime>"]);


    TLOG_DEBUG("SyncAllThread::init succ" << endl);
}

void SyncAllThread::reload()
{
    _tcConf.parseFile(_config);

    _syncTime = TC_Common::strto<int>(_tcConf["/Main/Cache<SyncTime>"]);

    TLOG_DEBUG("SyncAllThread::reload succ" << endl);
}

void SyncAllThread::createThread()
{
    //创建线程
    pthread_t thread;

    if (!_isStart)
    {
        _isStart = true;
        if (pthread_create(&thread, NULL, Run, (void*)this) != 0)
        {
            throw runtime_error("Create SyncAllThread fail");
        }
    }
}

void* SyncAllThread::Run(void* arg)
{
    pthread_detach(pthread_self());
    SyncAllThread* pthis = (SyncAllThread*)arg;
    pthis->setRuning(true);

    time_t tNow = TC_TimeProvider::getInstance()->getNow();
    time_t tSync = tNow + pthis->getSyncTime() + 1;

    pthis->sync();
    while (pthis->isStart())
    {
        try
        {
            int iRet = pthis->syncData(tSync);
            if (iRet == TC_HashMapMalloc::RT_OK)
            {
                TLOG_DEBUG("SyncAllThread::Run SyncAll data Succ" << endl);
                break;
            }
            else if (iRet != TC_HashMapMalloc::RT_NEED_SYNC && iRet != TC_HashMapMalloc::RT_NONEED_SYNC && iRet != TC_HashMapMalloc::RT_ONLY_KEY)
            {
                TLOG_ERROR("SyncAllThread::Run SyncAll data error:" << iRet << endl);
                g_app.ppReport(PPReport::SRP_CACHE_ERR, 1);
                break;
            }
        }
        catch (const std::exception & ex)
        {
            TLOG_ERROR("SyncAllThread::Run exception: " << ex.what() << endl);
            break;
        }
    }

    pthis->setRuning(false);
    pthis->setStart(false);
    return NULL;
}


void SyncAllThread::sync()
{
    g_sHashMap.sync();
}

int SyncAllThread::syncData(time_t t)
{
    CanSync& canSync = g_app.gstat()->getCanSync();
    int iRet = g_sHashMap.syncOnce(t, canSync);
    return iRet;
}

