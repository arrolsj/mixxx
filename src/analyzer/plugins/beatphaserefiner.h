#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mixxx {

// Keep a 1 ms stereo-energy envelope, not a second copy of the track. This
// refines only the phase of an already fitted constant-tempo grid.
class BeatPhaseRefiner {
  public:
    void reset(double sampleRate) {
        m_binFrames = std::max(1, int(std::lround(sampleRate * 0.001)));
        m_sampleRate = sampleRate;
        m_energy.clear();
        m_sum = 0;
        m_count = 0;
    }

    template<typename Sample>
    void process(const Sample* stereo, std::size_t samples) {
        for (std::size_t i = 0; i + 1 < samples; i += 2) {
            const double left = stereo[i];
            const double right = stereo[i + 1];
            m_sum += (left * left + right * right) * 0.5;
            if (++m_count == m_binFrames) {
                m_energy.push_back(static_cast<float>(m_sum / m_binFrames));
                m_sum = 0;
                m_count = 0;
            }
        }
    }

    double correction(double firstBeatFrames, double beatLengthFrames) const {
        if (!finitePosition(firstBeatFrames) || !finitePosition(beatLengthFrames) ||
                beatLengthFrames < m_sampleRate * 0.1 || m_sampleRate <= 0) {
            return 0;
        }
        const int radius = int(std::ceil(0.035 * m_sampleRate / m_binFrames));
        const double end = double(m_energy.size()) * m_binFrames;
        std::vector<double> offsets;
        int examined = 0;
        double first = std::fmod(firstBeatFrames, beatLengthFrames);
        if (first < 0) {
            first += beatLengthFrames;
        }
        for (double beat = first; beat < end; beat += beatLengthFrames) {
            const int center = int(beat / m_binFrames);
            if (center - radius < 1 || center + radius >= int(m_energy.size())) {
                continue;
            }
            ++examined;
            const int lo = center - radius;
            const int hi = center + radius;
            double largestRise = 0;
            double peakEnergy = 0;
            int best = -1;
            for (int i = lo; i <= hi; ++i) {
                peakEnergy = std::max(peakEnergy, double(m_energy[i]));
                const double rise = double(m_energy[i]) - m_energy[i - 1];
                if (rise > largestRise) {
                    largestRise = rise;
                    best = i;
                }
            }
            // Reject silence and slow/sustained energy changes. Do not shift
            // a grid merely because there is some local waveform maximum.
            if (best < 0 || peakEnergy < 1e-10 || largestRise < peakEnergy * 0.3) {
                continue;
            }
            while (best > lo &&
                    double(m_energy[best - 1]) - m_energy[best - 2] > largestRise * 0.1) {
                --best;
            }
            if (m_energy[best - 1] > peakEnergy * 0.2) {
                continue;
            }
            offsets.push_back(double(best) * m_binFrames - beat);
        }
        if (offsets.size() < 12 || offsets.size() * 2 < std::size_t(examined)) {
            return 0;
        }
        std::sort(offsets.begin(), offsets.end());
        const double median = offsets[offsets.size() / 2];
        const auto agreeing = std::count_if(offsets.begin(), offsets.end(), [&](double value) {
            return std::abs(value - median) <= m_sampleRate * 0.003;
        });
        if (agreeing * 10 < examined * 7 || std::abs(median) > m_sampleRate * 0.025) {
            return 0;
        }
        return std::round(median);
    }

  private:
    static bool finitePosition(double value) {
        // The product uses -ffast-math, which may erase std::isfinite checks.
        // Inspect the IEEE-754 exponent so invalid FramePos values fail closed.
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
    }

    double m_sampleRate = 0;
    int m_binFrames = 1;
    int m_count = 0;
    double m_sum = 0;
    std::vector<float> m_energy;
};

} // namespace mixxx
