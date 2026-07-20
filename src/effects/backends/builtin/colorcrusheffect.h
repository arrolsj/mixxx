#pragma once

#include <memory>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1High;

class ColorCrushGroupState final : public EffectState {
  public:
    explicit ColorCrushGroupState(const mixxx::EngineParameters& engineParameters);
    ~ColorCrushGroupState() override;

    CSAMPLE_GAIN previousColor;
    CSAMPLE heldLeft;
    CSAMPLE heldRight;
    CSAMPLE accumulator;
    double highPassCutoff;
    double previousRightAmount;
    mixxx::SampleBuffer crushedBuffer;
    mixxx::SampleBuffer filteredBuffer;
    std::unique_ptr<EngineFilterBiquad1High> highPassFilter;
};

class ColorCrushEffect : public EffectProcessorImpl<ColorCrushGroupState> {
  public:
    ColorCrushEffect() = default;
    ~ColorCrushEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void processChannel(
            ColorCrushGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

  private:
    EngineEffectParameterPointer m_pColorParameter;

    DISALLOW_COPY_AND_ASSIGN(ColorCrushEffect);
};
