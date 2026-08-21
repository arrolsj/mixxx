#pragma once

#include <memory>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1High;
class EngineFilterBiquad1Low;

class ColorFilterGroupState final : public EffectState {
  public:
    explicit ColorFilterGroupState(const mixxx::EngineParameters& engineParameters);
    ~ColorFilterGroupState() override;

    CSAMPLE_GAIN previousColor;
    double lowPassCutoff;
    double highPassCutoff;
    bool lowPassActive;
    bool highPassActive;
    mixxx::SampleBuffer lowPassBuffer;
    mixxx::SampleBuffer highPassBuffer;
    std::unique_ptr<EngineFilterBiquad1Low> lowPassFilter;
    std::unique_ptr<EngineFilterBiquad1High> highPassFilter;
};

class ColorFilterEffect : public EffectProcessorImpl<ColorFilterGroupState> {
  public:
    ColorFilterEffect() = default;
    ~ColorFilterEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void processChannel(
            ColorFilterGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

  private:
    EngineEffectParameterPointer m_pColorParameter;

    DISALLOW_COPY_AND_ASSIGN(ColorFilterEffect);
};
