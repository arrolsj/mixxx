#pragma once

#include <array>
#include <random>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/types.h"

class ColorNoiseGroupState final : public EffectState {
  public:
    explicit ColorNoiseGroupState(const mixxx::EngineParameters& engineParameters);
    ~ColorNoiseGroupState() override;

    CSAMPLE_GAIN previousGain;
    CSAMPLE_GAIN previousFeedback;
    double bandPassCutoff;
    double bandPassQ;
    std::mt19937 generator;

    void setBandPass(double frequency, double q, double sampleRate);
    CSAMPLE processBandPass(CSAMPLE input, int channel, CSAMPLE_GAIN feedback);
    struct FilterState {
        CSAMPLE input1 = 0;
        CSAMPLE input2 = 0;
        CSAMPLE output1 = 0;
        CSAMPLE output2 = 0;
    };
    std::array<FilterState, mixxx::kEngineChannelCount> filterState{};
    CSAMPLE b0 = 0;
    CSAMPLE a1 = 0;
    CSAMPLE a2 = 0;
};

class ColorNoiseEffect : public EffectProcessorImpl<ColorNoiseGroupState> {
  public:
    ColorNoiseEffect() = default;
    ~ColorNoiseEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void processChannel(
            ColorNoiseGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

  private:
    EngineEffectParameterPointer m_pColorParameter;

    DISALLOW_COPY_AND_ASSIGN(ColorNoiseEffect);
};
