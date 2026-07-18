#include <gtest/gtest.h>

#include "waveform/isynctimeprovider.h"
#include "waveform/visualplayposition.h"

namespace {

class FakeVSyncTimeProvider final : public VSyncTimeProvider {
  public:
    int fromTimerToNextSyncMicros(const PerformanceTimer&) override {
        return referenceToDisplayMicros;
    }

    int getSyncIntervalTimeMicros() const override {
        return syncIntervalMicros;
    }

    const char* timingSourceName() const override {
        return "test-vsync";
    }

    int referenceToDisplayMicros{500};
    int syncIntervalMicros{16667};
};

void setPosition(VisualPlayPosition* pPosition,
        double playPosition,
        double playRate,
        double positionStep,
        bool loopEnabled = false,
        double loopStart = 0.0,
        double loopEnd = 0.0) {
    PerformanceTimer callbackTime;
    callbackTime.start();
    VisualPlayPosition::setCallbackEntryToDacSecs(0.0, callbackTime);
    pPosition->set(playPosition,
            playRate,
            positionStep,
            playPosition,
            playRate,
            SlipModeState::Disabled,
            loopEnabled,
            false,
            false,
            loopStart,
            loopEnd,
            180.0,
            1000.0);
}

TEST(VisualPlayPositionTest, ProjectsEngineSnapshotToDisplayTime) {
    VisualPlayPosition position(QString{});
    FakeVSyncTimeProvider timing;
    setPosition(&position, 0.5, 1.0, 0.001);

    EXPECT_DOUBLE_EQ(0.5005, position.getAtNextVSync(&timing));
}

TEST(VisualPlayPositionTest, AppliesReverseRateWithoutQmlVelocityEstimation) {
    VisualPlayPosition position(QString{});
    FakeVSyncTimeProvider timing;
    setPosition(&position, 0.5, -1.0, 0.001);

    EXPECT_DOUBLE_EQ(0.4995, position.getAtNextVSync(&timing));
}

TEST(VisualPlayPositionTest, WrapsForwardPlaybackAtLoopBoundary) {
    VisualPlayPosition position(QString{});
    FakeVSyncTimeProvider timing;
    setPosition(&position, 0.799, 1.0, 0.01, true, 0.2, 0.8);

    EXPECT_NEAR(0.204, position.getAtNextVSync(&timing), 1e-12);
}

TEST(VisualPlayPositionTest, NewEngineSnapshotIsVisibleWithoutCorrectionFilter) {
    VisualPlayPosition position(QString{});
    FakeVSyncTimeProvider timing;
    setPosition(&position, 0.2, 1.0, 0.001);
    EXPECT_DOUBLE_EQ(0.2005, position.getAtNextVSync(&timing));

    setPosition(&position, 0.7, 0.0, 0.001);
    EXPECT_DOUBLE_EQ(0.7, position.getAtNextVSync(&timing));
}

TEST(VisualPlayPositionTest, InvalidSnapshotIsExplicit) {
    VisualPlayPosition position(QString{});
    FakeVSyncTimeProvider timing;

    EXPECT_DOUBLE_EQ(-1.0, position.getAtNextVSync(&timing));
}

} // namespace
