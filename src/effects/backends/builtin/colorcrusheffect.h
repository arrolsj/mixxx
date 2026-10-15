#pragma once

#include "effects/backends/effectprocessor.h"
#include "effects/backends/builtin/performancefxdsp.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class ColorCrushGroupState final : public EffectState {
  public:
    explicit ColorCrushGroupState(const mixxx::EngineParameters& engineParameters);
    int color = 128;
    int countdown = 0;
    int filterCode = -1;
    double sampleRate = 0;
    float phase = 0;
    std::array<float, 2> held{};
    performance_fx::Filter highPass;
    performance_fx::Ramp wet;
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
