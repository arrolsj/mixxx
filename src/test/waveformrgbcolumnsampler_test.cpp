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

TEST(WaveformRgbColumnSamplerTest,
        FiveLevelSamplerZoomHalvesTrackWidthAtEveryStep) {
    constexpr int kVisualFrames = 32000;
    constexpr double kLogicalWidth = 800.0;

    double previousTrackWidth = 0.0;
    for (const double zoom : std::array<double, 5>{2.0, 4.0, 8.0, 16.0, 32.0}) {
        const auto metrics = calculateWaveformRgbViewportMetrics(
                kVisualFrames * 2, kLogicalWidth, 1.0, 0.5, 0.5, zoom);
        ASSERT_TRUE(metrics.isValid());
        if (previousTrackWidth > 0.0) {
            EXPECT_DOUBLE_EQ(previousTrackWidth / 2.0, metrics.trackWidth);
        }
        previousTrackWidth = metrics.trackWidth;
    }
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

} // namespace
} // namespace mixxx::waveform
