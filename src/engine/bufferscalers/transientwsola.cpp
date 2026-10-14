#include "engine/bufferscalers/transientwsola.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kSequenceMs = 30;
constexpr double kOverlapMs = 8;
constexpr double kSeekMs = 12;
// Protected region around an onset: just before it and its attack body.
constexpr double kPreGuardMs = 2;
constexpr double kPostGuardMs = 12;
// Longest stretch of unmodified audio while waiting for a safe join.
constexpr double kMaxDeferMs = 90;
constexpr double kOnsetSpacingMs = 30;
// High-frequency energy rise against the preceding ~4-19 ms (6 dB).
constexpr double kOnsetRatio = 4.0;
constexpr double kOnsetFloorDb = -55;
// Prefer joins near the nominal position to limit timing jitter.
constexpr double kCenterWeight = 0.5;

constexpr int64_t kInputCapacityFrames = 1 << 15;
constexpr SINT kStretchedCapacityFrames = 1 << 13;

int64_t msToFrames(double ms, int sampleRate) {
    return static_cast<int64_t>(std::llround(ms * sampleRate / 1000.0));
}

} // namespace

TransientWsola::TransientWsola(int sampleRate)
        : m_sampleRate(sampleRate),
          m_overlap(msToFrames(kOverlapMs, sampleRate)),
          m_body(std::max<int64_t>(msToFrames(kSequenceMs, sampleRate) - m_overlap, 32)),
          m_seek(msToFrames(kSeekMs, sampleRate)),
          m_preGuard(msToFrames(kPreGuardMs, sampleRate)),
          m_postGuard(msToFrames(kPostGuardMs, sampleRate)),
          m_maxDefer(msToFrames(kMaxDeferMs, sampleRate)),
          m_onsetSpacing(msToFrames(kOnsetSpacingMs, sampleRate)),
          m_energyFloor(std::pow(10.0, kOnsetFloorDb / 10.0) * kHop),
          m_fade(static_cast<size_t>(m_overlap)),
          m_tempo(1.0),
          m_rate(1.0),
          m_in(2 * kInputCapacityFrames),
          m_mono(kInputCapacityFrames),
          m_mid(2 * kStretchedCapacityFrames) {
    for (int64_t i = 0; i < m_overlap; ++i) {
        m_fade[static_cast<size_t>(i)] = static_cast<float>((i + 0.5) / m_overlap);
    }
    clear();
}

void TransientWsola::setTempo(double tempo) {
    m_tempo = tempo;
}

void TransientWsola::setRate(double rate) {
    m_rate = rate;
}

void TransientWsola::clear() {
    m_base = 0;
    m_inFrames = 0;
    m_prevMono = 0;
    m_hopEnergy = 0;
    m_hopFill = 0;
    m_historyCount = 0;
    m_onsetCount = 0;
    m_onsetHead = 0;
    m_lastOnset = -m_onsetSpacing;
    m_inBody = true;
    m_cur = 0;
    m_target = 0;
    m_deferred = 0;
    m_midRead = 0;
    m_midWrite = 0;
    m_resamplePos = 0;
    m_stats = Stats();
}

void TransientWsola::compactInput() {
    // Keep the seek window behind the nominal position and the continuation.
    const int64_t keepFrom = std::min(m_cur, static_cast<int64_t>(std::floor(m_target))) -
            2 * m_seek - kHop;
    const int64_t drop = keepFrom - m_base;
    if (drop <= 0) {
        return;
    }
    const auto remaining = static_cast<size_t>(m_inFrames - drop);
    std::memmove(m_in.data(), m_in.data() + 2 * drop, 2 * remaining * sizeof(CSAMPLE));
    std::memmove(m_mono.data(), m_mono.data() + drop, remaining * sizeof(float));
    m_base += drop;
    m_inFrames -= drop;
}

SINT TransientWsola::freeInputFrames() {
    if (m_inFrames > kInputCapacityFrames / 2) {
        compactInput();
    }
    return static_cast<SINT>(kInputCapacityFrames - m_inFrames);
}

void TransientWsola::analyzeHop(double energy) {
    const int64_t hopStart = inEnd() - kHop;
    if (m_historyCount == kHistory) {
        std::copy(m_history.begin() + 1, m_history.end(), m_history.begin());
        --m_historyCount;
    }
    m_history[m_historyCount++] = energy;
    if (m_historyCount < kHistory) {
        return;
    }
    // Reference: hops 3..13 before the current one.
    double reference = 0;
    for (int i = 0; i < kHistory - 3; ++i) {
        reference += m_history[i];
    }
    reference /= kHistory - 3;
    if (energy > m_energyFloor && energy > kOnsetRatio * (reference + 1e-12) &&
            hopStart - m_lastOnset > m_onsetSpacing) {
        m_onsets[m_onsetHead] = hopStart;
        m_onsetHead = (m_onsetHead + 1) % kMaxOnsets;
        m_onsetCount = std::min(m_onsetCount + 1, kMaxOnsets);
        m_lastOnset = hopStart;
        ++m_stats.onsets;
    }
}

bool TransientWsola::touchesOnset(int64_t a, int64_t b) const {
    for (int i = 0; i < m_onsetCount; ++i) {
        const int64_t onset = m_onsets[(m_onsetHead - 1 - i + kMaxOnsets) % kMaxOnsets];
        if (onset + m_postGuard < a - m_postGuard) {
            break; // Older onsets are further away.
        }
        if (onset - m_preGuard < b && onset + m_postGuard > a) {
            return true;
        }
    }
    return false;
}

double TransientWsola::score(int64_t q, int64_t target) const {
    double num = 0;
    double den = 1e-9;
    for (int64_t i = 0; i < m_overlap; i += 2) {
        const float a = mono(m_cur + i);
        const float b = mono(q + i);
        num += a * b;
        den += b * b;
    }
    const double x = static_cast<double>(q - target) / m_seek;
    return num / std::sqrt(den) * (1.0 - kCenterWeight * x * x);
}

void TransientWsola::pushStretched(const CSAMPLE* pFrame) {
    m_mid[2 * m_midWrite] = pFrame[0];
    m_mid[2 * m_midWrite + 1] = pFrame[1];
    ++m_midWrite;
    m_target += m_tempo;
}

bool TransientWsola::stretch() {
    if (m_midWrite + std::max<int64_t>(m_body, m_overlap + kHop) > kStretchedCapacityFrames) {
        if (m_midRead == 0) {
            return false; // Wait for the output to be read.
        }
        const SINT remaining = m_midWrite - m_midRead;
        std::memmove(m_mid.data(), m_mid.data() + 2 * m_midRead, 2 * remaining * sizeof(CSAMPLE));
        m_midRead = 0;
        m_midWrite = remaining;
        if (m_midWrite + std::max<int64_t>(m_body, m_overlap + kHop) > kStretchedCapacityFrames) {
            return false;
        }
    }
    if (m_inBody) {
        if (m_cur + m_body > inEnd()) {
            return false;
        }
        for (int64_t i = 0; i < m_body; ++i) {
            pushStretched(&m_in[static_cast<size_t>(2 * (m_cur + i - m_base))]);
        }
        m_cur += m_body;
        m_deferred = 0;
        m_inBody = false;
        return true;
    }

    // Join: crossfade the continuation at m_cur into a new position.
    const auto target = static_cast<int64_t>(std::llround(m_target));
    const int64_t qLo = std::max(target - m_seek, m_base);
    const int64_t qHi = target + m_seek;
    if (std::max(qHi, m_cur) + m_overlap + m_postGuard + kHop > inEnd()) {
        return false; // Need input and onset analysis beyond the join.
    }
    const bool force = m_deferred >= m_maxDefer;
    const auto acceptable = [&](int64_t q) {
        // Joining in place is seamless.
        return force || q == m_cur ||
                !touchesOnset(std::min(m_cur, q), std::max(m_cur, q) + m_overlap);
    };
    int64_t best = -1;
    double bestScore = 0;
    for (int64_t q = qLo; q <= qHi; q += 4) {
        if (!acceptable(q)) {
            continue;
        }
        const double s = score(q, target);
        if (best < 0 || s > bestScore) {
            best = q;
            bestScore = s;
        }
    }
    if (best >= 0) {
        const int64_t coarse = best;
        for (int64_t q = std::max(qLo, coarse - 3); q <= std::min(qHi, coarse + 3); ++q) {
            if (q == coarse || !acceptable(q)) {
                continue;
            }
            const double s = score(q, target);
            if (s > bestScore) {
                best = q;
                bestScore = s;
            }
        }
    }
    if (std::llabs(m_cur - target) <= 1 && m_cur >= qLo) {
        best = m_cur; // Unity tempo: continue unchanged.
    }
    if (best < 0) {
        // Play the original through the onset and retry later.
        if (m_cur + kHop > inEnd()) {
            return false;
        }
        for (int64_t i = 0; i < kHop; ++i) {
            pushStretched(&m_in[static_cast<size_t>(2 * (m_cur + i - m_base))]);
        }
        m_cur += kHop;
        if (m_deferred == 0) {
            ++m_stats.deferredJoins;
        }
        m_deferred += kHop;
        return true;
    }
    if (force) {
        ++m_stats.forcedJoins;
    }
    if (best == m_cur) {
        // In-place join: copy exactly instead of crossfading equal audio.
        for (int64_t i = 0; i < m_overlap; ++i) {
            pushStretched(&m_in[static_cast<size_t>(2 * (m_cur + i - m_base))]);
        }
        m_cur += m_overlap;
        m_inBody = true;
        ++m_stats.joins;
        return true;
    }
    for (int64_t i = 0; i < m_overlap; ++i) {
        const auto a = static_cast<size_t>(2 * (m_cur + i - m_base));
        const auto b = static_cast<size_t>(2 * (best + i - m_base));
        const float w = m_fade[static_cast<size_t>(i)];
        const CSAMPLE frame[2] = {
                m_in[a] * (1 - w) + m_in[b] * w,
                m_in[a + 1] * (1 - w) + m_in[b + 1] * w};
        pushStretched(frame);
    }
    m_cur = best + m_overlap;
    m_inBody = true;
    ++m_stats.joins;
    return true;
}

void TransientWsola::putSamples(const CSAMPLE* pInterleaved, SINT frames) {
    frames = std::min(frames, freeInputFrames());
    for (SINT i = 0; i < frames; ++i) {
        const CSAMPLE left = pInterleaved[2 * i];
        const CSAMPLE right = pInterleaved[2 * i + 1];
        const auto index = static_cast<size_t>(m_inFrames);
        m_in[2 * index] = left;
        m_in[2 * index + 1] = right;
        const float mono = 0.5f * (left + right);
        m_mono[index] = mono;
        ++m_inFrames;
        // First difference emphasizes the high-frequency content of attacks.
        const float hf = mono - m_prevMono;
        m_prevMono = mono;
        m_hopEnergy += hf * hf;
        if (++m_hopFill == kHop) {
            analyzeHop(m_hopEnergy);
            m_hopEnergy = 0;
            m_hopFill = 0;
        }
    }
    while (stretch()) {
    }
}

SINT TransientWsola::receiveSamples(CSAMPLE* pInterleaved, SINT maxFrames) {
    while (stretch()) {
    }
    SINT produced = 0;
    if (m_rate == 1.0 && m_resamplePos == 0) {
        produced = std::min(maxFrames, stretchedFrames());
        std::copy_n(m_mid.data() + 2 * m_midRead, 2 * produced, pInterleaved);
        m_midRead += produced;
        return produced;
    }
    // Cubic Hermite interpolation, position relative to m_midRead.
    while (produced < maxFrames) {
        const auto i = static_cast<SINT>(m_resamplePos);
        if (i + 2 >= stretchedFrames()) {
            break;
        }
        const double f = m_resamplePos - i;
        for (int c = 0; c < 2; ++c) {
            const auto at = [&](SINT k) {
                return m_mid[2 * (m_midRead + std::max<SINT>(k, 0)) + c];
            };
            const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
            const double c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            pInterleaved[2 * produced + c] =
                    static_cast<CSAMPLE>(((c3 * f + c2) * f + c1) * f + y1);
        }
        ++produced;
        m_resamplePos += m_rate;
    }
    const auto consumed = std::max<SINT>(static_cast<SINT>(m_resamplePos) - 1, 0);
    m_midRead += consumed;
    m_resamplePos -= consumed;
    if (m_rate == 1.0 && m_resamplePos == std::floor(m_resamplePos)) {
        // Back to an integer phase: resume direct copying.
        m_midRead += static_cast<SINT>(m_resamplePos);
        m_resamplePos = 0;
    }
    return produced;
}
