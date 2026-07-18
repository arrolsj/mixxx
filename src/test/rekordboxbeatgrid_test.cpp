#include <gtest/gtest.h>

#include "library/rekordbox/rekordboxbeatgrid.h"

namespace mixxx::rekordbox {
namespace {

TEST(RekordboxBeatGridTest, KeepsGridThatAlreadyStartsOnDownbeat) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
    };

    const auto anchored = withDownbeatAnchor(positions, 1, 500.0);

    EXPECT_EQ(positions, anchored);
}

TEST(RekordboxBeatGridTest, PrependsPrerollWithoutChangingExportedPositions) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
            audio::FramePos(2000),
    };

    for (int firstBeatNumber = 2; firstBeatNumber <= 4; ++firstBeatNumber) {
        const auto anchored = withDownbeatAnchor(positions, firstBeatNumber, 500.0);
        ASSERT_EQ(positions.size() + firstBeatNumber - 1, anchored.size());
        EXPECT_DOUBLE_EQ(
                1000.0 - (firstBeatNumber - 1) * 500.0, anchored.first().value());
        for (int index = 0; index < positions.size(); ++index) {
            EXPECT_EQ(positions[index], anchored[index + firstBeatNumber - 1]);
        }
    }
}

TEST(RekordboxBeatGridTest, RejectsInvalidBarPhaseOrTempo) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
    };

    EXPECT_EQ(positions, withDownbeatAnchor(positions, 5, 500.0));
    EXPECT_EQ(positions, withDownbeatAnchor(positions, 3, 0.0));
}

} // namespace
} // namespace mixxx::rekordbox
