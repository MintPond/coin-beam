// Copyright 2020 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LOG_VERBOSE_ENABLED
    #define LOG_VERBOSE_ENABLED 0
#endif

#include "utility/logger.h"
#include "wallet/laser/mediator.h"
#include "node/node.h"
#include "core/unittest/mini_blockchain.h"
#include "utility/test_helpers.h"
#include "laser_test_utils.h"
#include "test_helpers.h"

WALLET_TEST_INIT
#include "wallet_test_environment.cpp"

using namespace beam;
using namespace beam::wallet;
using namespace std;

namespace {
const Height kMaxTestHeight = 42;
}  // namespace

int main()
{
    int logLevel = LOG_LEVEL_DEBUG;
#if LOG_VERBOSE_ENABLED
    logLevel = LOG_LEVEL_VERBOSE;
#endif
    const auto path = boost::filesystem::system_complete("logs");
    auto logger = Logger::create(logLevel, logLevel, logLevel, "laser_test", path.string());

    Rules::get().pForks[1].m_Height = 1;
	Rules::get().FakePoW = true;
    Rules::get().MaxRollback = 5;
	Rules::get().UpdateChecksum();

    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    auto wdbFirst = createSqliteWalletDB("laser_test_close_first.db", false, false);
    auto wdbSecond = createSqliteWalletDB("laser_test_close_second.db", false, false);

    const AmountList amounts = {100000000, 100000000, 100000000, 100000000};
    for (auto amount : amounts)
    {
        Coin coinFirst = CreateAvailCoin(amount, 1);
        wdbFirst->storeCoin(coinFirst);

        Coin coinSecond = CreateAvailCoin(amount, 1);
        wdbSecond->storeCoin(coinSecond);
    }

    // m_hRevisionMaxLifeTime, m_hLockTime, m_hPostLockReserve, m_Fee
    Lightning::Channel::Params params = {80, 10, 10, 100};
    auto laserFirst = std::make_unique<laser::Mediator>(wdbFirst, params);
    auto laserSecond = std::make_unique<laser::Mediator>(wdbSecond, params);

    LaserObserver observer_1, observer_2;
    laser::ChannelIDPtr channel_1, channel_2;
    bool laser1Closed = false, laser2Closed = false;

    observer_1.onOpened = [&channel_1] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: first opened";
        channel_1 = chID;
    };
    observer_2.onOpened = [&channel_2] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: second opened";
        channel_2 = chID;
    };
    observer_1.onOpenFailed = observer_2.onOpenFailed = [] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: open failed";
        WALLET_CHECK(false);
    };
    observer_1.onClosed = [&laser1Closed] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: first closed";
        laser1Closed = true;
    };
    observer_2.onClosed = [&laser2Closed] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: second closed";
        laser2Closed = true;
    };
    observer_1.onCloseFailed = observer_2.onCloseFailed = [] (const laser::ChannelIDPtr& chID)
    {
        LOG_INFO() << "Test laser CLOSE: close failed";
        WALLET_CHECK(false);
    };
    laserFirst->AddObserver(&observer_1);
    laserSecond->AddObserver(&observer_2);

    auto newBlockFunc = [
        &laserFirst,
        &laserSecond,
        &channel_1,
        &channel_2,
        &laser1Closed,
        &laser2Closed
    ] (Height height)
    {
        if (height > kMaxTestHeight)
        {
            LOG_ERROR() << "Test laser CLOSE: time expired";
            WALLET_CHECK(false);
            io::Reactor::get_Current().stop();
        }

        if (height == 3)
        {
            laserFirst->WaitIncoming(100000000, 100000000, 101);
            auto firstWalletID = laserFirst->getWaitingWalletID();
            laserSecond->OpenChannel(100000000, 100000000, 101, firstWalletID, 10);
        }

        if (channel_1 && channel_2)
        {
            auto channel1Str = to_hex(channel_1->m_pData, channel_1->nBytes);
            WALLET_CHECK(laserFirst->GracefulClose(channel1Str));
            channel_1.reset();
            channel_2.reset();
        }

        if (laser1Closed && laser2Closed)
        {
            LOG_INFO() << "Test laser CLOSE: finished";
            io::Reactor::get_Current().stop();
        }

    };

    TestNode node(newBlockFunc, 2);
    io::Timer::Ptr timer = io::Timer::create(*mainReactor);
    timer->start(1000, true, [&node]() {node.AddBlock(); });

    laserFirst->SetNetwork(CreateNetwork(*laserFirst));
    laserSecond->SetNetwork(CreateNetwork(*laserSecond));

    mainReactor->run();

    return WALLET_CHECK_RESULT;
}