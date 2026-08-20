#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "track/beats.h"
#include "waveform/renderers/waveformbeatgridsampler.h"

namespace mixxx::waveform {
namespace {

constexpr auto kSampleRate = audio::SampleRate(44100);

WaveformRgbViewportMetrics makeViewport(
        int physicalWidth, double trackWidth, double contentX) {
    return {
            physicalWidth,
            1000,
            0,
            1.0,
            trackWidth,
            contentX,
    };
}

TEST(WaveformBeatGridSamplerTest, UsesSharedTrackCoordinatesAndPixelSnapping) {
    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(120.0));
    const auto trackEnd = audio::FramePos(4 * kSampleRate);
    const auto beatGridData = buildWaveformBeatGridData(pBeats, trackEnd);

    auto lines = sampleWaveformBeatGridLines(
            beatGridData, trackEnd, makeViewport(400, 400.0, -100.24), 2.0);
    auto beatAtViewportStart =
            std::find_if(lines.begin(), lines.end(), [](const auto& line) {
                return line.x == 0.0f;
            });
    ASSERT_NE(lines.end(), beatAtViewportStart);
    EXPECT_FALSE(beatAtViewportStart->isDownbeat);

    lines = sampleWaveformBeatGridLines(
            beatGridData, trackEnd, makeViewport(400, 400.0, -100.26), 2.0);
    beatAtViewportStart =
            std::find_if(lines.begin(), lines.end(), [](const auto& line) {
                return line.x == -0.5f;
            });
    ASSERT_NE(lines.end(), beatAtViewportStart);
    EXPECT_FALSE(beatAtViewportStart->isDownbeat);
}

TEST(WaveformBeatGridSamplerTest, LongTracksStayBoundedAndBarAligned) {
    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(120.0));
    const auto trackEnd = audio::FramePos(7200 * kSampleRate);
    const auto beatGridData = buildWaveformBeatGridData(pBeats, trackEnd);

    for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
        const auto lines = sampleWaveformBeatGridLines(
                beatGridData,
                trackEnd,
                makeViewport(static_cast<int>(800 * dpr), 100.0, 0.0),
                dpr);
        ASSERT_FALSE(lines.empty());
        EXPECT_LE(lines.size(),
                static_cast<std::size_t>(kMaximumRenderedBeatGridLines));
        EXPECT_TRUE(std::all_of(lines.begin(), lines.end(), [](const auto& line) {
            return line.isDownbeat;
        }));
    }
}

TEST(WaveformBeatGridSamplerTest, ZoomOneThroughTenStaysBoundedAtEveryDpr) {
    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(128.0));
    const auto trackEnd = audio::FramePos(300 * kSampleRate);
    const auto beatGridData = buildWaveformBeatGridData(pBeats, trackEnd);

    for (int zoom = 1; zoom <= 10; ++zoom) {
        for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
            const auto viewport = calculateWaveformRgbViewportMetrics(
                    600000, 800.0, dpr, 0.5, 0.5, zoom);
            const auto lines = sampleWaveformBeatGridLines(
                    beatGridData, trackEnd, viewport, dpr);
            ASSERT_FALSE(lines.empty());
            EXPECT_LE(lines.size(),
                    static_cast<std::size_t>(kMaximumRenderedBeatGridLines));
            EXPECT_TRUE(std::all_of(
                    lines.begin(), lines.end(), [](const auto& line) {
                        return std::isfinite(line.x);
                    }));
        }
    }
}

TEST(WaveformBeatGridSamplerTest, DownbeatsOnlyOmitsOrdinaryBeatLines) {
    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(120.0));
    const auto trackEnd = audio::FramePos(60 * kSampleRate);
    const auto beatGridData = buildWaveformBeatGridData(pBeats, trackEnd);

    for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
        const auto lines = sampleWaveformBeatGridLines(beatGridData,
                trackEnd,
                makeViewport(static_cast<int>(800 * dpr), 800.0, 0.0),
                dpr,
                true);
        ASSERT_FALSE(lines.empty());
        EXPECT_TRUE(std::all_of(lines.begin(), lines.end(), [](const auto& line) {
            return line.isDownbeat;
        }));
    }
}

TEST(WaveformBeatGridSamplerTest,
        ExactDecodedLengthAlignsCueCentersAcrossZoomAndDpr) {
    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(120.0));
    const auto exactTrackEnd = audio::FramePos(300 * kSampleRate + 1234);
    const auto cuePosition = audio::FramePos(280 * kSampleRate);
    const auto beatGridData = buildWaveformBeatGridData(pBeats, exactTrackEnd);
    const double displayPosition = cuePosition.value() / exactTrackEnd.value();

    for (int zoom = 1; zoom <= 10; ++zoom) {
        for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
            SCOPED_TRACE(testing::Message()
                    << "zoom=" << zoom << " dpr=" << dpr);
            const auto viewport = calculateWaveformRgbViewportMetrics(
                    600000, 800.0, dpr, displayPosition, 0.5, zoom);
            const auto lines = sampleWaveformBeatGridLines(
                    beatGridData, exactTrackEnd, viewport, dpr);
            ASSERT_FALSE(lines.empty());

            const double cueCenter = viewport.contentX +
                    cuePosition.value() / exactTrackEnd.value() *
                            viewport.trackWidth;
            const auto closestLine = std::min_element(lines.begin(),
                    lines.end(),
                    [cueCenter](const auto& left, const auto& right) {
                        return std::abs(left.x - cueCenter) <
                                std::abs(right.x - cueCenter);
                    });
            ASSERT_NE(lines.end(), closestLine);
            EXPECT_LE(std::abs(closestLine->x - cueCenter),
                    0.5 / dpr + 0.0001);
        }
    }
}

TEST(WaveformBeatGridSamplerTest, GeometryIsBoundedByLinesAndPhysicalHeight) {
    std::vector<WaveformBeatGridLine> lines(kMaximumRenderedBeatGridLines,
            WaveformBeatGridLine{0.0f, false});
    constexpr double kLogicalHeight = 128.0;

    for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
        const std::size_t rectangleCount = waveformBeatGridRectangleCount(
                lines, kLogicalHeight, dpr);
        EXPECT_EQ(lines.size() * 2, rectangleCount);
    }
}

TEST(WaveformBeatGridSamplerTest,
        DownbeatsKeepDashesWhileOrdinaryBeatsUseEdgeMarkers) {
    const std::vector<WaveformBeatGridLine> lines{
            {0.0f, false}, {10.0f, true}, {20.0f, false}};
    constexpr double kLogicalHeight = 128.0;

    for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
        const std::size_t expected = 4U + static_cast<std::size_t>(
                waveformBeatGridDashCount(kLogicalHeight, dpr, true));
        EXPECT_EQ(expected,
                waveformBeatGridRectangleCount(lines, kLogicalHeight, dpr));
    }
}

TEST(WaveformBeatGridSamplerTest, InvalidInputsProduceNoGeometry) {
    const auto viewport = makeViewport(400, 400.0, 0.0);
    EXPECT_TRUE(sampleWaveformBeatGridLines(
                        {}, audio::FramePos(1000), viewport, 1.0)
                        .empty());

    const auto pBeats = Beats::fromConstTempo(
            kSampleRate, audio::kStartFramePos, Bpm(120.0));
    const auto beatGridData = buildWaveformBeatGridData(
            pBeats, audio::FramePos(1000));
    EXPECT_TRUE(sampleWaveformBeatGridLines(
                        beatGridData, audio::kStartFramePos, viewport, 1.0)
                        .empty());
}

} // namespace
} // namespace mixxx::waveform
