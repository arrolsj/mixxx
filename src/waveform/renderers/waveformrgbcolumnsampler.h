#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "waveform/waveform.h"

namespace mixxx::waveform {

struct WaveformRgbColumn {
    float leftAmplitude{0.0f};
    float rightAmplitude{0.0f};
    float lowAmplitude{0.0f};
    float midAmplitude{0.0f};
    float highAmplitude{0.0f};
    float unionAmplitude{0.0f};
};

struct WaveformRgbColorF {
    float red{0.0f};
    float green{0.0f};
    float blue{0.0f};
};

constexpr std::uint8_t kWaveformLowBand = 1U << 0;
constexpr std::uint8_t kWaveformMidBand = 1U << 1;
constexpr std::uint8_t kWaveformHighBand = 1U << 2;

struct WaveformRgbLayer {
    float amplitude{0.0f};
    std::uint8_t bands{0};
};

struct WaveformRgbColor {
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t blue{0};
};

struct RekordboxWaveformEnvelope {
    float low{0.0f};
    float mid{0.0f};
    float high{0.0f};
};

// Mixxx stores detailed waveform frames at 441 Hz. Rekordbox builds its PWV7
// display data from roughly 1 ms source peaks with instant attack and a
// band-specific exponential release. Scale those per-millisecond coefficients
// to one Mixxx visual frame so the time constants remain equivalent.
constexpr double kMixxxWaveformVisualFramesPerSecond = 441.0;
constexpr double kRekordboxEnvelopeFramesPerSecond = 1000.0;
constexpr double kRekordboxLowReleasePerFrame = 0.99;
constexpr double kRekordboxMidReleasePerFrame = 0.98;
constexpr double kRekordboxHighReleasePerFrame = 0.97;
constexpr float kRekordboxWhiteBandScale = 0.7f;

inline float rekordboxReleaseForMixxxFrame(double releasePerMillisecond) {
    return static_cast<float>(std::pow(releasePerMillisecond,
            kRekordboxEnvelopeFramesPerSecond /
                    kMixxxWaveformVisualFramesPerSecond));
}

inline float advanceRekordboxEnvelope(
        float previous, float input, float release) {
    return input >= previous
            ? input
            : input + release * (previous - input);
}

inline void advanceRekordboxWaveformEnvelope(
        RekordboxWaveformEnvelope* pEnvelope,
        float low,
        float mid,
        float high) {
    pEnvelope->low = advanceRekordboxEnvelope(pEnvelope->low,
            low,
            rekordboxReleaseForMixxxFrame(
                    kRekordboxLowReleasePerFrame));
    pEnvelope->mid = advanceRekordboxEnvelope(pEnvelope->mid,
            mid,
            rekordboxReleaseForMixxxFrame(
                    kRekordboxMidReleasePerFrame));
    pEnvelope->high = advanceRekordboxEnvelope(pEnvelope->high,
            high,
            rekordboxReleaseForMixxxFrame(
                    kRekordboxHighReleasePerFrame));
}

inline WaveformRgbColor rekordboxWaveformLayerColor(std::uint8_t bands) {
    switch (bands) {
    case kWaveformLowBand:
        return {57, 197, 187};
    case kWaveformMidBand:
        return {230, 106, 154};
    case kWaveformHighBand:
        return {255, 255, 255};
    case kWaveformLowBand | kWaveformMidBand:
        return {184, 79, 120};
    case kWaveformLowBand | kWaveformHighBand:
        return {210, 220, 250};
    case kWaveformMidBand | kWaveformHighBand:
        return {255, 240, 215};
    case kWaveformLowBand | kWaveformMidBand | kWaveformHighBand:
        return {245, 235, 215};
    default:
        return {0, 0, 0};
    }
}

// Colors for every layer band combination, indexed by the band mask. QML can
// override single entries; the rest keep the Rekordbox defaults above.
struct WaveformLayerPalette {
    std::array<WaveformRgbColor, 8> colors;

    WaveformLayerPalette() {
        for (std::size_t bands = 0; bands < colors.size(); ++bands) {
            colors[bands] = rekordboxWaveformLayerColor(
                    static_cast<std::uint8_t>(bands));
        }
    }

    WaveformRgbColor color(std::uint8_t bands) const {
        return bands < colors.size() ? colors[bands] : WaveformRgbColor{};
    }
};

inline WaveformRgbColorF mixWaveformRgbColor(
        const WaveformRgbColumn& column,
        const WaveformRgbColorF& lowColor,
        const WaveformRgbColorF& midColor,
        const WaveformRgbColorF& highColor) {
    WaveformRgbColorF color{
            column.lowAmplitude * lowColor.red +
                    column.midAmplitude * midColor.red +
                    column.highAmplitude * highColor.red,
            column.lowAmplitude * lowColor.green +
                    column.midAmplitude * midColor.green +
                    column.highAmplitude * highColor.green,
            column.lowAmplitude * lowColor.blue +
                    column.midAmplitude * midColor.blue +
                    column.highAmplitude * highColor.blue,
    };
    const float maxComponent = std::max({color.red, color.green, color.blue});
    if (maxComponent > 0.0f) {
        color.red /= maxComponent;
        color.green /= maxComponent;
        color.blue /= maxComponent;
    }
    return color;
}

inline float shapeRekordboxHighBand(float amplitude) {
    constexpr float kPi = 3.14159265358979323846f;
    const float normalized = std::clamp(amplitude / 255.0f, 0.0f, 1.0f);
    return 255.0f *
            (0.5f - 0.5f * std::cos(kPi * normalized));
}

inline std::array<WaveformRgbLayer, 3> buildRekordboxWaveformLayers(
        const WaveformRgbColumn& column) {
    std::array<WaveformRgbLayer, 3> layers{{
            {column.lowAmplitude, kWaveformLowBand},
            {column.midAmplitude, kWaveformMidBand},
            {shapeRekordboxHighBand(column.highAmplitude) *
                            kRekordboxWhiteBandScale,
                    kWaveformHighBand},
    }};
    std::stable_sort(layers.begin(),
            layers.end(),
            [](const auto& left, const auto& right) {
                return left.amplitude > right.amplitude;
            });

    std::uint8_t visibleBands = 0;
    for (WaveformRgbLayer& layer : layers) {
        visibleBands |= layer.bands;
        layer.bands = visibleBands;
    }
    return layers;
}

inline std::array<WaveformRgbLayer, 3> buildRekordboxOverviewWaveformLayers(
        const WaveformRgbColumn& column) {
    std::array<WaveformRgbLayer, 3> layers{{
            {column.lowAmplitude, kWaveformLowBand},
            {column.midAmplitude, kWaveformMidBand},
            {shapeRekordboxHighBand(column.highAmplitude) *
                            kRekordboxWhiteBandScale,
                    kWaveformHighBand},
    }};
    const float bandTotal = layers[0].amplitude + layers[1].amplitude +
            layers[2].amplitude;
    if (bandTotal <= 0.0f) {
        return layers;
    }

    // The overview summary's separated bands are substantially quieter than
    // its full-band peak. Fit three contiguous low-to-high segments inside the
    // full silhouette while preserving their relative thickness.
    const float targetAmplitude = column.unionAmplitude > 0.0f
            ? std::min(column.unionAmplitude, 255.0f)
            : std::min(bandTotal, 255.0f);
    const float scale = targetAmplitude / bandTotal;
    for (WaveformRgbLayer& layer : layers) {
        layer.amplitude *= scale;
    }
    return layers;
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
        bool useRekordboxEnvelope = false) {
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

    int envelopeFrameStart = 0;
    std::vector<RekordboxWaveformEnvelope> envelopeFrames;
    if (useRekordboxEnvelope && completedVisualFrames > 0) {
        const double firstCenterVisualFrame =
                static_cast<double>(viewport.firstGlobalPixel) *
                viewport.visualFramesPerPhysicalPixel;
        const double lastCenterVisualFrame =
                static_cast<double>(viewport.firstGlobalPixel +
                        viewport.physicalWidth - 1) *
                viewport.visualFramesPerPhysicalPixel;
        const int firstVisibleFrame = static_cast<int>(std::clamp<long>(
                std::lround(firstCenterVisualFrame - halfSamplingRange),
                0,
                completedVisualFrames));
        const int lastVisibleFrame = static_cast<int>(std::clamp<long>(
                std::max(std::lround(lastCenterVisualFrame + halfSamplingRange),
                        std::lround(lastCenterVisualFrame - halfSamplingRange) +
                                1),
                0,
                completedVisualFrames));
        constexpr double kEnvelopeWarmupSeconds = 0.3;
        const int warmupFrames = static_cast<int>(std::ceil(
                kMixxxWaveformVisualFramesPerSecond *
                kEnvelopeWarmupSeconds));
        envelopeFrameStart = std::max(0, firstVisibleFrame - warmupFrames);
        envelopeFrames.reserve(static_cast<std::size_t>(
                std::max(0, lastVisibleFrame - envelopeFrameStart)));

        RekordboxWaveformEnvelope envelope;
        for (int visualFrame = envelopeFrameStart;
                visualFrame < lastVisibleFrame;
                ++visualFrame) {
            const int visualIndex = visualFrame * 2;
            const WaveformData& left = data[visualIndex];
            const WaveformData& right = data[visualIndex + 1];
            advanceRekordboxWaveformEnvelope(&envelope,
                    std::max(left.filtered.low, right.filtered.low),
                    std::max(left.filtered.mid, right.filtered.mid),
                    std::max(left.filtered.high, right.filtered.high));
            envelopeFrames.push_back(envelope);
        }
    }

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
            if (useRekordboxEnvelope &&
                    visualFrame >= envelopeFrameStart &&
                    visualFrame - envelopeFrameStart <
                            static_cast<int>(envelopeFrames.size())) {
                const RekordboxWaveformEnvelope& envelope =
                        envelopeFrames[static_cast<std::size_t>(
                                visualFrame - envelopeFrameStart)];
                maxLow = std::max(maxLow,
                        static_cast<std::uint8_t>(std::clamp(
                                std::lround(envelope.low), 0L, 255L)));
                maxMid = std::max(maxMid,
                        static_cast<std::uint8_t>(std::clamp(
                                std::lround(envelope.mid), 0L, 255L)));
                maxHigh = std::max(maxHigh,
                        static_cast<std::uint8_t>(std::clamp(
                                std::lround(envelope.high), 0L, 255L)));
            }
        }

        WaveformRgbColumn& column =
                columns[static_cast<std::size_t>(columnIndex)];
        column.leftAmplitude = static_cast<float>(maxAll[0]);
        column.rightAmplitude = static_cast<float>(maxAll[1]);
        column.lowAmplitude = static_cast<float>(maxLow);
        column.midAmplitude = static_cast<float>(maxMid);
        column.highAmplitude = static_cast<float>(maxHigh);
        column.unionAmplitude = static_cast<float>(
                std::max(maxAll[0], maxAll[1]));
    }

    return columns;
}

} // namespace mixxx::waveform
