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
    EXPECT_FLOAT_EQ(expected.lowAmplitude, actual.lowAmplitude);
    EXPECT_FLOAT_EQ(expected.midAmplitude, actual.midAmplitude);
    EXPECT_FLOAT_EQ(expected.highAmplitude, actual.highAmplitude);
    EXPECT_FLOAT_EQ(expected.unionAmplitude, actual.unionAmplitude);
}

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
                    beforeMetrics);
            const auto after = sampleWaveformRgbColumns(data.data(),
                    static_cast<int>(data.size()),
                    static_cast<int>(data.size()),
                    afterMetrics);
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
            baseMetrics);
    const auto inside = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            insideMetrics);
    const auto crossed = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            crossedMetrics);
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
            metrics);
    ASSERT_EQ(1U, columns.size());
    EXPECT_FLOAT_EQ(50.0f, columns[0].leftAmplitude);
    EXPECT_FLOAT_EQ(100.0f, columns[0].rightAmplitude);
    EXPECT_FLOAT_EQ(40.0f, columns[0].lowAmplitude);
    EXPECT_FLOAT_EQ(80.0f, columns[0].midAmplitude);
    EXPECT_FLOAT_EQ(120.0f, columns[0].highAmplitude);
    EXPECT_FLOAT_EQ(100.0f, columns[0].unionAmplitude);
}

TEST(WaveformRgbColumnSamplerTest, PreservesOriginalRgbColorMixing) {
    const WaveformRgbColumn column{
            50.0f, 100.0f, 40.0f, 80.0f, 120.0f, 100.0f};
    const WaveformRgbColorF color = mixWaveformRgbColor(column,
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f});
    EXPECT_NEAR(1.0f / 3.0f, color.red, 1e-6f);
    EXPECT_NEAR(2.0f / 3.0f, color.green, 1e-6f);
    EXPECT_FLOAT_EQ(1.0f, color.blue);
}

TEST(WaveformRgbColumnSamplerTest, ShapesHighBandWithRekordboxCosineCurve) {
    EXPECT_FLOAT_EQ(0.0f, shapeRekordboxHighBand(0.0f));
    EXPECT_NEAR(127.5f, shapeRekordboxHighBand(127.5f), 1e-4f);
    EXPECT_FLOAT_EQ(255.0f, shapeRekordboxHighBand(255.0f));
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxEnvelopeUsesInstantAttackAndBandSpecificRelease) {
    RekordboxWaveformEnvelope envelope;
    advanceRekordboxWaveformEnvelope(&envelope, 200.0f, 200.0f, 200.0f);
    EXPECT_FLOAT_EQ(200.0f, envelope.low);
    EXPECT_FLOAT_EQ(200.0f, envelope.mid);
    EXPECT_FLOAT_EQ(200.0f, envelope.high);

    advanceRekordboxWaveformEnvelope(&envelope, 0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(200.0f * std::pow(0.99, 1000.0 / 441.0),
            envelope.low,
            1e-4f);
    EXPECT_NEAR(200.0f * std::pow(0.98, 1000.0 / 441.0),
            envelope.mid,
            1e-4f);
    EXPECT_NEAR(200.0f * std::pow(0.97, 1000.0 / 441.0),
            envelope.high,
            1e-4f);
    EXPECT_GT(envelope.low, envelope.mid);
    EXPECT_GT(envelope.mid, envelope.high);
}

TEST(WaveformRgbColumnSamplerTest,
        RekordboxEnvelopeSmoothsBandPeaksWithoutChangingOverallSignal) {
    constexpr int kVisualFrames = 64;
    auto data = makeWaveformData(kVisualFrames);
    data[20 * 2].filtered.low = 200;
    data[20 * 2].filtered.all = 150;

    const auto metrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 4.0, 1.0, 20.0 / 64.0, 0.0, 1.0);
    const auto raw = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            metrics);
    const auto smoothed = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            metrics,
            true);

    ASSERT_EQ(4U, raw.size());
    ASSERT_EQ(raw.size(), smoothed.size());
    EXPECT_FLOAT_EQ(200.0f, smoothed[0].lowAmplitude);
    EXPECT_FLOAT_EQ(0.0f, raw[1].lowAmplitude);
    EXPECT_GT(smoothed[1].lowAmplitude, 0.0f);
    EXPECT_FLOAT_EQ(raw[0].leftAmplitude, smoothed[0].leftAmplitude);
    EXPECT_FLOAT_EQ(raw[1].leftAmplitude, smoothed[1].leftAmplitude);
}

TEST(WaveformRgbColumnSamplerTest, LimitsWhiteBandToSeventyPercent) {
    const WaveformRgbColumn column{
            0.0f, 0.0f, 0.0f, 0.0f, 255.0f, 255.0f};
    const auto layers = buildRekordboxWaveformLayers(column);
    EXPECT_NEAR(178.5f, layers[0].amplitude, 1e-4f);
    EXPECT_EQ(kWaveformHighBand, layers[0].bands);
}

TEST(WaveformRgbColumnSamplerTest,
        SortsBandLayersAndAccumulatesOverlapPaletteMasks) {
    const std::array<WaveformRgbColumn, 6> columns{{
            {0.0f, 0.0f, 120.0f, 80.0f, 20.0f, 120.0f},
            {0.0f, 0.0f, 220.0f, 20.0f, 255.0f, 220.0f},
            {0.0f, 0.0f, 80.0f, 120.0f, 20.0f, 120.0f},
            {0.0f, 0.0f, 20.0f, 220.0f, 255.0f, 220.0f},
            {0.0f, 0.0f, 120.0f, 20.0f, 255.0f, 178.5f},
            {0.0f, 0.0f, 20.0f, 120.0f, 255.0f, 178.5f},
    }};
    const std::array<std::array<std::uint8_t, 3>, 6> expectedBands{{
            {{kWaveformLowBand,
                    kWaveformLowBand | kWaveformMidBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
            {{kWaveformLowBand,
                    kWaveformLowBand | kWaveformHighBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
            {{kWaveformMidBand,
                    kWaveformLowBand | kWaveformMidBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
            {{kWaveformMidBand,
                    kWaveformMidBand | kWaveformHighBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
            {{kWaveformHighBand,
                    kWaveformLowBand | kWaveformHighBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
            {{kWaveformHighBand,
                    kWaveformMidBand | kWaveformHighBand,
                    kWaveformLowBand | kWaveformMidBand | kWaveformHighBand}},
    }};

    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto layers = buildRekordboxWaveformLayers(columns[index]);
        EXPECT_GT(layers[0].amplitude, layers[1].amplitude);
        EXPECT_GT(layers[1].amplitude, layers[2].amplitude);
        for (std::size_t layer = 0; layer < layers.size(); ++layer) {
            EXPECT_EQ(expectedBands[index][layer], layers[layer].bands);
        }
    }
}

TEST(WaveformRgbColumnSamplerTest,
        OverviewFitsOrderedBandLayersToFullWaveformSilhouette) {
    const WaveformRgbColumn column{
            0.0f, 0.0f, 60.0f, 30.0f, 20.0f, 180.0f};
    const auto overviewLayers = buildRekordboxOverviewWaveformLayers(column);

    EXPECT_EQ(kWaveformLowBand, overviewLayers[0].bands);
    EXPECT_EQ(kWaveformMidBand, overviewLayers[1].bands);
    EXPECT_EQ(kWaveformHighBand, overviewLayers[2].bands);
    EXPECT_NEAR(2.0f,
            overviewLayers[0].amplitude / overviewLayers[1].amplitude,
            1e-4f);
    EXPECT_NEAR(180.0f,
            overviewLayers[0].amplitude + overviewLayers[1].amplitude +
                    overviewLayers[2].amplitude,
            1e-4f);
}

TEST(WaveformRgbColumnSamplerTest,
        OverviewShrinksStackWhenBandsExceedFullWaveformSilhouette) {
    const WaveformRgbColumn column{
            0.0f, 0.0f, 180.0f, 90.0f, 20.0f, 120.0f};
    const auto overviewLayers = buildRekordboxOverviewWaveformLayers(column);

    EXPECT_LT(overviewLayers[0].amplitude, column.lowAmplitude);
    EXPECT_NEAR(120.0f,
            overviewLayers[0].amplitude + overviewLayers[1].amplitude +
                    overviewLayers[2].amplitude,
            1e-4f);
}

TEST(WaveformRgbColumnSamplerTest, UsesRekordboxThreeBandPalette) {
    struct PaletteEntry {
        std::uint8_t bands;
        WaveformRgbColor color;
    };
    const std::array<PaletteEntry, 7> expected{{
            {kWaveformLowBand, {57, 197, 187}},
            {kWaveformMidBand, {230, 106, 154}},
            {kWaveformHighBand, {255, 255, 255}},
            {kWaveformLowBand | kWaveformMidBand, {184, 79, 120}},
            {kWaveformLowBand | kWaveformHighBand, {210, 220, 250}},
            {kWaveformMidBand | kWaveformHighBand, {255, 240, 215}},
            {kWaveformLowBand | kWaveformMidBand | kWaveformHighBand,
                    {245, 235, 215}},
    }};

    for (const PaletteEntry& entry : expected) {
        const WaveformRgbColor actual =
                rekordboxWaveformLayerColor(entry.bands);
        EXPECT_EQ(entry.color.red, actual.red);
        EXPECT_EQ(entry.color.green, actual.green);
        EXPECT_EQ(entry.color.blue, actual.blue);
    }
}

TEST(WaveformRgbColumnSamplerTest, OutputIsBoundedByPhysicalViewport) {
    auto data = makeWaveformData(50000);
    fillDeterministic(&data);
    const auto metrics = calculateWaveformRgbViewportMetrics(
            static_cast<int>(data.size()), 17.25, 2.0, 0.5, 0.5, 7.0);
    const auto columns = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            static_cast<int>(data.size()),
            metrics);
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
            completedMetrics);
    const auto incomplete = sampleWaveformRgbColumns(data.data(),
            static_cast<int>(data.size()),
            kCompletedDataElements,
            incompleteMetrics);
    ASSERT_EQ(1U, completed.size());
    ASSERT_EQ(1U, incomplete.size());
    EXPECT_FLOAT_EQ(200.0f, completed[0].lowAmplitude);
    EXPECT_FLOAT_EQ(200.0f, completed[0].unionAmplitude);
    EXPECT_FLOAT_EQ(0.0f, incomplete[0].lowAmplitude);
    EXPECT_FLOAT_EQ(0.0f, incomplete[0].unionAmplitude);
}

} // namespace
} // namespace mixxx::waveform
