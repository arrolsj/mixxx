#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "audio/types.h"
#include "track/beats.h"
#include "waveform/renderers/waveformrgbcolumnsampler.h"

namespace mixxx::waveform {

constexpr int kMaximumRenderedBeatGridLines = 192;
constexpr int kBeatGridOverscanBeats = 4;

struct WaveformBeatGridLine {
    float x{0.0f};
    bool isDownbeat{false};
};

struct WaveformBeatGridData {
    BeatsPointer pBeats;
    int sourceBeatCount{0};
    int firstBeatOffset{0};
};

inline int positiveModulo(int value, int divisor) {
    return ((value % divisor) + divisor) % divisor;
}

/// Cache track-wide metadata once when a track or its beat grid changes.
/// The renderer retains the immutable Beats object without copying every beat;
/// zoom updates query only the visible range and never rebuild QVariant maps.
inline WaveformBeatGridData buildWaveformBeatGridData(
        const BeatsPointer& pBeats, audio::FramePos trackEndPosition) {
    WaveformBeatGridData data;
    if (pBeats == nullptr || !trackEndPosition.isValid() ||
            trackEndPosition <= audio::kStartFramePos) {
        return data;
    }

    const auto firstBeat = pBeats->iteratorFrom(audio::kStartFramePos);
    data.pBeats = pBeats;
    data.sourceBeatCount = pBeats->numBeatsInRange(
            audio::kStartFramePos, trackEndPosition);
    data.firstBeatOffset = firstBeat - pBeats->cfirstmarker();
    return data;
}

/// Select visible beat lines in the same viewport coordinates as the RGB
/// waveform. The result is bounded independently of track length and contains
/// no QML delegates or per-line scene graph nodes.
inline std::vector<WaveformBeatGridLine> sampleWaveformBeatGridLines(
        const WaveformBeatGridData& beatGridData,
        audio::FramePos trackEndPosition,
        const WaveformRgbViewportMetrics& viewport,
        double devicePixelRatio) {
    const double dpr = std::max(devicePixelRatio, 0.01);
    if (beatGridData.pBeats == nullptr || beatGridData.sourceBeatCount <= 0 ||
            !trackEndPosition.isValid() ||
            trackEndPosition <= audio::kStartFramePos ||
            !viewport.isValid() || viewport.trackWidth <= 0.0) {
        return {};
    }

    const int sourceBeatCount = beatGridData.sourceBeatCount;

    const double logicalViewportWidth = viewport.physicalWidth / dpr;
    const double trackEndFrame = trackEndPosition.value();
    const double firstVisibleFrame = std::clamp(
            -viewport.contentX / viewport.trackWidth * trackEndFrame,
            0.0,
            trackEndFrame);
    const double lastVisibleFrame = std::clamp(
            (logicalViewportWidth - viewport.contentX) /
                    viewport.trackWidth * trackEndFrame,
            0.0,
            trackEndFrame);

    auto firstBeat = beatGridData.pBeats->iteratorFrom(audio::kStartFramePos);
    const auto beatIndexAt = [&](double frame) {
        const int index =
                beatGridData.pBeats->iteratorFrom(audio::FramePos(frame)) -
                firstBeat;
        return std::clamp(index, 0, sourceBeatCount);
    };

    const int firstVisibleBeatIndex = std::max(
            0, beatIndexAt(firstVisibleFrame) - kBeatGridOverscanBeats);
    const int onePastLastVisibleBeatIndex = std::min(sourceBeatCount,
            beatIndexAt(lastVisibleFrame) + kBeatGridOverscanBeats);
    const int visibleSourceBeatCount = std::max(
            0, onePastLastVisibleBeatIndex - firstVisibleBeatIndex);
    if (visibleSourceBeatCount == 0) {
        return {};
    }

    const int beatStride =
            visibleSourceBeatCount <= kMaximumRenderedBeatGridLines
            ? 1
            : 4 * static_cast<int>(std::ceil(
                      static_cast<double>(visibleSourceBeatCount) /
                      (4 * kMaximumRenderedBeatGridLines)));
    int firstRenderedBeatIndex = firstVisibleBeatIndex;
    if (beatStride > 1) {
        const int alignmentLimit = std::min(
                onePastLastVisibleBeatIndex, firstVisibleBeatIndex + 4);
        while (firstRenderedBeatIndex < alignmentLimit &&
                positiveModulo(beatGridData.firstBeatOffset +
                                       firstRenderedBeatIndex,
                        4) != 0) {
            ++firstRenderedBeatIndex;
        }
    }

    const int renderedBeatCount =
            firstRenderedBeatIndex >= onePastLastVisibleBeatIndex
            ? 0
            : static_cast<int>(std::ceil(
                      static_cast<double>(onePastLastVisibleBeatIndex -
                              firstRenderedBeatIndex) /
                      beatStride));
    const int boundedRenderedBeatCount = std::min(
            renderedBeatCount, kMaximumRenderedBeatGridLines);
    std::vector<WaveformBeatGridLine> lines;
    lines.reserve(static_cast<std::size_t>(boundedRenderedBeatCount));

    auto beat = firstBeat + firstRenderedBeatIndex;
    for (int index = 0;
            index < boundedRenderedBeatCount;
            ++index, beat += beatStride) {
        const int sourceBeatIndex = firstRenderedBeatIndex + index * beatStride;
        const double trackX =
                (*beat).value() / trackEndFrame * viewport.trackWidth;
        const double viewportX = viewport.contentX + trackX;
        lines.push_back({
                static_cast<float>(std::round(viewportX * dpr) / dpr),
                positiveModulo(
                        beatGridData.firstBeatOffset + sourceBeatIndex, 4) == 0,
        });
    }
    return lines;
}

inline int waveformBeatGridDashCount(
        double logicalHeight, double devicePixelRatio, bool isDownbeat) {
    if (logicalHeight <= 0.0) {
        return 0;
    }
    const double strokeWidth = 2.0 / std::max(devicePixelRatio, 0.01);
    const double patternLength = isDownbeat ? 7.0 : 6.0;
    return static_cast<int>(std::ceil(
            logicalHeight / (patternLength * strokeWidth)));
}

inline std::size_t waveformBeatGridDashCount(
        const std::vector<WaveformBeatGridLine>& lines,
        double logicalHeight,
        double devicePixelRatio) {
    std::size_t count = 0;
    for (const auto& line : lines) {
        count += static_cast<std::size_t>(waveformBeatGridDashCount(
                logicalHeight, devicePixelRatio, line.isDownbeat));
    }
    return count;
}

} // namespace mixxx::waveform
