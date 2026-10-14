#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "util/types.h"

// Transient-aware WSOLA time stretcher for interleaved stereo audio.
//
// Output is built from input segments joined by short crossfades, like
// SoundTouch. Before each join the stretcher checks whether a drum-like onset
// lies in the input that the join would skip, repeat or crossfade. Join
// positions touching an onset are rejected; if none is acceptable, the current
// segment keeps playing the original audio through the onset and the join is
// retried afterwards. The small timing debt is absorbed by the following
// joins, so the long-term tempo stays exact.
//
// Pitch and sample rate conversion are applied after the time stretch with a
// cubic resampler (rate = output pitch factor). At tempo 1 and rate 1 the
// output equals the input.
//
// All buffers are allocated in the constructor; putSamples(),
// receiveSamples() and clear() do not allocate.
class TransientWsola {
  public:
    explicit TransientWsola(int sampleRate);

    // Input frames consumed per output frame by the time stretch alone.
    void setTempo(double tempo);
    // Resampling factor: input frames per output frame after stretching.
    // Raises the pitch by this factor and also speeds up playback by it.
    void setRate(double rate);

    // Number of frames that putSamples() can currently accept. May discard
    // input history that is no longer needed.
    SINT freeInputFrames();
    void putSamples(const CSAMPLE* pInterleaved, SINT frames);
    SINT receiveSamples(CSAMPLE* pInterleaved, SINT maxFrames);
    void clear();

    // Statistics, cleared by clear().
    struct Stats {
        int64_t joins = 0;
        int64_t deferredJoins = 0;
        int64_t forcedJoins = 0;
        int64_t onsets = 0;
        int64_t syncedOnsets = 0;
        int64_t unsyncedOnsets = 0;
    };
    const Stats& stats() const {
        return m_stats;
    }

  private:
    static constexpr int kHop = 64;
    static constexpr int kHistory = 14;
    static constexpr int kMaxOnsets = 256;

    int64_t inEnd() const {
        return m_base + m_inFrames;
    }
    float mono(int64_t pos) const {
        return m_mono[static_cast<size_t>(pos - m_base)];
    }
    void analyzeHop(double energy);
    bool touchesOnset(int64_t a, int64_t b) const;
    int64_t nextOnsetAfter(int64_t pos) const;
    int64_t onsetBefore(int64_t pos) const;
    int64_t findJoin(int64_t qLo, int64_t qHi, int64_t target, bool force) const;
    double score(int64_t q, int64_t target, int64_t halfWidth) const;
    bool stretch();
    void compactInput();
    void pushStretched(const CSAMPLE* pFrame);
    SINT stretchedFrames() const {
        return m_midWrite - m_midRead;
    }

    const int m_sampleRate;
    const int64_t m_overlap;
    const int64_t m_body;
    const int64_t m_seek;
    const int64_t m_preGuard;
    const int64_t m_postGuard;
    const int64_t m_maxDefer;
    const int64_t m_onsetSpacing;
    const int64_t m_syncSeek;
    const int64_t m_syncMargin;
    const int64_t m_syncLead;
    const double m_energyFloor;
    std::vector<float> m_fade;

    double m_tempo;
    double m_rate;

    // Unstretched input, absolute frame position m_base at index 0.
    std::vector<CSAMPLE> m_in;
    std::vector<float> m_mono;
    int64_t m_base;
    int64_t m_inFrames;

    // Onset analysis.
    float m_prevMono;
    double m_hopEnergy;
    int m_hopFill;
    std::array<double, kHistory> m_history;
    int m_historyCount;
    std::array<int64_t, kMaxOnsets> m_onsets;
    int m_onsetCount;
    int m_onsetHead;
    int64_t m_lastOnset;

    // Stretch state.
    bool m_inBody;
    int64_t m_cur;
    double m_target;
    int64_t m_deferred;
    // Onset the next join must place on time, and the last one handled.
    int64_t m_syncOnset;
    int64_t m_lastSyncedOnset;
    // Input position up to which a placed onset is copied unchanged.
    int64_t m_holdUntil;

    // Stretched audio waiting for resampling.
    std::vector<CSAMPLE> m_mid;
    SINT m_midRead;
    SINT m_midWrite;
    double m_resamplePos;

    Stats m_stats;
};
