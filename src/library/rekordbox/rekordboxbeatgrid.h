#pragma once

#include <QVector>

#include <cmath>

#include "audio/frame.h"

namespace mixxx::rekordbox {

/// Preserve Rekordbox bar phase when its exported beat list starts on beat
/// 2, 3, or 4. The inserted preroll beats only establish the persisted
/// downbeat anchor; every exported beat position remains unchanged.
inline QVector<audio::FramePos> withDownbeatAnchor(
        const QVector<audio::FramePos>& beatPositions,
        int firstBeatNumber,
        audio::FrameDiff_t firstBeatLengthFrames) {
    constexpr int kBeatsPerBar = 4;
    if (beatPositions.isEmpty() || firstBeatNumber <= 1) {
        return beatPositions;
    }
    if (firstBeatNumber > kBeatsPerBar ||
            !std::isfinite(firstBeatLengthFrames) || firstBeatLengthFrames <= 0.0) {
        return beatPositions;
    }

    QVector<audio::FramePos> anchoredBeatPositions;
    anchoredBeatPositions.reserve(beatPositions.size() + firstBeatNumber - 1);
    const audio::FramePos firstPosition = beatPositions.first();
    for (int beatsBefore = firstBeatNumber - 1; beatsBefore > 0; --beatsBefore) {
        anchoredBeatPositions.append(
                firstPosition - beatsBefore * firstBeatLengthFrames);
    }
    anchoredBeatPositions.append(beatPositions);
    return anchoredBeatPositions;
}

} // namespace mixxx::rekordbox
