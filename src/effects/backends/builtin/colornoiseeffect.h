#pragma once

#include <memory>
#include <random>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1Band;

class ColorNoiseGroupState final : public EffectState {
  public:
    explicit ColorNoiseGroupState(const mixxx::EngineParameters& engineParameters);
    ~ColorNoiseGroupState() override;

    CSAMPLE_GAIN previousGain;
    CSAMPLE_GAIN previousToneGain;
    double bandPassCutoff;
    double bandPassQ;
    mixxx::SampleBuffer noiseBuffer;
    mixxx::SampleBuffer bandPassBuffer;
    std::mt19937 generator;
    std::unique_ptr<EngineFilterBiquad1Band> bandPassFilter;
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
