#include "track/cue.h"

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "mixer/basetrackplayer.h"
#include "qml/qmlcuesmodel.h"
#include "qml/qmlplayerproxy.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/color/predefinedcolorpalettes.h"

namespace mixxx {

namespace {

class FakeQmlTrackPlayer final : public BaseTrackPlayer {
  public:
    FakeQmlTrackPlayer()
            : BaseTrackPlayer(nullptr, QStringLiteral("[Channel1]")) {
    }

    TrackPointer getLoadedTrack() const override {
        return m_pTrack;
    }

    void setupEqControls() override {
    }

    void slotLoadTrack(TrackPointer pTrack, bool play) override {
        Q_UNUSED(play);
        const auto pOldTrack = m_pTrack;
        emit loadingTrack(pTrack, pOldTrack);
        m_pTrack = pTrack;
        emit newTrackLoaded(pTrack);
    }

    void slotCloneFromGroup(const QString& group) override {
        Q_UNUSED(group);
    }

    void slotCloneDeck() override {
    }

    void slotEjectTrack(double value) override {
        if (value <= 0) {
            return;
        }
        const auto pOldTrack = m_pTrack;
        emit loadingTrack(nullptr, pOldTrack);
        m_pTrack.reset();
        emit playerEmpty();
    }

  private:
    TrackPointer m_pTrack;
};

qml::QmlCuesModel* cueModelProperty(
        qml::QmlPlayerProxy* pPlayer, const char* propertyName) {
    const QVariant property = pPlayer->property(propertyName);
    if (!property.canConvert<QObject*>()) {
        return nullptr;
    }
    return qobject_cast<qml::QmlCuesModel*>(property.value<QObject*>());
}

} // namespace

TEST(CueTest, NewCueIsDirty) {
    const auto cue = Cue(
            mixxx::CueType::HotCue,
            1,
            mixxx::audio::kStartFramePos,
            mixxx::audio::kInvalidFramePos,
            mixxx::PredefinedColorPalettes::kDefaultCueColor);
    EXPECT_TRUE(cue.isDirty());
}

TEST(CueTest, DefaultCueInfoToCueRoundtrip) {
    const CueInfo cueInfo1;
    const Cue cueObject(
            cueInfo1,
            audio::SampleRate(44100),
            true);
    auto cueInfo2 = cueObject.getCueInfo(
            audio::SampleRate(44100));
    cueInfo2.setColor(std::nullopt);
    EXPECT_EQ(cueInfo1, cueInfo2);
}

TEST(CueTest, ConvertCueInfoToCueRoundtrip) {
    // Due to rounding errors this test may fail if the
    // cue position/sample conversions don't always result
    // in integer numbers.
    const auto cueInfo1 = CueInfo(
            CueType::HotCue,
            std::make_optional(1.0 * 44100 * mixxx::kEngineChannelCount),
            std::nullopt,
            std::make_optional(3),
            QStringLiteral("label"),
            RgbColor::optional(0xABCDEF));
    const Cue cueObject(
            cueInfo1,
            audio::SampleRate(44100),
            true);
    EXPECT_TRUE(cueObject.isDirty());
    const auto cueInfo2 = cueObject.getCueInfo(
            audio::SampleRate(44100));
    EXPECT_EQ(cueInfo1, cueInfo2);
}

TEST(CueTest, MemoryCueIsDisplayOnlyMarkerWithoutHotcueSlot) {
    const Cue cue(
            CueType::MemoryCue,
            Cue::kNoHotCue,
            audio::FramePos(1234),
            audio::kInvalidFramePos,
            PredefinedColorPalettes::kDefaultCueColor);

    EXPECT_EQ(CueType::MemoryCue, cue.getType());
    EXPECT_EQ(Cue::kNoHotCue, cue.getHotCue());
    EXPECT_FALSE(cue.getEndPosition().isValid());
}

TEST(CueTest, QmlCueModelExposesColorAndTransparentFallback) {
    qml::QmlCuesModel model;
    CuePointer coloredCue(new Cue(
            CueType::HotCue,
            0,
            audio::FramePos(1234),
            audio::kInvalidFramePos,
            RgbColor(0xF87090)));
    CuePointer uncoloredCue(new Cue(
            CueType::MemoryCue,
            Cue::kNoHotCue,
            audio::FramePos(5678),
            audio::kInvalidFramePos,
            PredefinedColorPalettes::kDefaultCueColor));
    model.setCues({coloredCue, uncoloredCue});

    EXPECT_EQ(QByteArray("color"),
            model.roleNames().value(qml::QmlCuesModel::ColorRole));
    EXPECT_EQ(QColor(0xF8, 0x70, 0x90),
            model.data(model.index(0, 0), qml::QmlCuesModel::ColorRole)
                    .value<QColor>());
    EXPECT_EQ(QColor(0, 0, 0, 0),
            model.data(model.index(1, 0), qml::QmlCuesModel::ColorRole)
                    .value<QColor>());
}

TEST(CueTest, QmlPlayerProxyRefreshesSeparateMemoryCueModelOnTrackLoad) {
    FakeQmlTrackPlayer trackPlayer;
    qml::QmlPlayerProxy playerProxy(&trackPlayer);
    TrackPointer pTrack = Track::newTemporary();

    pTrack->createAndAddCue(CueType::HotCue,
            0,
            audio::FramePos(1000),
            audio::kInvalidFramePos,
            RgbColor(0x112233));
    pTrack->createAndAddCue(CueType::Loop,
            1,
            audio::FramePos(2000),
            audio::FramePos(3000),
            RgbColor(0x445566));
    pTrack->createAndAddCue(CueType::MemoryCue,
            Cue::kNoHotCue,
            audio::FramePos(1000),
            audio::kInvalidFramePos,
            RgbColor(0xF87090));

    trackPlayer.slotLoadTrack(pTrack, false);

    auto* pHotcuesModel = cueModelProperty(&playerProxy, "hotcuesModel");
    auto* pMemoryCuesModel = cueModelProperty(&playerProxy, "memoryCuesModel");
    ASSERT_NE(nullptr, pHotcuesModel);
    ASSERT_NE(nullptr, pMemoryCuesModel);
    EXPECT_EQ(2, pHotcuesModel->rowCount({}));
    ASSERT_EQ(1, pMemoryCuesModel->rowCount({}));
    EXPECT_EQ(1000,
            pMemoryCuesModel
                    ->data(pMemoryCuesModel->index(0, 0),
                            qml::QmlCuesModel::StartPositionRole)
                    .toInt());
    EXPECT_EQ(QColor(0xF8, 0x70, 0x90),
            pMemoryCuesModel
                    ->data(pMemoryCuesModel->index(0, 0),
                            qml::QmlCuesModel::ColorRole)
                    .value<QColor>());

    pTrack->createAndAddCue(CueType::MemoryCue,
            Cue::kNoHotCue,
            audio::FramePos(500),
            audio::kInvalidFramePos);

    ASSERT_EQ(2, pMemoryCuesModel->rowCount({}));
    EXPECT_EQ(500,
            pMemoryCuesModel
                    ->data(pMemoryCuesModel->index(0, 0),
                            qml::QmlCuesModel::StartPositionRole)
                    .toInt());
    EXPECT_EQ(2, pHotcuesModel->rowCount({}));
}
} // namespace mixxx
