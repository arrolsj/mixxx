#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "waveform/waveform.h"

namespace mixxx::waveform {

struct WaveformRgbColorF {
    float red{0.0f};
    float green{0.0f};
    float blue{0.0f};
};

struct WaveformRgbColumn {
    float leftAmplitude{0.0f};
    float rightAmplitude{0.0f};
    float red{0.0f};
    float green{0.0f};
    float blue{0.0f};
};
struct WaveformThreeBandColumn {
    float leftLow{0.0f};
    float rightLow{0.0f};
    float leftMid{0.0f};
    float rightMid{0.0f};
    float leftHigh{0.0f};
    float rightHigh{0.0f};
};

/// PWV7 uses a nonlinear display scale. Keep the transfer independent from
/// viewport columns: a whole-track robust reference preserves the source
/// dynamics, while this curve suppresses ordinary energy and leaves headroom
/// for transients above the 99th percentile.
constexpr float kThreeBandDisplayHeightScale = 0.78f;
constexpr float kThreeBandDisplayCurveStrength = 1.5f;
constexpr float kMixxxThreeBandReference = 255.0f;
constexpr float kMinimumRekordboxThreeBandReference = 64.0f;

inline float calculateRekordboxThreeBandReference(
        const std::vector<RekordboxThreeBandColumn>& source) {
    if (source.empty()) {
        return kMixxxThreeBandReference;
    }

    std::array<std::size_t, 256> histogram{};
    for (const RekordboxThreeBandColumn& column : source) {
        ++histogram[std::max({column.low, column.mid, column.high})];
    }

    constexpr double kRobustPercentile = 0.99;
    const std::size_t target = std::max<std::size_t>(
            1, static_cast<std::size_t>(
                       std::ceil(source.size() * kRobustPercentile)));
    std::size_t cumulative = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        cumulative += histogram[value];
        if (cumulative >= target) {
            return std::clamp(static_cast<float>(value),
                    kMinimumRekordboxThreeBandReference,
                    kMixxxThreeBandReference);
        }
    }
    return kMixxxThreeBandReference;
}

inline float threeBandDisplayAmplitude(float value, float reference) {
    if (value <= 0.0f) {
        return 0.0f;
    }
    const float normalized = value / std::max(reference, 1.0f);
    return kThreeBandDisplayHeightScale *
            (1.0f - std::exp(-kThreeBandDisplayCurveStrength *
                    normalized * normalized));
}

inline float peakPreservingRms(
        double squareSum, float maximum, std::size_t count) {
    if (count == 0) {
        return 0.0f;
    }
    const float rms = static_cast<float>(
            std::sqrt(squareSum / static_cast<double>(count)));
    constexpr float kPeakContribution = 0.35f;
    return rms + kPeakContribution * (maximum - rms);
}


/// Immutable mapping between a bounded viewport and the global waveform pixel
/// grid. The global integer pixel index is what makes waveform columns stable:
/// scrolling changes only firstGlobalPixel, never the sampling phase of a
/// column that remains visible.
struct WaveformRgbViewportMetrics {
    int physicalWidth{0};
    int visualFrameCount{0};
    std::int64_t firstGlobalPixel{0};
    double visualFramesPerPhysicalPixel{0.0};
    double trackWidth{0.0};
    double contentX{0.0};

    bool isValid() const {
        return physicalWidth > 0 && visualFrameCount > 0 &&
                visualFramesPerPhysicalPixel > 0.0;
    }
};

inline WaveformRgbViewportMetrics calculateWaveformRgbViewportMetrics(
        int dataSize,
        double logicalWidth,
        double devicePixelRatio,
        double displayPosition,
        double playMarkerPosition,
        double visualFramesPerLogicalPixel) {
    WaveformRgbViewportMetrics metrics;

    const double dpr = std::max(devicePixelRatio, 0.01);
    if (logicalWidth > 0.0) {
        metrics.physicalWidth = std::max(
                1, static_cast<int>(std::llround(logicalWidth * dpr)));
    }
    metrics.visualFrameCount = std::max(dataSize / 2, 0);
    if (metrics.physicalWidth == 0 || metrics.visualFrameCount == 0) {
        return metrics;
    }

    const double visualFramesPerLogical =
            std::max(visualFramesPerLogicalPixel, 0.01);
    metrics.visualFramesPerPhysicalPixel = visualFramesPerLogical / dpr;

    const double trackPhysicalPixels =
            static_cast<double>(metrics.visualFrameCount) /
            metrics.visualFramesPerPhysicalPixel;
    const std::int64_t playPixel =
            std::llround(displayPosition * trackPhysicalPixels);
    const std::int64_t playMarkerPixel = std::llround(
            playMarkerPosition * static_cast<double>(metrics.physicalWidth));

    metrics.firstGlobalPixel = playPixel - playMarkerPixel;
    metrics.trackWidth = trackPhysicalPixels / dpr;
    metrics.contentX = -static_cast<double>(metrics.firstGlobalPixel) / dpr;
    return metrics;
}

inline std::vector<WaveformRgbColumn> sampleWaveformRgbColumns(
        const WaveformData* data,
        int dataSize,
        int completion,
        const WaveformRgbViewportMetrics& viewport,
        const WaveformRgbColorF& lowColor,
        const WaveformRgbColorF& midColor,
        const WaveformRgbColorF& highColor) {
    if (data == nullptr || dataSize < 2 || !viewport.isValid()) {
        return {};
    }

    std::vector<WaveformRgbColumn> columns(
            static_cast<std::size_t>(viewport.physicalWidth));

    // The analyzer publishes completed stereo pairs through an atomic counter.
    // Keep the full data size for coordinates, but never read beyond that
    // published prefix while analysis is still running.
    int completedDataSize = std::clamp(completion, 0, dataSize);
    completedDataSize -= completedDataSize % 2;
    const int completedVisualFrames = completedDataSize / 2;

    const double halfSamplingRange =
            viewport.visualFramesPerPhysicalPixel / 2.0;
    for (int columnIndex = 0;
            columnIndex < viewport.physicalWidth;
            ++columnIndex) {
        const std::int64_t globalPixel =
                viewport.firstGlobalPixel + columnIndex;
        const double centerVisualFrame =
                static_cast<double>(globalPixel) *
                viewport.visualFramesPerPhysicalPixel;

        const long rawStart =
                std::lround(centerVisualFrame - halfSamplingRange);
        const long rawStop =
                std::lround(centerVisualFrame + halfSamplingRange);
        const long requestedStop = std::max(rawStop, rawStart + 1);
        const int visualFrameStart = static_cast<int>(
                std::clamp<long>(rawStart, 0, completedVisualFrames));
        const int visualFrameStop = static_cast<int>(
                std::clamp<long>(requestedStop, 0, completedVisualFrames));

        std::uint8_t maxLow = 0;
        std::uint8_t maxMid = 0;
        std::uint8_t maxHigh = 0;
        std::uint8_t maxAll[2]{0, 0};
        for (int visualFrame = visualFrameStart;
                visualFrame < visualFrameStop;
                ++visualFrame) {
            const int visualIndex = visualFrame * 2;
            for (int channel = 0; channel < 2; ++channel) {
                const WaveformData& value = data[visualIndex + channel];
                maxLow = std::max(maxLow, value.filtered.low);
                maxMid = std::max(maxMid, value.filtered.mid);
                maxHigh = std::max(maxHigh, value.filtered.high);
                maxAll[channel] =
                        std::max(maxAll[channel], value.filtered.all);
            }
        }

        const float low = static_cast<float>(maxLow);
        const float mid = static_cast<float>(maxMid);
        const float high = static_cast<float>(maxHigh);

        WaveformRgbColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        column.leftAmplitude = static_cast<float>(maxAll[0]);
        column.rightAmplitude = static_cast<float>(maxAll[1]);

        float red = low * lowColor.red + mid * midColor.red +
                high * highColor.red;
        float green = low * lowColor.green + mid * midColor.green +
                high * highColor.green;
        float blue = low * lowColor.blue + mid * midColor.blue +
                high * highColor.blue;
        const float maxComponent = std::max({red, green, blue});
        if (maxComponent > 0.0f) {
            red /= maxComponent;
            green /= maxComponent;
            blue /= maxComponent;
        } else {
            red = 0.0f;
            green = 0.0f;
            blue = 0.0f;
        }
        column.red = red;
        column.green = green;
        column.blue = blue;
    }

    return columns;
}

inline std::vector<WaveformThreeBandColumn> sampleWaveformThreeBandColumns(
        const WaveformData* data,
        int dataSize,
        int completion,
        const WaveformRgbViewportMetrics& viewport) {
    if (data == nullptr || dataSize < 2 || !viewport.isValid()) {
        return {};
    }

    std::vector<WaveformThreeBandColumn> columns(
            static_cast<std::size_t>(viewport.physicalWidth));
    int completedDataSize = std::clamp(completion, 0, dataSize);
    completedDataSize -= completedDataSize % 2;
    const int completedVisualFrames = completedDataSize / 2;
    const double halfSamplingRange =
            viewport.visualFramesPerPhysicalPixel / 2.0;

    for (int columnIndex = 0;
            columnIndex < viewport.physicalWidth;
            ++columnIndex) {
        const std::int64_t globalPixel =
                viewport.firstGlobalPixel + columnIndex;
        const double centerVisualFrame =
                static_cast<double>(globalPixel) *
                viewport.visualFramesPerPhysicalPixel;
        const long rawStart =
                std::lround(centerVisualFrame - halfSamplingRange);
        const long rawStop =
                std::lround(centerVisualFrame + halfSamplingRange);
        const long requestedStop = std::max(rawStop, rawStart + 1);
        const int visualFrameStart = static_cast<int>(
                std::clamp<long>(rawStart, 0, completedVisualFrames));
        const int visualFrameStop = static_cast<int>(
                std::clamp<long>(requestedStop, 0, completedVisualFrames));

        double lowSquareSum[2]{0.0, 0.0};
        double midSquareSum[2]{0.0, 0.0};
        double highSquareSum[2]{0.0, 0.0};
        float lowMaximum[2]{0.0f, 0.0f};
        float midMaximum[2]{0.0f, 0.0f};
        float highMaximum[2]{0.0f, 0.0f};
        std::size_t sampleCount = 0;
        for (int visualFrame = visualFrameStart;
                visualFrame < visualFrameStop;
                ++visualFrame) {
            const int visualIndex = visualFrame * 2;
            for (int channel = 0; channel < 2; ++channel) {
                const WaveformData& value = data[visualIndex + channel];
                const float low = value.filtered.low;
                const float mid = value.filtered.mid;
                const float high = value.filtered.high;
                lowSquareSum[channel] += low * low;
                midSquareSum[channel] += mid * mid;
                highSquareSum[channel] += high * high;
                lowMaximum[channel] = std::max(lowMaximum[channel], low);
                midMaximum[channel] = std::max(midMaximum[channel], mid);
                highMaximum[channel] = std::max(highMaximum[channel], high);
            }
            ++sampleCount;
        }

        WaveformThreeBandColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        column.leftLow = peakPreservingRms(
                lowSquareSum[0], lowMaximum[0], sampleCount);
        column.rightLow = peakPreservingRms(
                lowSquareSum[1], lowMaximum[1], sampleCount);
        column.leftMid = peakPreservingRms(
                midSquareSum[0], midMaximum[0], sampleCount);
        column.rightMid = peakPreservingRms(
                midSquareSum[1], midMaximum[1], sampleCount);
        column.leftHigh = peakPreservingRms(
                highSquareSum[0], highMaximum[0], sampleCount);
        column.rightHigh = peakPreservingRms(
                highSquareSum[1], highMaximum[1], sampleCount);
    }
    return columns;
}

inline std::vector<WaveformThreeBandColumn>
sampleRekordboxThreeBandColumns(
        const std::vector<RekordboxThreeBandColumn>& source,
        const WaveformRgbViewportMetrics& viewport) {
    if (source.empty() || !viewport.isValid()) {
        return {};
    }

    std::vector<WaveformThreeBandColumn> columns(
            static_cast<std::size_t>(viewport.physicalWidth));
    const double halfSamplingRange =
            viewport.visualFramesPerPhysicalPixel / 2.0;
    const double sourcePerVisualFrame =
            static_cast<double>(source.size()) /
            static_cast<double>(viewport.visualFrameCount);

    for (int columnIndex = 0;
            columnIndex < viewport.physicalWidth;
            ++columnIndex) {
        const std::int64_t globalPixel =
                viewport.firstGlobalPixel + columnIndex;
        const double centerVisualFrame =
                static_cast<double>(globalPixel) *
                viewport.visualFramesPerPhysicalPixel;
        const long rawStart =
                std::lround(centerVisualFrame - halfSamplingRange);
        const long rawStop =
                std::lround(centerVisualFrame + halfSamplingRange);
        const long requestedStop = std::max(rawStop, rawStart + 1);
        const int visualFrameStart = static_cast<int>(std::clamp<long>(
                rawStart, 0, viewport.visualFrameCount));
        const int visualFrameStop = static_cast<int>(std::clamp<long>(
                requestedStop, 0, viewport.visualFrameCount));
        if (visualFrameStart >= visualFrameStop) {
            continue;
        }

        const std::size_t sourceStart = std::min<std::size_t>(
                static_cast<std::size_t>(std::floor(
                        visualFrameStart * sourcePerVisualFrame)),
                source.size());
        const std::size_t sourceStop = std::min<std::size_t>(
                std::max(sourceStart + 1,
                        static_cast<std::size_t>(std::ceil(
                                visualFrameStop * sourcePerVisualFrame))),
                source.size());

        double lowSquareSum = 0.0;
        double midSquareSum = 0.0;
        double highSquareSum = 0.0;
        float lowMaximum = 0.0f;
        float midMaximum = 0.0f;
        float highMaximum = 0.0f;
        for (std::size_t sourceIndex = sourceStart;
                sourceIndex < sourceStop;
                ++sourceIndex) {
            const RekordboxThreeBandColumn& value = source[sourceIndex];
            const float low = value.low;
            const float mid = value.mid;
            const float high = value.high;
            lowSquareSum += low * low;
            midSquareSum += mid * mid;
            highSquareSum += high * high;
            lowMaximum = std::max(lowMaximum, low);
            midMaximum = std::max(midMaximum, mid);
            highMaximum = std::max(highMaximum, high);
        }
        const std::size_t sampleCount = sourceStop - sourceStart;
        const float low = peakPreservingRms(
                lowSquareSum, lowMaximum, sampleCount);
        const float mid = peakPreservingRms(
                midSquareSum, midMaximum, sampleCount);
        const float high = peakPreservingRms(
                highSquareSum, highMaximum, sampleCount);

        WaveformThreeBandColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        column.leftLow = column.rightLow = low;
        column.leftMid = column.rightMid = mid;
        column.leftHigh = column.rightHigh = high;
    }
    return columns;
}

} // namespace mixxx::waveform
