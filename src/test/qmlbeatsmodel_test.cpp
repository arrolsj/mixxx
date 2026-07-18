#include <gtest/gtest.h>

#include "audio/types.h"
#include "library/rekordbox/rekordboxbeatgrid.h"
#include "qml/qmlbeatsmodel.h"
#include "test/mixxxtest.h"
#include "track/beats.h"

namespace {

class QmlBeatsModelTest : public MixxxTest {
};

TEST_F(QmlBeatsModelTest, ExposesFourBeatPhaseRelativeToPersistedGridAnchor) {
    constexpr auto sampleRate = mixxx::audio::SampleRate(8000);
    const auto pBeats = mixxx::Beats::fromConstTempo(
            sampleRate,
            mixxx::audio::FramePos(16000),
            mixxx::Bpm(60));

    mixxx::qml::QmlBeatsModel model;
    model.setBeats(pBeats, mixxx::audio::FramePos(68000));

    ASSERT_GE(model.rowCount({}), 7);
    EXPECT_EQ(model.roleNames().value(mixxx::qml::QmlBeatsModel::FramePositionRole),
            QByteArray("framePosition"));
    EXPECT_EQ(model.roleNames().value(mixxx::qml::QmlBeatsModel::IsDownbeatRole),
            QByteArray("isDownbeat"));
    EXPECT_EQ(model.roleNames().value(mixxx::qml::QmlBeatsModel::BeatNumberRole),
            QByteArray("beatNumber"));

    // The first visible beat is two beats before the persisted anchor, so the
    // model must not infer bar phase from row zero.
    EXPECT_EQ(3, model.data(model.index(0, 0),
                         mixxx::qml::QmlBeatsModel::BeatNumberRole)
                         .toInt());
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

TEST_F(QmlBeatsModelTest, ExposesRekordboxBeatNumberToQml) {
    constexpr auto sampleRate = mixxx::audio::SampleRate(8000);
    const QVector<mixxx::audio::FramePos> exportedPositions{
            mixxx::audio::FramePos(8000),
            mixxx::audio::FramePos(12000),
            mixxx::audio::FramePos(16000),
            mixxx::audio::FramePos(20000),
            mixxx::audio::FramePos(24000),
    };
    const auto anchoredPositions = mixxx::rekordbox::withDownbeatAnchor(
            exportedPositions, 3, 4000.0);
    const auto pBeats = mixxx::Beats::fromBeatPositions(
            sampleRate, anchoredPositions, QStringLiteral("Rekordbox USB drive"));

    mixxx::qml::QmlBeatsModel model;
    model.setBeats(pBeats, mixxx::audio::FramePos(25600));

    ASSERT_GE(model.rowCount({}), 7);
    EXPECT_DOUBLE_EQ(8000.0,
            model.data(model.index(2, 0),
                         mixxx::qml::QmlBeatsModel::FramePositionRole)
                    .toDouble());
    EXPECT_EQ(3,
            model.data(model.index(2, 0),
                         mixxx::qml::QmlBeatsModel::BeatNumberRole)
                    .toInt());
    EXPECT_FALSE(model.data(model.index(2, 0),
                              mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                         .toBool());
    EXPECT_EQ(1,
            model.data(model.index(4, 0),
                         mixxx::qml::QmlBeatsModel::BeatNumberRole)
                    .toInt());
    EXPECT_TRUE(model.data(model.index(4, 0),
                             mixxx::qml::QmlBeatsModel::IsDownbeatRole)
                        .toBool());
}

} // namespace
