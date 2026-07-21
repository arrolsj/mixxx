#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "waveform/renderers/waveformrgbcolumnsampler.h"

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
        RekordboxThreeBandUsesPeakPreservingRmsForBothSides) {
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
    EXPECT_FLOAT_EQ(
            peakPreservingRms(10.0 * 10.0 + 90.0 * 90.0, 90.0f, 2),
            columns[0].leftLow);
    EXPECT_FLOAT_EQ(columns[0].leftLow, columns[0].rightLow);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(20.0 * 20.0 + 70.0 * 70.0, 70.0f, 2),
            columns[0].leftMid);
    EXPECT_FLOAT_EQ(columns[0].leftMid, columns[0].rightMid);
    EXPECT_FLOAT_EQ(
            peakPreservingRms(30.0 * 30.0 + 50.0 * 50.0, 50.0f, 2),
            columns[0].leftHigh);
    EXPECT_FLOAT_EQ(columns[0].leftHigh, columns[0].rightHigh);
    EXPECT_LT(columns[0].leftLow, 90.0f);
    EXPECT_GT(columns[0].leftLow, 65.0f);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxReferenceUsesWholeTrackRobustPercentile) {
    std::vector<RekordboxThreeBandColumn> source(100);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index].mid = index < 80 ? 20 : (index < 99 ? 100 : 255);
    }

    EXPECT_FLOAT_EQ(100.0f, calculateRekordboxThreeBandReference(source));

    const std::vector<RekordboxThreeBandColumn> quiet{
            {.mid = 20, .high = 10, .low = 5},
    };
    EXPECT_FLOAT_EQ(kMinimumRekordboxThreeBandReference,
            calculateRekordboxThreeBandReference(quiet));
}

TEST(WaveformRgbColumnSamplerTest,
        ThreeBandTransferSuppressesOrdinaryEnergyAndKeepsPeakHeadroom) {
    const float ordinary = threeBandDisplayAmplitude(50.0f, 100.0f);
    const float robustPeak = threeBandDisplayAmplitude(100.0f, 100.0f);
    const float transient = threeBandDisplayAmplitude(200.0f, 100.0f);

    EXPECT_FLOAT_EQ(0.0f, threeBandDisplayAmplitude(0.0f, 100.0f));
    EXPECT_LT(ordinary, robustPeak);
    EXPECT_LT(robustPeak, transient);
    EXPECT_LT(transient, kThreeBandDisplayHeightScale);
    EXPECT_NEAR(0.606f, robustPeak, 0.001f);
}

} // namespace
} // namespace mixxx::waveform
