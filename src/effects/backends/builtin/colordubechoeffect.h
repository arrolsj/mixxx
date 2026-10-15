#pragma once

#include <array>
#include <memory>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1Band;
class EngineFilterBiquad1High;
class EngineFilterBiquad1Low;

class ColorDubEchoGroupState final : public EffectState {
  public:
    explicit ColorDubEchoGroupState(
            const mixxx::EngineParameters& engineParameters);
    ~ColorDubEchoGroupState() override;

    struct Branch {
        explicit Branch(int samples, int blockSamples);
        mixxx::SampleBuffer delay;
        mixxx::SampleBuffer work;
        double delayFrames = 0;
        double nextDelayFrames = 0;
        double requestedDelayFrames = 0;
        int fadeRemaining = 0;
        float send = 0;
        float targetSend = 0;
        float sendStep = 0;
        int sendRemaining = 0;
        float feedback = 0;
    };
    int color = 127;
    int controlCountdown = 0;
    int writeFrame = 0;
    // Default filter selection fades the branch output down, then up to unity.
    float outputGain = 0.7f;
    int outputGainRemaining = 0;
    bool outputGainFadingOut = true;
    std::array<Branch, 2> branches;
    std::unique_ptr<EngineFilterBiquad1Band> leftPre;
    std::unique_ptr<EngineFilterBiquad1High> rightPre;
    std::unique_ptr<EngineFilterBiquad1Low> rightPost;
};

class ColorDubEchoEffect : public EffectProcessorImpl<ColorDubEchoGroupState> {
  public:
    ColorDubEchoEffect() = default;
    ~ColorDubEchoEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void processChannel(
            ColorDubEchoGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

  private:
    EngineEffectParameterPointer m_pColorParameter;

    DISALLOW_COPY_AND_ASSIGN(ColorDubEchoEffect);
};
