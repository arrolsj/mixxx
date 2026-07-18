#include <gtest/gtest.h>

#include "audio/types.h"
#include "qml/qmlbeatsmodel.h"
#include "test/mixxxtest.h"
#include "track/beats.h"

namespace {

class QmlBeatsModelTest : public MixxxTest {
};

TEST_F(QmlBeatsModelTest, ExposesFourBeatPhaseRelativeToPersistedGridAnchor) {
    constexpr auto sampleRate = mixxx::audio::SampleRate(1000);
    const auto pBeats = mixxx::Beats::fromConstTempo(
            sampleRate,
            mixxx::audio::FramePos(2000),
            mixxx::Bpm(60));

    mixxx::qml::QmlBeatsModel model;
    model.setBeats(pBeats, mixxx::audio::FramePos(8500));

    ASSERT_GE(model.rowCount({}), 7);
    EXPECT_EQ(model.roleNames().value(mixxx::qml::QmlBeatsModel::FramePositionRole),
            QByteArray("framePosition"));
    EXPECT_EQ(model.roleNames().value(mixxx::qml::QmlBeatsModel::IsDownbeatRole),
            QByteArray("isDownbeat"));

    // The first visible beat is two beats before the persisted anchor, so the
    // model must not infer bar phase from row zero.
    EXPECT_FALSE(model.data(model.index(0, 0),
                              mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                         .toBool());
    EXPECT_TRUE(model.data(model.index(2, 0),
                             mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                        .toBool());
    EXPECT_FALSE(model.data(model.index(3, 0),
                              mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                         .toBool());
    EXPECT_TRUE(model.data(model.index(6, 0),
                             mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                        .toBool());
}

} // namespace
