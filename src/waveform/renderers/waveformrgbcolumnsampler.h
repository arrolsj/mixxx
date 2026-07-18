#pragma once

#include <algorithm>
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

} // namespace mixxx::waveform
