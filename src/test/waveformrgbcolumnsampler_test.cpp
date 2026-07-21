#include "waveform/renderers/waveformrgbcolumnsampler.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

namespace mixxx::waveform {
namespace {

std::vector<WaveformData> makeWaveformData(int visualFrames) {
    return std::vector<WaveformData>(
            static_cast<std::size_t>(visualFrames * 2), WaveformData(0));
}

void fillDeterministic(std::vector<WaveformData>* pData) {
    for (std::size_t index = 0; index < pData->size(); ++index) {
        WaveformData& value = (*pData)[index];
        value.filtered.low = static_cast<unsigned char>((index * 17 + 11) % 251 + 1);
        value.filtered.mid = static_cast<unsigned char>((index * 29 + 7) % 251 + 1);
        value.filtered.high = static_cast<unsigned char>((index * 43 + 3) % 251 + 1);
        value.filtered.all = static_cast<unsigned char>((index * 61 + 5) % 251 + 1);
    }
}

void expectColumnEqual(
        const WaveformRgbColumn& expected, const WaveformRgbColumn& actual) {
    EXPECT_FLOAT_EQ(expected.leftAmplitude, actual.leftAmplitude);
    EXPECT_FLOAT_EQ(expected.rightAmplitude, actual.rightAmplitude);
    EXPECT_FLOAT_EQ(expected.red, actual.red);
    EXPECT_FLOAT_EQ(expected.green, actual.green);
    EXPECT_FLOAT_EQ(expected.blue, actual.blue);
}

const WaveformRgbColorF kLowColor{1.0f, 0.0f, 0.0f};
const WaveformRgbColorF kMidColor{0.0f, 1.0f, 0.0f};
const WaveformRgbColorF kHighColor{0.0f, 0.0f, 1.0f};

TEST(WaveformRgbColumnSamplerTest,
        OnePhysicalTrackPixelIsPureTranslationAtEveryZoomAndDpr) {
    constexpr int kVisualFrames = 512;
    constexpr double kLogicalWidth = 32.0;
    auto data = makeWaveformData(kVisualFrames);
    fillDeterministic(&data);

    for (const double zoom : std::array<double, 3>{1.0, 6.0, 10.0}) {
        for (const double dpr : std::array<double, 2>{1.0, 2.0}) {
            const double trackPhysicalPixels = kVisualFrames * dpr / zoom;
            const auto beforeMetrics = calculateWaveformRgbViewportMetrics(
                    static_cast<int>(data.size()),
                    kLogicalWidth,
                    dpr,
                    0.5,
                    0.5,
                    zoom);
            const auto afterMetrics = calculateWaveformRgbViewportMetrics(
                    static_cast<int>(data.size()),
                    kLogicalWidth,
                    dpr,
                    0.5 + 1.0 / trackPhysicalPixels,
                    0.5,
                    zoom);
            ASSERT_EQ(beforeMetrics.firstGlobalPixel + 1,
                    afterMetrics.firstGlobalPixel);

            const auto before = sampleWaveformRgbColumns(data.data(),
                    static_cast<int>(data.size()),
                    static_cast<int>(data.size()),
                    beforeMetrics,
                    kLowColor,
                    kMidColor,
                    kHighColor);
            const auto after = sampleWaveformRgbColumns(data.data(),
                    static_cast<int>(data.size()),
                    static_cast<int>(data.size()),
                    afterMetrics,
                    kLowColor,
                    kMidColor,
                    kHighColor);
            ASSERT_EQ(before.size(), after.size());
            for (std::size_t index = 0; index + 1 < before.size(); ++index) {
                expectColumnEqual(before[index + 1], after[index]);
            }
        }
    }
}

TEST(WaveformRgbColumnSamplerTest,
        MotionInsidePhysicalPixelDoesNotChangeEnvelopeOrColor) {
    constexpr int kVisualFrames = 512;
    constexpr double kZoom = 6.0;
    constexpr double kDpr = 1.0;
    constexpr double kLogicalWidth = 24.0;
    auto data = makeWaveformData(kVisualFrames);
    fillDeterministic(&data);
    const double trackPhysicalPixels = kVisualFrames * kDpr / kZoom;

    const auto baseMetrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()),
            kLogicalWidth,
            kDpr,
            40.0 / trackPhysicalPixels,
            0.5,
            kZoom);
    const auto insideMetrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()),
            kLogicalWidth,
            kDpr,
            40.49 / trackPhysicalPixels,
            0.5,
            kZoom);
    const auto crossedMetrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()),
            kLogicalWidth,
            kDpr,
            40.51 / trackPhysicalPixels,
            0.5,
            kZoom);
    EXPECT_EQ(baseMetrics.firstGlobalPixel, insideMetrics.firstGlobalPixel);
    EXPECT_EQ(baseMetrics.firstGlobalPixel + 1, crossedMetrics.firstGlobalPixel);

    const auto base = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            baseMetrics,
            kLowColor,
            kMidColor,
            kHighColor);
    const auto inside = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            insideMetrics,
            kLowColor,
            kMidColor,
            kHighColor);
    const auto crossed = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            crossedMetrics,
            kLowColor,
            kMidColor,
            kHighColor);
    ASSERT_EQ(base.size(), inside.size());
    ASSERT_EQ(base.size(), crossed.size());
    for (std::size_t index = 0; index < base.size(); ++index) {
        expectColumnEqual(base[index], inside[index]);
    }
    for (std::size_t index = 0; index + 1 < base.size(); ++index) {
        expectColumnEqual(base[index + 1], crossed[index]);
    }
}

TEST(WaveformRgbColumnSamplerTest, AggregatesSeparatedBandAndChannelPeaks) {
    constexpr int kVisualFrames = 32;
    auto data = makeWaveformData(kVisualFrames);
    data[4 * 2].filtered.low = 40;
    data[5 * 2 + 1].filtered.mid = 80;
    data[10 * 2].filtered.high = 120;
    data[6 * 2].filtered.all = 50;
    data[9 * 2 + 1].filtered.all = 100;

    const auto metrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 1.0, 1.0, 0.25, 0.0, 8.0);
    const auto columns = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            metrics,
            kLowColor,
            kMidColor,
            kHighColor);
    ASSERT_EQ(1U, columns.size());
    EXPECT_FLOAT_EQ(50.0f, columns[0].leftAmplitude);
    EXPECT_FLOAT_EQ(100.0f, columns[0].rightAmplitude);
    EXPECT_NEAR(1.0f / 3.0f, columns[0].red, 1e-6f);
    EXPECT_NEAR(2.0f / 3.0f, columns[0].green, 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, columns[0].blue);
}

TEST(WaveformRgbColumnSamplerTest, OutputIsBoundedByPhysicalViewport) {
    auto data = makeWaveformData(50000);
    fillDeterministic(&data);
    const auto metrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 17.25, 2.0, 0.5, 0.5, 7.0);
    const auto columns = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            metrics,
            kLowColor,
            kMidColor,
            kHighColor);
    EXPECT_EQ(35, metrics.physicalWidth);
    EXPECT_EQ(static_cast<std::size_t>(metrics.physicalWidth), columns.size());
}

TEST(WaveformRgbColumnSamplerTest, NeverReadsBeyondCompletedStereoPrefix) {
    constexpr int kVisualFrames = 64;
    auto data = makeWaveformData(kVisualFrames);
    for (WaveformData& value : data) {
        value.filtered.low = 200;
        value.filtered.all = 200;
    }
    constexpr int kCompletedDataElements = 16;

    const auto completedMetrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 1.0, 1.0, 0.0, 0.0, 4.0);
    const auto incompleteMetrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 1.0, 1.0, 0.75, 0.0, 4.0);
    const auto completed = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            kCompletedDataElements,
            completedMetrics,
            kLowColor,
            kMidColor,
            kHighColor);
    const auto incomplete = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            kCompletedDataElements,
            incompleteMetrics,
            kLowColor,
            kMidColor,
            kHighColor);
    ASSERT_EQ(1U, completed.size());
    ASSERT_EQ(1U, incomplete.size());
    EXPECT_FLOAT_EQ(200.0f, completed[0].leftAmplitude);
    EXPECT_FLOAT_EQ(0.0f, incomplete[0].leftAmplitude);
    EXPECT_FLOAT_EQ(0.0f, incomplete[0].rightAmplitude);
}
TEST(WaveformRgbColumnSamplerTest,
        ThreeBandKeepsIndependentStereoBandsWithRobustPeaks) {
    constexpr int kVisualFrames = 8;
    constexpr std::size_t kSampleCount = 3;
    auto data = makeWaveformData(kVisualFrames);
    data[0].filtered.low = 40;
    data[1].filtered.low = 80;
    data[2].filtered.mid = 120;
    data[3].filtered.mid = 60;
    data[4].filtered.high = 30;
    data[5].filtered.high = 150;

    WaveformRgbViewportMetrics viewport;
    viewport.physicalWidth = 1;
    viewport.visualFrameCount = kVisualFrames;
    viewport.firstGlobalPixel = 0;
    viewport.visualFramesPerPhysicalPixel = 6.0;
    const auto columns = sampleWaveformThreeBandColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            viewport);

    ASSERT_EQ(1U, columns.size());
    EXPECT_FLOAT_EQ(
            peakPreservingRms(40.0 * 40.0, 40.0f, kSampleCount),
            columns[0].leftLow);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(80.0 * 80.0, 80.0f, kSampleCount),
            columns[0].rightLow);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(120.0 * 120.0, 120.0f, kSampleCount),
            columns[0].leftMid);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(60.0 * 60.0, 60.0f, kSampleCount),
            columns[0].rightMid);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(30.0 * 30.0, 30.0f, kSampleCount),
            columns[0].leftHigh);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(150.0 * 150.0, 150.0f, kSampleCount),
            columns[0].rightHigh);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxThreeBandUsesSourceMeanForBothSides) {
    const std::vector<RekordboxThreeBandColumn> source{
            {.mid = 20, .high = 30, .low = 10},
            {.mid = 70, .high = 50, .low = 90},
            {.mid = 40, .high = 110, .low = 60},
            {.mid = 10, .high = 20, .low = 30},
    };
    WaveformRgbViewportMetrics viewport;
    viewport.physicalWidth = 1;
    viewport.visualFrameCount = 4;
    viewport.firstGlobalPixel = 0;
    viewport.visualFramesPerPhysicalPixel = 4.0;

    const auto columns = sampleRekordboxThreeBandColumns(source, viewport);

    ASSERT_EQ(1U, columns.size());
    EXPECT_FLOAT_EQ(50.0f, columns[0].leftLow);
    EXPECT_FLOAT_EQ(columns[0].leftLow, columns[0].rightLow);
    EXPECT_FLOAT_EQ(45.0f, columns[0].leftMid);
    EXPECT_FLOAT_EQ(columns[0].leftMid, columns[0].rightMid);
    EXPECT_FLOAT_EQ(40.0f, columns[0].leftHigh);
    EXPECT_FLOAT_EQ(columns[0].leftHigh, columns[0].rightHigh);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxSourceMeanKeepsImpulseInOnePhysicalColumn) {
    std::vector<RekordboxThreeBandColumn> source(8);
    source[3] = {.mid = 120, .high = 60, .low = 200};

    WaveformRgbViewportMetrics viewport;
    viewport.physicalWidth = 4;
    viewport.visualFrameCount = 8;
    viewport.firstGlobalPixel = 0;
    viewport.visualFramesPerPhysicalPixel = 2.0;

    const auto columns = sampleRekordboxThreeBandColumns(source, viewport);

    ASSERT_EQ(4U, columns.size());
    EXPECT_FLOAT_EQ(0.0f, columns[1].leftLow);
    EXPECT_FLOAT_EQ(100.0f, columns[2].leftLow);
    EXPECT_FLOAT_EQ(60.0f, columns[2].leftMid);
    EXPECT_FLOAT_EQ(30.0f, columns[2].leftHigh);
    EXPECT_FLOAT_EQ(0.0f, columns[3].leftLow);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxReferenceUsesWholeTrackRobustPercentile) {
    std::vector<RekordboxThreeBandColumn> source(100);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index].mid = index < 80 ? 20 : (index < 99 ? 100 : 255);
        source[index].high = 255;
        source[index].low = 200;
    }

    const WaveformThreeBandReference reference =
            calculateRekordboxThreeBandReference(source);
    EXPECT_FLOAT_EQ(200.0f, reference.low);
    EXPECT_FLOAT_EQ(100.0f, reference.mid);
    EXPECT_FLOAT_EQ(255.0f, reference.high);

    const std::vector<RekordboxThreeBandColumn> quiet{
            {.mid = 20, .high = 10, .low = 5},
    };
    const WaveformThreeBandReference quietReference =
            calculateRekordboxThreeBandReference(quiet);
    EXPECT_FLOAT_EQ(kMinimumRekordboxThreeBandReference, quietReference.low);
    EXPECT_FLOAT_EQ(kMinimumRekordboxThreeBandReference, quietReference.mid);
    EXPECT_FLOAT_EQ(kMinimumRekordboxThreeBandReference, quietReference.high);
}

TEST(WaveformRgbColumnSamplerTest,
        ThreeBandTransferKeepsProfilesAndBandsIndependent) {
    constexpr auto detail = WaveformThreeBandDisplayProfile::RekordboxDetail;
    constexpr auto preview = WaveformThreeBandDisplayProfile::RekordboxPreview;
    constexpr auto fallback = WaveformThreeBandDisplayProfile::MixxxFallback;
    constexpr WaveformThreeBandReference reference{
            .low = 100.0f,
            .mid = 100.0f,
            .high = 100.0f,
    };

    EXPECT_FLOAT_EQ(0.0f,
            threeBandDisplayAmplitude(
                    0.0f, reference, WaveformThreeBandBand::Low, detail));
    EXPECT_NEAR(0.435275f,
            threeBandDisplayAmplitude(
                    50.0f, reference, WaveformThreeBandBand::Low, detail),
            1e-6f);
    EXPECT_NEAR(0.208932f,
            threeBandDisplayAmplitude(
                    50.0f, reference, WaveformThreeBandBand::Mid, detail),
            1e-6f);
    EXPECT_NEAR(0.104466f,
            threeBandDisplayAmplitude(
                    50.0f, reference, WaveformThreeBandBand::High, detail),
            1e-6f);
    EXPECT_FLOAT_EQ(0.245f,
            threeBandDisplayAmplitude(
                    50.0f, reference, WaveformThreeBandBand::Low, preview));
    EXPECT_FLOAT_EQ(0.09f,
            threeBandDisplayAmplitude(
                    50.0f, reference, WaveformThreeBandBand::High, fallback));
    EXPECT_FLOAT_EQ(kThreeBandDisplayHeightScale,
            threeBandDisplayAmplitude(255.0f,
                    {.low = 1.0f, .mid = 1.0f, .high = 1.0f},
                    WaveformThreeBandBand::Low,
                    detail));

    const WaveformThreeBandReference representative{
            .low = 60.0f,
            .mid = 110.0f,
            .high = 110.0f,
    };
    const float low = threeBandDisplayAmplitude(
            5.0f, representative, WaveformThreeBandBand::Low, detail);
    const float mid = threeBandDisplayAmplitude(
            40.0f, representative, WaveformThreeBandBand::Mid, detail);
    const float high = threeBandDisplayAmplitude(
            25.0f, representative, WaveformThreeBandBand::High, detail);
    EXPECT_GT(low, mid);
    EXPECT_GT(mid, high);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxPreviewStackUsesCumulativeBandBoundaries) {
    const RekordboxThreeBandColumn source{
            .mid = 50,
            .high = 40,
            .low = 20,
    };
    const auto stack = calculateRekordboxThreeBandPreviewStack(source);

    EXPECT_NEAR(20.0f / 255.0f * 0.49f, stack.low, 1e-6f);
    EXPECT_NEAR(stack.low + 50.0f / 255.0f * 0.32f, stack.mid, 1e-6f);
    EXPECT_NEAR(stack.mid + 40.0f / 255.0f * 0.25f, stack.high, 1e-6f);
    EXPECT_LT(stack.low, stack.mid);
    EXPECT_LT(stack.mid, stack.high);
}

TEST(WaveformRgbColumnSamplerTest,
        ThreeBandEnvelopeConnectsAdjacentColumnsWithOneSegmentOverhead) {
    const std::vector<WaveformThreeBandColumn> columns{
            {},
            {.leftLow = 100.0f, .rightLow = 50.0f},
            {},
    };
    const auto edges = buildWaveformThreeBandEnvelopeEdges(columns,
            100.0f,
            1.0,
            {.low = 100.0f, .mid = 100.0f, .high = 100.0f},
            WaveformThreeBandDisplayProfile::RekordboxDetail,
            1.0f,
            1.0f,
            1.0f);

    ASSERT_EQ(columns.size() + 2, edges.size());
    EXPECT_EQ(columns.size() + 1, edges.size() - 1);
    EXPECT_FLOAT_EQ(-0.5f, edges[0].x);
    EXPECT_FLOAT_EQ(0.0f, edges[1].x);
    EXPECT_FLOAT_EQ(1.0f, edges[2].x);
    EXPECT_FLOAT_EQ(2.0f, edges[3].x);
    EXPECT_FLOAT_EQ(2.5f, edges[4].x);
    EXPECT_FLOAT_EQ(100.0f, edges[1].low.top);
    EXPECT_FLOAT_EQ(50.0f, edges[2].low.top);
    EXPECT_NEAR(143.5275f, edges[2].low.bottom, 1e-4f);
    EXPECT_FLOAT_EQ(100.0f, edges[3].low.top);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxPreviewEnvelopeUsesCumulativeBoundaries) {
    const std::vector<WaveformThreeBandColumn> columns{
            {.leftLow = 20.0f,
                    .rightLow = 20.0f,
                    .leftMid = 50.0f,
                    .rightMid = 50.0f,
                    .leftHigh = 40.0f,
                    .rightHigh = 40.0f},
    };
    const auto stack = calculateRekordboxThreeBandPreviewStack(
            {.mid = 50, .high = 40, .low = 20});
    const auto edges = buildWaveformThreeBandEnvelopeEdges(columns,
            100.0f,
            1.0,
            {.low = 32.0f, .mid = 32.0f, .high = 32.0f},
            WaveformThreeBandDisplayProfile::RekordboxPreview,
            1.0f,
            1.0f,
            1.0f);

    ASSERT_EQ(2U, edges.size());
    EXPECT_NEAR(100.0f - stack.low * 100.0f, edges[0].low.top, 1e-5f);
    EXPECT_NEAR(100.0f - stack.mid * 100.0f, edges[0].mid.top, 1e-5f);
    EXPECT_NEAR(100.0f - stack.high * 100.0f, edges[0].high.top, 1e-5f);
    EXPECT_LT(edges[0].high.top, edges[0].mid.top);
    EXPECT_LT(edges[0].mid.top, edges[0].low.top);
}

TEST(WaveformRgbColumnSamplerTest,
        ThreeBandEnvelopeHandlesSingleColumnDprAndExtremeGain) {
    const std::vector<WaveformThreeBandColumn> columns{
            {.leftLow = 255.0f, .rightLow = 255.0f},
    };
    const auto edges = buildWaveformThreeBandEnvelopeEdges(columns,
            50.0f,
            2.0,
            {.low = 64.0f, .mid = 64.0f, .high = 64.0f},
            WaveformThreeBandDisplayProfile::RekordboxDetail,
            10.0f,
            1.0f,
            1.0f);

    ASSERT_EQ(2U, edges.size());
    EXPECT_FLOAT_EQ(-0.25f, edges[0].x);
    EXPECT_FLOAT_EQ(0.25f, edges[1].x);
    for (const auto& edge : edges) {
        EXPECT_FLOAT_EQ(2.5f, edge.low.top);
        EXPECT_FLOAT_EQ(97.5f, edge.low.bottom);
    }
}

TEST(WaveformRgbColumnSamplerTest,
        ThreeBandSamplerNeverReadsBeyondCompletedStereoPrefix) {
    auto data = makeWaveformData(4);
    for (WaveformData& value : data) {
        value.filtered.low = 200;
        value.filtered.mid = 100;
        value.filtered.high = 50;
    }
    WaveformRgbViewportMetrics viewport;
    viewport.physicalWidth = 2;
    viewport.visualFrameCount = 4;
    viewport.firstGlobalPixel = 0;
    viewport.visualFramesPerPhysicalPixel = 1.0;

    const auto columns = sampleWaveformThreeBandColumns(
            data.data(), static_cast<int>(data.size()), 2, viewport);

    ASSERT_EQ(2U, columns.size());
    EXPECT_GT(columns[0].leftLow, 0.0f);
    EXPECT_GT(columns[0].rightHigh, 0.0f);
    EXPECT_FLOAT_EQ(0.0f, columns[1].leftLow);
    EXPECT_FLOAT_EQ(0.0f, columns[1].rightMid);
    EXPECT_FLOAT_EQ(0.0f, columns[1].rightHigh);
}

} // namespace
} // namespace mixxx::waveform
