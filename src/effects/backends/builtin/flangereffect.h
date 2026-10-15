#pragma once

#include <QMap>

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/rampingvalue.h"
#include "util/sample.h"
#include "util/samplebuffer.h"
#include "util/types.h"

namespace {
constexpr double kMaxDelayMs = 2.6666667461395264;
constexpr double kMinDelayMs = 0.0;
constexpr double kCenterDelayMs = (kMaxDelayMs - kMinDelayMs) / 2 + kMinDelayMs;
constexpr double kMaxLfoWidthMs = kMaxDelayMs - kMinDelayMs;
constexpr double kMinLfoBeats = 1 / 4.0;
constexpr double kMaxLfoBeats = 32.0;
} // anonymous namespace

struct FlangerGroupState : public EffectState {
    FlangerGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              delayLeft(static_cast<int>((kMaxDelayMs + 1.0) * engineParameters.sampleRate() / 1000)),
              delayRight(delayLeft.size()),
              delayPos(0),
              lfoFrames(0),
              previousPeriodFrames(-1),
              prev_regen(0),
              prev_mix(0),
              prev_width(0),
              prev_manual(static_cast<CSAMPLE_GAIN>(kCenterDelayMs)) {
        delayLeft.clear();
        delayRight.clear();
    }
    ~FlangerGroupState() override = default;

    mixxx::SampleBuffer delayLeft;
    mixxx::SampleBuffer delayRight;
    unsigned int delayPos;
    unsigned int lfoFrames;
    double previousPeriodFrames;
    CSAMPLE_GAIN prev_regen;
    CSAMPLE_GAIN prev_mix;
    CSAMPLE_GAIN prev_width;
    CSAMPLE_GAIN prev_manual;
    double targetWidth = -1;
    double targetManual = -1;
    double widthStep = 0;
    double manualStep = 0;
    int widthRemaining = 0;
    int manualRemaining = 0;
    float outputGain = 1;
};

class FlangerEffect : public EffectProcessorImpl<FlangerGroupState> {
  public:
    FlangerEffect() = default;
    ~FlangerEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            FlangerGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pSpeedParameter;
    EngineEffectParameterPointer m_pWidthParameter;
    EngineEffectParameterPointer m_pManualParameter;
    EngineEffectParameterPointer m_pRegenParameter;
    EngineEffectParameterPointer m_pMixParameter;
    EngineEffectParameterPointer m_pTripletParameter;

    DISALLOW_COPY_AND_ASSIGN(FlangerEffect);
};
