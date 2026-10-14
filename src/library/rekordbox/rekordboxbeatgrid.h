#pragma once

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/frame.h"
#include "audio/types.h"
#include "track/beats.h"
#include "track/bpm.h"

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

/// One entry of a Rekordbox beat grid (PQTZ).
struct GridBeat {
    audio::FramePos position;
    /// Tempo exported by Rekordbox for this beat, in BPM * 100.
    int tempo;
};

/// Build beats from Rekordbox's own tempo values. Its beat times are rounded
/// to whole milliseconds, so a beat map of those times makes the local BPM
/// wander by about 0.1. Each run of beats with the same exported tempo
/// becomes one constant-tempo section starting at its first beat, which
/// reproduces Rekordbox's grid and BPM. A grid with a missing tempo keeps
/// the exported beat positions.
inline BeatsPointer beatsFromGrid(audio::SampleRate sampleRate,
        const QVector<GridBeat>& grid,
        int firstBeatNumber,
        const QString& subVersion) {
    constexpr int kBeatsPerBar = 4;
    if (grid.isEmpty()) {
        return nullptr;
    }
    const auto beatLength = [sampleRate](int tempo) {
        return 60.0 * 100.0 * sampleRate.value() / tempo;
    };
    if (std::any_of(grid.cbegin(), grid.cend(), [](const GridBeat& beat) {
            return beat.tempo <= 0;
        })) {
        QVector<audio::FramePos> positions;
        positions.reserve(grid.size());
        for (const auto& beat : grid) {
            positions.append(beat.position);
        }
        const int firstTempo = grid.first().tempo;
        return Beats::fromBeatPositions(sampleRate,
                withDownbeatAnchor(positions,
                        firstBeatNumber,
                        firstTempo > 0 ? beatLength(firstTempo) : 0.0),
                subVersion);
    }

    const auto frame = [](double position) {
        return audio::FramePos(std::round(position));
    };
    const auto bpm = [](int tempo) {
        return Bpm(tempo / 100.0);
    };
    // Prepend beats before an export that starts on beat 2, 3 or 4, so the
    // first section starts on a downbeat (see withDownbeatAnchor).
    const int preroll = firstBeatNumber > 1 && firstBeatNumber <= kBeatsPerBar
            ? firstBeatNumber - 1
            : 0;
    const double anchor = grid.first().position.value() -
            preroll * beatLength(grid.first().tempo);

    std::vector<int> sectionStarts{0};
    for (int i = 1; i < grid.size(); ++i) {
        if (grid[i].tempo != grid[i - 1].tempo) {
            sectionStarts.push_back(i);
        }
    }
    if (sectionStarts.size() == 1) {
        return Beats::fromConstTempo(
                sampleRate, frame(anchor), bpm(grid.first().tempo), subVersion);
    }
    std::vector<BeatMarker> markers;
    for (std::size_t section = 0; section + 1 < sectionStarts.size(); ++section) {
        const int start = sectionStarts[section];
        const int beats = sectionStarts[section + 1] - start;
        markers.emplace_back(
                frame(section == 0 ? anchor : grid[start].position.value()),
                section == 0 ? beats + preroll : beats);
    }
    const GridBeat& last = grid[sectionStarts.back()];
    return Beats::fromBeatMarkers(sampleRate,
            markers,
            frame(last.position.value()),
            bpm(last.tempo),
            subVersion);
}

} // namespace mixxx::rekordbox
