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

/// Convert one exported beat. The decoder timing offset may move the first
/// beats before the start of the track; they keep that position here so the
/// grid fit stays exact (see beatsFromGrid for the beat map fallback).
inline GridBeat gridBeatFromExport(audio::SampleRate sampleRate,
        int timeMillis,
        int timingOffsetMillis,
        int tempo) {
    return {audio::FramePos(sampleRate.value() / 1000.0 *
                    static_cast<double>(timeMillis - timingOffsetMillis)),
            tempo};
}

/// Build beats from a Rekordbox beat grid. Its beat times are rounded to
/// whole milliseconds, so a beat map of those times makes the local BPM
/// wander by about 0.1. Each run of beats with the same exported tempo is
/// fitted with a least-squares line and becomes one constant-tempo section.
/// The fitted beat length, not the exported tempo (rounded to 0.01 BPM),
/// keeps long tracks on Rekordbox's beats. A grid that does not fit within
/// the rounding tolerance (e.g. edited by hand) or lacks a tempo keeps the
/// exported beat positions.
inline BeatsPointer beatsFromGrid(audio::SampleRate sampleRate,
        const QVector<GridBeat>& grid,
        int firstBeatNumber,
        const QString& subVersion) {
    constexpr int kBeatsPerBar = 4;
    // Millisecond rounding of the exported times plus a margin.
    constexpr double kFitToleranceSeconds = 0.0015;
    if (grid.isEmpty()) {
        return nullptr;
    }
    const auto beatLength = [sampleRate](int tempo) {
        return 60.0 * 100.0 * sampleRate.value() / tempo;
    };
    const auto exportedPositions = [&] {
        // A decoder offset can move the first exported beats before the
        // start of the track; a beat map keeps them at 1 ms.
        const audio::FramePos earliestPosition(sampleRate.value() / 1000.0);
        QVector<audio::FramePos> positions;
        positions.reserve(grid.size());
        for (const auto& beat : grid) {
            positions.append(std::max(beat.position, earliestPosition));
        }
        const int firstTempo = grid.first().tempo;
        return Beats::fromBeatPositions(sampleRate,
                withDownbeatAnchor(positions,
                        firstBeatNumber,
                        firstTempo > 0 ? beatLength(firstTempo) : 0.0),
                subVersion);
    };
    if (std::any_of(grid.cbegin(), grid.cend(), [](const GridBeat& beat) {
            return beat.tempo <= 0;
        })) {
        return exportedPositions();
    }

    struct Section {
        int count;
        double first;
        double length;
    };
    std::vector<Section> sections;
    const double tolerance = kFitToleranceSeconds * sampleRate.value();
    for (int start = 0; start < grid.size();) {
        int end = start + 1;
        while (end < grid.size() && grid[end].tempo == grid[start].tempo) {
            ++end;
        }
        const int count = end - start;
        Section section{count, grid[start].position.value(), beatLength(grid[start].tempo)};
        if (count >= 2) {
            const double meanIndex = (count - 1) / 2.0;
            double meanPosition = 0;
            for (int i = start; i < end; ++i) {
                meanPosition += grid[i].position.value();
            }
            meanPosition /= count;
            double covariance = 0;
            double variance = 0;
            for (int i = 0; i < count; ++i) {
                covariance += (i - meanIndex) * (grid[start + i].position.value() - meanPosition);
                variance += (i - meanIndex) * (i - meanIndex);
            }
            section.length = covariance / variance;
            section.first = meanPosition - section.length * meanIndex;
            for (int i = 0; i < count; ++i) {
                const double fitted = section.first + i * section.length;
                if (std::abs(grid[start + i].position.value() - fitted) > tolerance) {
                    return exportedPositions();
                }
            }
        }
        if (!(section.length > 0)) {
            return exportedPositions();
        }
        sections.push_back(section);
        start = end;
    }

    const auto frame = [](double position) {
        return audio::FramePos(std::round(position));
    };
    const auto bpm = [sampleRate](double length) {
        return Bpm(60.0 * sampleRate.value() / length);
    };
    // Prepend beats before an export that starts on beat 2, 3 or 4, so the
    // first section starts on a downbeat (see withDownbeatAnchor).
    const int preroll = firstBeatNumber > 1 && firstBeatNumber <= kBeatsPerBar
            ? firstBeatNumber - 1
            : 0;
    const double anchor = sections.front().first - preroll * sections.front().length;
    if (sections.size() == 1) {
        return Beats::fromConstTempo(
                sampleRate, frame(anchor), bpm(sections.front().length), subVersion);
    }
    std::vector<BeatMarker> markers;
    for (std::size_t i = 0; i + 1 < sections.size(); ++i) {
        markers.emplace_back(frame(i == 0 ? anchor : sections[i].first),
                i == 0 ? sections[i].count + preroll : sections[i].count);
    }
    const Section& last = sections.back();
    return Beats::fromBeatMarkers(sampleRate,
            markers,
            frame(last.first),
            bpm(last.length),
            subVersion);
}

} // namespace mixxx::rekordbox
