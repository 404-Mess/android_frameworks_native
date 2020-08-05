/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include <mutex>

#include "Scheduler/EventThread.h"
#include "Scheduler/RefreshRateConfigs.h"
#include "TestableScheduler.h"
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockDisplay.h"
#include "mock/MockEventThread.h"
#include "mock/MockLayer.h"
#include "mock/MockSchedulerCallback.h"

using testing::_;
using testing::Return;

namespace android {
namespace {

constexpr PhysicalDisplayId PHYSICAL_DISPLAY_ID = 999;

class SchedulerTest : public testing::Test {
protected:
    class MockEventThreadConnection : public android::EventThreadConnection {
    public:
        explicit MockEventThreadConnection(EventThread* eventThread)
              : EventThreadConnection(eventThread, ResyncCallback(),
                                      ISurfaceComposer::eConfigChangedSuppress) {}
        ~MockEventThreadConnection() = default;

        MOCK_METHOD1(stealReceiveChannel, status_t(gui::BitTube* outChannel));
        MOCK_METHOD1(setVsyncRate, status_t(uint32_t count));
        MOCK_METHOD0(requestNextVsync, void());
    };

    SchedulerTest();

    Hwc2::mock::Display mDisplay;
    const scheduler::RefreshRateConfigs mConfigs{{HWC2::Display::Config::Builder(mDisplay, 0)
                                                          .setVsyncPeriod(16'666'667)
                                                          .setConfigGroup(0)
                                                          .build()},
                                                 HwcConfigIndexType(0)};

    mock::SchedulerCallback mSchedulerCallback;

    // The scheduler should initially disable VSYNC.
    struct ExpectDisableVsync {
        ExpectDisableVsync(mock::SchedulerCallback& callback) {
            EXPECT_CALL(callback, setVsyncEnabled(false)).Times(1);
        }
    } mExpectDisableVsync{mSchedulerCallback};

    static constexpr bool kUseContentDetectionV2 = false;
    TestableScheduler mScheduler{mConfigs, mSchedulerCallback, kUseContentDetectionV2};

    Scheduler::ConnectionHandle mConnectionHandle;
    mock::EventThread* mEventThread;
    sp<MockEventThreadConnection> mEventThreadConnection;
};

SchedulerTest::SchedulerTest() {
    auto eventThread = std::make_unique<mock::EventThread>();
    mEventThread = eventThread.get();
    EXPECT_CALL(*mEventThread, registerDisplayEventConnection(_)).WillOnce(Return(0));

    mEventThreadConnection = new MockEventThreadConnection(mEventThread);

    // createConnection call to scheduler makes a createEventConnection call to EventThread. Make
    // sure that call gets executed and returns an EventThread::Connection object.
    EXPECT_CALL(*mEventThread, createEventConnection(_, _))
            .WillRepeatedly(Return(mEventThreadConnection));

    mConnectionHandle = mScheduler.createConnection(std::move(eventThread));
    EXPECT_TRUE(mConnectionHandle);
}

} // namespace

TEST_F(SchedulerTest, invalidConnectionHandle) {
    Scheduler::ConnectionHandle handle;

    const sp<IDisplayEventConnection> connection =
            mScheduler.createDisplayEventConnection(handle,
                                                    ISurfaceComposer::eConfigChangedSuppress);

    EXPECT_FALSE(connection);
    EXPECT_FALSE(mScheduler.getEventConnection(handle));

    // The EXPECT_CALLS make sure we don't call the functions on the subsequent event threads.
    EXPECT_CALL(*mEventThread, onHotplugReceived(_, _)).Times(0);
    mScheduler.onHotplugReceived(handle, PHYSICAL_DISPLAY_ID, false);

    EXPECT_CALL(*mEventThread, onScreenAcquired()).Times(0);
    mScheduler.onScreenAcquired(handle);

    EXPECT_CALL(*mEventThread, onScreenReleased()).Times(0);
    mScheduler.onScreenReleased(handle);

    std::string output;
    EXPECT_CALL(*mEventThread, dump(_)).Times(0);
    mScheduler.dump(handle, output);
    EXPECT_TRUE(output.empty());

    EXPECT_CALL(*mEventThread, setPhaseOffset(_)).Times(0);
    mScheduler.setPhaseOffset(handle, 10);
}

TEST_F(SchedulerTest, validConnectionHandle) {
    const sp<IDisplayEventConnection> connection =
            mScheduler.createDisplayEventConnection(mConnectionHandle,
                                                    ISurfaceComposer::eConfigChangedSuppress);

    ASSERT_EQ(mEventThreadConnection, connection);
    EXPECT_TRUE(mScheduler.getEventConnection(mConnectionHandle));

    EXPECT_CALL(*mEventThread, onHotplugReceived(PHYSICAL_DISPLAY_ID, false)).Times(1);
    mScheduler.onHotplugReceived(mConnectionHandle, PHYSICAL_DISPLAY_ID, false);

    EXPECT_CALL(*mEventThread, onScreenAcquired()).Times(1);
    mScheduler.onScreenAcquired(mConnectionHandle);

    EXPECT_CALL(*mEventThread, onScreenReleased()).Times(1);
    mScheduler.onScreenReleased(mConnectionHandle);

    std::string output("dump");
    EXPECT_CALL(*mEventThread, dump(output)).Times(1);
    mScheduler.dump(mConnectionHandle, output);
    EXPECT_FALSE(output.empty());

    EXPECT_CALL(*mEventThread, setPhaseOffset(10)).Times(1);
    mScheduler.setPhaseOffset(mConnectionHandle, 10);

    static constexpr size_t kEventConnections = 5;
    EXPECT_CALL(*mEventThread, getEventThreadConnectionCount()).WillOnce(Return(kEventConnections));
    EXPECT_EQ(kEventConnections, mScheduler.getEventThreadConnectionCount(mConnectionHandle));
}

TEST_F(SchedulerTest, noLayerHistory) {
    // Layer history should not be created if there is a single config.
    ASSERT_FALSE(mScheduler.hasLayerHistory());

    TestableSurfaceFlinger flinger;
    mock::MockLayer layer(flinger.flinger());

    // Content detection should be no-op.
    mScheduler.registerLayer(&layer);
    mScheduler.recordLayerHistory(&layer, 0, LayerHistory::LayerUpdateType::Buffer);

    constexpr bool kPowerStateNormal = true;
    mScheduler.setDisplayPowerState(kPowerStateNormal);

    constexpr uint32_t kDisplayArea = 999'999;
    mScheduler.onPrimaryDisplayAreaChanged(kDisplayArea);

    EXPECT_CALL(mSchedulerCallback, changeRefreshRate(_, _)).Times(0);
    mScheduler.chooseRefreshRateForContent();
}

} // namespace android
