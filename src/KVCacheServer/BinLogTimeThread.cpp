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
#include "BinLogTimeThread.h"
#include "CacheServer.h"

TC_ThreadLock BinLogTimeThread::_lock;

void BinLogTimeThread::init(const string& sConf)
{
    _configFile = sConf;
    _tcConf.parseFile(_configFile);

    _srp_binlogSynDiff = Application::getCommunicator()->getStatReport()->createPropertyReport("M/S_ReplicationLatency", PropertyReport::avg());
    if (_srp_binlogSynDiff == 0)
    {
        TLOG_ERROR("BinLogTimeThread::init createPropertyReport error" << endl);
        assert(false);
    }

    _saveSyncTimeInterval = TC_Common::strto<int>(_tcConf.get("/Main/BinLog<SaveSyncTimeInterval>", "10"));

    TLOG_DEBUG("BinLogTimerThread::init succ" << endl);
}

void BinLogTimeThread::reload()
{
    _tcConf.parseFile(_configFile);
    _saveSyncTimeInterval = TC_Common::strto<int>(_tcConf.get("/Main/BinLog<SaveSyncTimeInterval>", "10"));

    TLOG_DEBUG("BinLogTimeThread::reload Succ" << endl);
}

void BinLogTimeThread::createThread()
{
    //创建线程
    pthread_t thread;

    if (!_isStart)
    {
        _isStart = true;
        if (pthread_create(&thread, NULL, Run, (void*)this) != 0)
        {
            throw runtime_error("Create BinLogTimeThread fail");
        }
    }
}

void* BinLogTimeThread::Run(void* arg)
{
    pthread_detach(pthread_self());
    BinLogTimeThread* pthis = (BinLogTimeThread*)arg;
    pthis->setRuning(true);
    string sAddr = pthis->getBakSourceAddr();
    if (sAddr.length() > 0)
    {
        pthis->_binLogPrx = Application::getCommunicator()->stringToProxy<BinLogPrx>(sAddr);
    }

    pthis->getSyncTime();

    usleep(3000000);

    time_t tLastSaveTime = 0;
    while (pthis->isStart())
    {
        time_t tNow = TC_TimeProvider::getInstance()->getNow();
        if (tNow - tLastSaveTime > pthis->_saveSyncTimeInterval)
        {
            pthis->saveSyncTime();
            tLastSaveTime = tNow;
        }
        if (g_app.gstat()->isSlaveCreating() == true)
        {
            pthis->_isInSlaveCreating = true;
            usleep(100000);
            continue;
        }
        pthis->_isInSlaveCreating = false;
        if (g_app.gstat()->serverType() != SLAVE)
        {
            pthis->reportBinLogDiff();
            usleep(100000);
            continue;
        }

        try
        {
            string sTmpAddr = pthis->getBakSourceAddr();
            if (sTmpAddr != sAddr)
            {
                TLOG_DEBUG("MasterBinLogAddr changed from " << sAddr << " to " << sTmpAddr << endl);
                sAddr = sTmpAddr;
                if (sAddr.length() > 0)
                {
                    pthis->_binLogPrx = Application::getCommunicator()->stringToProxy<BinLogPrx>(sAddr);
                    pthis->_binLogPrx->tars_timeout(1000);
                }
                else
                {
                    pthis->reportBinLogDiff();
                    usleep(100000);
                    continue;
                }
            }

            if (sTmpAddr.size() > 0)
            {
                uint32_t tLastBinLog = 0;
                int iRet = pthis->_binLogPrx->getLastBinLogTime(tLastBinLog);
                if (iRet == 0)
                {
                    UpdateLastBinLogTime(tLastBinLog);
                }
            }
            else
            {
                TLOG_ERROR("[BinLogTimeThread::Run] getBakSourceAddr is empty." << endl);
            }

            pthis->_failCnt = 0;
            pthis->reportBinLogDiff();

            usleep(500000);
            continue;
        }
        catch (const std::exception &ex)
        {
            TLOG_ERROR("[BinLogTimeThread::Run] exception: " << ex.what() << endl);
        }
        catch (...)
        {
            TLOG_ERROR("[BinLogTimeThread::Run] unkown exception" << endl);
        }
        pthis->reportBinLogDiff();
        if (++pthis->_failCnt >= 3)
        {
            g_app.ppReport(PPReport::SRP_BINLOG_ERR, 1);
            pthis->_failCnt = 0;
            sleep(1);
        }
        else
            usleep(100000);
    }
    pthis->setRuning(false);
    pthis->setStart(false);
    return NULL;
}

void BinLogTimeThread::UpdateLastBinLogTime(uint32_t tLast, uint32_t tSync)
{
    TC_ThreadLock::Lock lock(_lock);

    g_app.gstat()->setBinlogTime(tSync, tLast);
}

string BinLogTimeThread::getBakSourceAddr()
{
    string sServerName = ServerConfig::Application + "." + ServerConfig::ServerName;

    ServerInfo server;
    int iRet = g_route_table.getBakSource(sServerName, server);
    if (iRet != UnpackTable::RET_SUCC)
    {
        TLOG_ERROR("[BinLogTimeThread::getBakSourceAddr] getBakSource error, iRet = " << iRet << endl);
        g_app.ppReport(PPReport::SRP_BINLOG_ERR, 1);
        return "";
    }

    string sBakSourceAddr;
    sBakSourceAddr = server.BinLogServant;
    return sBakSourceAddr;
}

void BinLogTimeThread::reportBinLogDiff()
{
    static time_t tLastReport = 0;
    time_t tNow = TC_TimeProvider::getInstance()->getNow();
    if (tNow - tLastReport < 60)
    {
        return;
    }

    if (g_app.gstat()->getBinlogTimeSync() != 0)
    {
        _srp_binlogSynDiff->report(g_app.gstat()->getBinlogTimeLast() - g_app.gstat()->getBinlogTimeSync());
    }

    tLastReport = tNow;
}

void BinLogTimeThread::getSyncTime()
{
    string sFile = ServerConfig::DataPath + "/sync_time.data";

    ifstream fin;
    fin.open(sFile.c_str(), ios::in | ios::binary);
    if (!fin)
    {
        TLOG_ERROR("open file: " << sFile << " error" << endl);
        return;
    }

    string line;
    if (getline(fin, line))
    {
        vector<string> vt;
        vt = TC_Common::sepstr<string>(line, "|");
        if (vt.size() != 2)
        {
            TLOG_ERROR("sync_time.data is error" << endl);
            fin.close();
            return;
        }
        else
        {
            uint32_t tLast = TC_Common::strto<uint32_t>(vt[0]);
            uint32_t tSync = TC_Common::strto<uint32_t>(vt[1]);
            UpdateLastBinLogTime(tLast, tSync);
        }
    }
    else
    {
        TLOG_ERROR("sync_time.data is error" << endl);
    }
    fin.close();
}

void BinLogTimeThread::saveSyncTime()
{
    string sFile = ServerConfig::DataPath + "/sync_time.data";

    int fd = open(sFile.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fd < 0)
    {
        TLOG_ERROR("Save SyncTime error, open " << sFile << " failed" << endl);
        g_app.ppReport(PPReport::SRP_EX, 1);
        return;
    }

    //在同步时间文件后面加10个空格，用于覆盖旧的内容
    string line;
    {
        TC_ThreadLock::Lock lock(_lock);
        string sBlank(10, ' ');
        line = TC_Common::tostr(g_app.gstat()->getBinlogTimeLast()) + "|" + TC_Common::tostr(g_app.gstat()->getBinlogTimeSync());
        line += sBlank;
    }

    write(fd, line.c_str(), line.size());
    close(fd);
}

