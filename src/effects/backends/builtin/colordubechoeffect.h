#pragma once

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class ColorDubEchoGroupState final : public EffectState {
  public:
    static constexpr int kMaxDelaySeconds = 3;

    explicit ColorDubEchoGroupState(
            const mixxx::EngineParameters& engineParameters);
    ~ColorDubEchoGroupState() override;

    CSAMPLE_GAIN previousColor;
    int previousDelaySamples;
    int writePosition;
    mixxx::SampleBuffer delayBuffer;
    mixxx::SampleBuffer lowPassState;
    mixxx::SampleBuffer highPassInputState;
    mixxx::SampleBuffer highPassOutputState;
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
