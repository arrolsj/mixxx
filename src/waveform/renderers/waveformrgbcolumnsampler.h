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

enum class WaveformThreeBandBand {
    Low,
    Mid,
    High,
};

enum class WaveformThreeBandDisplayProfile {
    MixxxFallback,
    RekordboxPreview,
    RekordboxDetail,
};

struct WaveformThreeBandReference {
    float low{255.0f};
    float mid{255.0f};
    float high{255.0f};
};

struct WaveformThreeBandPreviewStack {
    float low{0.0f};
    float mid{0.0f};
    float high{0.0f};
};

struct WaveformThreeBandEnvelopeRange {
    float top{0.0f};
    float bottom{0.0f};
};

struct WaveformThreeBandEnvelopeEdge {
    float x{0.0f};
    WaveformThreeBandEnvelopeRange low;
    WaveformThreeBandEnvelopeRange mid;
    WaveformThreeBandEnvelopeRange high;
};

constexpr float kThreeBandDisplayHeightScale = 0.95f;
constexpr WaveformThreeBandReference kMixxxThreeBandReference{};
constexpr float kMinimumRekordboxThreeBandReference = 32.0f;

inline WaveformThreeBandReference calculateRekordboxThreeBandReference(
        const std::vector<RekordboxThreeBandColumn>& source) {
    if (source.empty()) {
        return kMixxxThreeBandReference;
    }

    std::array<std::size_t, 256> lowHistogram{};
    std::array<std::size_t, 256> midHistogram{};
    std::array<std::size_t, 256> highHistogram{};
    for (const RekordboxThreeBandColumn& column : source) {
        // PWV6/PWV7 store independently scaled M/H/L bytes. A shared
        // reference makes the sparse low byte disappear, so each band gets
        // its own robust whole-track anchor.
        ++lowHistogram[column.low];
        ++midHistogram[column.mid];
        ++highHistogram[column.high];
    }

    constexpr double kRobustPercentile = 0.99;
    const std::size_t target = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::ceil(source.size() * kRobustPercentile)));
    const auto robustReference = [target](
                                         const std::array<std::size_t, 256>& histogram) {
        std::size_t cumulative = 0;
        for (std::size_t value = 0; value < histogram.size(); ++value) {
            cumulative += histogram[value];
            if (cumulative >= target) {
                return std::clamp(static_cast<float>(value),
                        kMinimumRekordboxThreeBandReference,
                        255.0f);
            }
        }
        return 255.0f;
    };
    return {
            robustReference(lowHistogram),
            robustReference(midHistogram),
            robustReference(highHistogram),
    };
}

inline float threeBandDisplayScale(WaveformThreeBandBand band,
        WaveformThreeBandDisplayProfile profile) {
    switch (profile) {
    case WaveformThreeBandDisplayProfile::RekordboxPreview:
        // PWV6 is conventionally rendered as a cumulative preview. These
        // public reverse-engineered component weights are kept separate from
        // the PWV7 detail profile.
        switch (band) {
        case WaveformThreeBandBand::Low:
            return 0.49f;
        case WaveformThreeBandBand::Mid:
            return 0.32f;
        case WaveformThreeBandBand::High:
            return 0.25f;
        }
        break;
    case WaveformThreeBandDisplayProfile::RekordboxDetail:
        // Public PWV7 implementations confirm that the native bytes need
        // band-specific display scaling, but do not perfectly match rekordbox.
        // These ratios are calibrated against corresponding rekordbox pixels
        // and public .2EX fixtures; they are display weights, not crossovers.
        switch (band) {
        case WaveformThreeBandBand::Low:
            return 0.50f;
        case WaveformThreeBandBand::Mid:
            return 0.24f;
        case WaveformThreeBandBand::High:
            return 0.12f;
        }
        break;
    case WaveformThreeBandDisplayProfile::MixxxFallback:
        // Mixxx analysis uses actual filter magnitudes, but the high band is
        // perceptually dense. Keep it slightly stronger than native PWV7
        // without letting it mask the low-frequency envelope.
        switch (band) {
        case WaveformThreeBandBand::Low:
            return 1.0f;
        case WaveformThreeBandBand::Mid:
            return 0.75f;
        case WaveformThreeBandBand::High:
            return 0.18f;
        }
        break;
    }
    return 0.0f;
}

inline float threeBandDisplayPower(
        WaveformThreeBandDisplayProfile profile) {
    // PWV7's visible scale is strongly concave. A fifth-root transfer retains
    // exact onset positions while restoring low-band presence among its many
    // small native byte values. Preview and Mixxx-filtered data keep their
    // linear semantics.
    return profile == WaveformThreeBandDisplayProfile::RekordboxDetail
            ? 0.20f
            : 1.0f;
}

inline float threeBandReferenceForBand(
        const WaveformThreeBandReference& reference,
        WaveformThreeBandBand band) {
    switch (band) {
    case WaveformThreeBandBand::Low:
        return reference.low;
    case WaveformThreeBandBand::Mid:
        return reference.mid;
    case WaveformThreeBandBand::High:
        return reference.high;
    }
    return 255.0f;
}

inline float threeBandDisplayAmplitude(float value,
        const WaveformThreeBandReference& reference,
        WaveformThreeBandBand band,
        WaveformThreeBandDisplayProfile profile) {
    if (value <= 0.0f) {
        return 0.0f;
    }
    const float normalizedEnergy =
            value / std::max(threeBandReferenceForBand(reference, band), 1.0f);
    const float normalized = std::pow(
                                     normalizedEnergy,
                                     threeBandDisplayPower(profile)) *
            threeBandDisplayScale(band, profile);
    return std::clamp(normalized, 0.0f, kThreeBandDisplayHeightScale);
}

inline WaveformThreeBandPreviewStack calculateRekordboxThreeBandPreviewStack(
        const RekordboxThreeBandColumn& column) {
    // PWV6 bytes are already encoded for the fixed public preview weights.
    // Unlike PWV7 detail, normalizing each band to its own track percentile
    // would destroy the native M/H/L relationship before stacking.
    constexpr WaveformThreeBandReference reference =
            kMixxxThreeBandReference;
    WaveformThreeBandPreviewStack stack;
    stack.low = threeBandDisplayAmplitude(column.low,
            reference,
            WaveformThreeBandBand::Low,
            WaveformThreeBandDisplayProfile::RekordboxPreview);
    stack.mid = std::min(kThreeBandDisplayHeightScale,
            stack.low + threeBandDisplayAmplitude(column.mid, reference, WaveformThreeBandBand::Mid, WaveformThreeBandDisplayProfile::RekordboxPreview));
    stack.high = std::min(kThreeBandDisplayHeightScale,
            stack.mid + threeBandDisplayAmplitude(column.high, reference, WaveformThreeBandBand::High, WaveformThreeBandDisplayProfile::RekordboxPreview));
    return stack;
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

        double lowSum = 0.0;
        double midSum = 0.0;
        double highSum = 0.0;
        for (std::size_t sourceIndex = sourceStart;
                sourceIndex < sourceStop;
                ++sourceIndex) {
            const RekordboxThreeBandColumn& value = source[sourceIndex];
            lowSum += value.low;
            midSum += value.mid;
            highSum += value.high;
        }
        const std::size_t sampleCount = sourceStop - sourceStart;
        const float low =
                static_cast<float>(lowSum / static_cast<double>(sampleCount));
        const float mid =
                static_cast<float>(midSum / static_cast<double>(sampleCount));
        const float high =
                static_cast<float>(highSum / static_cast<double>(sampleCount));

        WaveformThreeBandColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        column.leftLow = column.rightLow = low;
        column.leftMid = column.rightMid = mid;
        column.leftHigh = column.rightHigh = high;
    }
    return columns;
}

inline std::vector<WaveformThreeBandEnvelopeEdge>
buildWaveformThreeBandEnvelopeEdges(
        const std::vector<WaveformThreeBandColumn>& columns,
        float halfHeight,
        double devicePixelRatio,
        const WaveformThreeBandReference& reference,
        WaveformThreeBandDisplayProfile profile,
        float lowGain,
        float midGain,
        float highGain) {
    if (columns.empty() || halfHeight <= 0.0f) {
        return {};
    }

    const float dpr = static_cast<float>(
            std::max(devicePixelRatio, 0.01));
    const float halfPhysicalPixel = 0.5f / dpr;
    const auto displayHeight = [&](float value,
                                       WaveformThreeBandBand band,
                                       float gain) {
        const WaveformThreeBandReference& displayReference =
                profile == WaveformThreeBandDisplayProfile::RekordboxPreview
                ? kMixxxThreeBandReference
                : reference;
        return std::min(kThreeBandDisplayHeightScale,
                threeBandDisplayAmplitude(
                        value, displayReference, band, profile) *
                        std::max(gain, 0.0f));
    };
    const auto bandRange = [&](const WaveformThreeBandColumn& column,
                                   WaveformThreeBandBand band,
                                   float gain) {
        float left = 0.0f;
        float right = 0.0f;
        switch (band) {
        case WaveformThreeBandBand::Low:
            left = column.leftLow;
            right = column.rightLow;
            break;
        case WaveformThreeBandBand::Mid:
            left = column.leftMid;
            right = column.rightMid;
            break;
        case WaveformThreeBandBand::High:
            left = column.leftHigh;
            right = column.rightHigh;
            break;
        }
        const float leftHeight = displayHeight(left, band, gain);
        const float rightHeight = displayHeight(right, band, gain);
        return WaveformThreeBandEnvelopeRange{
                halfHeight - halfHeight * leftHeight,
                halfHeight + halfHeight * rightHeight};
    };
    const auto previewStack = [&](float low, float mid, float high) {
        WaveformThreeBandPreviewStack stack;
        stack.low = displayHeight(
                low, WaveformThreeBandBand::Low, lowGain);
        stack.mid = std::min(kThreeBandDisplayHeightScale,
                stack.low + displayHeight(mid, WaveformThreeBandBand::Mid, midGain));
        stack.high = std::min(kThreeBandDisplayHeightScale,
                stack.mid + displayHeight(high, WaveformThreeBandBand::High, highGain));
        return stack;
    };
    const auto makeEdge = [&](float x,
                                  const WaveformThreeBandColumn& column) {
        if (profile ==
                WaveformThreeBandDisplayProfile::RekordboxPreview) {
            const WaveformThreeBandPreviewStack left = previewStack(
                    column.leftLow, column.leftMid, column.leftHigh);
            const WaveformThreeBandPreviewStack right = previewStack(
                    column.rightLow, column.rightMid, column.rightHigh);
            const auto stackedRange = [&](float leftHeight,
                                              float rightHeight) {
                return WaveformThreeBandEnvelopeRange{
                        halfHeight - halfHeight * leftHeight,
                        halfHeight + halfHeight * rightHeight};
            };
            return WaveformThreeBandEnvelopeEdge{
                    x,
                    stackedRange(left.low, right.low),
                    stackedRange(left.mid, right.mid),
                    stackedRange(left.high, right.high)};
        }
        return WaveformThreeBandEnvelopeEdge{
                x,
                bandRange(column, WaveformThreeBandBand::Low, lowGain),
                bandRange(column, WaveformThreeBandBand::Mid, midGain),
                bandRange(column, WaveformThreeBandBand::High, highGain)};
    };

    std::vector<WaveformThreeBandEnvelopeEdge> edges;
    edges.reserve(columns.size() + 2);
    edges.push_back(makeEdge(-halfPhysicalPixel, columns.front()));
    if (columns.size() == 1) {
        edges.push_back(makeEdge(halfPhysicalPixel, columns.front()));
        return edges;
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
        edges.push_back(makeEdge(
                static_cast<float>(index) / dpr, columns[index]));
    }
    edges.push_back(makeEdge(
            static_cast<float>(columns.size() - 1) / dpr +
                    halfPhysicalPixel,
            columns.back()));
    return edges;
}

} // namespace mixxx::waveform
