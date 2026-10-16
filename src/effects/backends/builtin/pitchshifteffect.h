#pragma once

#include <QMap>
#include <array>
#include <vector>

#include "effects/backends/builtin/performancefxdsp.h"
#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/types.h"

namespace lastcue {

/// Time-domain delay-line pitch shifter matching the default Beat FX PITCH
/// path of rekordbox 7.2.19 (rbxfrm::PitchShifter). The read head moves at
/// 1 + f samples per sample; splices are aligned to rising zero crossings of
/// a 30-300 Hz band of the input and cross-faded with a smoothstep window.
/// See docs/evidence/rkb-bfx-pitch-20261016 in the LastCUE Box repository.
class RbxPitchShifter {
  public:
    void configure(double sampleRate, int maxFramesPerBuffer);
    void reset();
    /// Control in the native integer unit: -200..+200.
    void setControl(float control);
    /// In-place stereo processing of interleaved frames.
    void process(CSAMPLE* pInterleaved, int frames);

    float smoothedOffset() const {
        return m_offset;
    }

    /// The read-speed offset f (read speed is 1 + f) for a native control.
    static float offsetForControl(float control);

  private:
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::array<std::array<float, 4>, 2> memory{};
        void configure(double sampleRate, double cutoff, bool highPass);
        float process(float x, int channel);
    };

    float read(int channel, int delay) const;
    void readInterpolated(float delay, float* pLeft, float* pRight) const;
    void controlSplice();

    double m_sampleRate = 44100;
    int m_minWindow = 573;
    int m_maxWindow = 1014;
    int m_crossfade = 132;
    float m_crossfadeStep = 1.0f / 132;
    int m_startFill = 1323;
    int m_gateFrames = 882;

    std::vector<CSAMPLE> m_ring;
    int m_ringFrames = 0;
    int m_write = 0;
    int m_filled = 0;

    float m_target = 0;
    float m_offset = 0;
    float m_delay = 0;
    float m_jump = 0;
    bool m_splicing = false;
    bool m_starting = true;
    int m_crossfadeCount = 0;
    int m_zeroCrossingAge = 0;

    Biquad m_highPass;
    Biquad m_lowPass;
    std::vector<float> m_analysis;

    performance_fx::Ramp m_gate;
};

} // namespace lastcue

class PitchShiftGroupState : public EffectState {
  public:
    PitchShiftGroupState(const mixxx::EngineParameters& engineParameters);
    ~PitchShiftGroupState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters);

    lastcue::RbxPitchShifter shifter;
    std::vector<CSAMPLE> wet;
    performance_fx::Ramp dry;
    performance_fx::Ramp wetGain;
    bool initialized = false;
};

class PitchShiftEffect final : public EffectProcessorImpl<PitchShiftGroupState> {
  public:
    PitchShiftEffect() = default;
    ~PitchShiftEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            PitchShiftGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

    /// Native control x = (400 * p - 200) * a for knob p in [0, 1] and
    /// LEVEL/DEPTH a in [0, 1]; the knob here is stored as v = 2p - 1.
    static float nativeControl(double pitch, double depth);

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pPitchParameter;
    EngineEffectParameterPointer m_pDepthParameter;

    DISALLOW_COPY_AND_ASSIGN(PitchShiftEffect);
};
