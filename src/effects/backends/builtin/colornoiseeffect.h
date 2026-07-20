#pragma once

#include <memory>
#include <random>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1Low;

class ColorNoiseGroupState final : public EffectState {
  public:
    explicit ColorNoiseGroupState(const mixxx::EngineParameters& engineParameters);
    ~ColorNoiseGroupState() override;

    CSAMPLE_GAIN previousColor;
    double cutoff;
    mixxx::SampleBuffer noiseBuffer;
    mixxx::SampleBuffer filteredNoiseBuffer;
    std::mt19937 generator;
    std::unique_ptr<EngineFilterBiquad1Low> lowPassFilter;
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
