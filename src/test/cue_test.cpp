#include "track/cue.h"

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "qml/qmlcuesmodel.h"
#include "test/mixxxtest.h"
#include "util/color/color.h"
#include "util/color/predefinedcolorpalettes.h"

namespace mixxx {

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
} // namespace mixxx
