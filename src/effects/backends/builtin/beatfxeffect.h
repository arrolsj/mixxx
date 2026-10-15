#pragma once

#include "effects/backends/effectprocessor.h"
#include "effects/backends/builtin/performancefxdsp.h"
#include "util/samplebuffer.h"

class BeatDelayState : public EffectState {
  public:
    explicit BeatDelayState(const mixxx::EngineParameters& p);
    bool hasTail() const override { return tailFrames>0; }
    mixxx::SampleBuffer history;
    int write=0, delay=0, requested=0, phase=0, fadeRemaining=0, tailFrames=0;
    float fade=1;
    performance_fx::Ramp inputGain, dry, wet;
    performance_fx::DcBlock dc;
};

class BeatEchoEffect : public EffectProcessorImpl<BeatDelayState> {
  public:
    BeatEchoEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    bool processesWhileDisabled() const override { return true; }
    void loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) override;
    void processChannel(BeatDelayState*, const CSAMPLE*, CSAMPLE*,
            const mixxx::EngineParameters&, EffectEnableState, const GroupFeatureState&) override;
  protected:
    explicit BeatEchoEffect(bool echo) : m_echo(echo) {}
  private:
    bool m_echo=true;
    EngineEffectParameterPointer m_time, m_depth;
};

class BeatDelayEffect final : public BeatEchoEffect {
  public:
    BeatDelayEffect() : BeatEchoEffect(false) {}
    static QString getId();
    static EffectManifestPointer getManifest();
};

class SpiralState : public EffectState {
  public:
    explicit SpiralState(const mixxx::EngineParameters& p);
    bool hasTail() const override { return tailFrames>0; }
    mixxx::SampleBuffer history, pitch;
    int write=0, pitchWrite=0, tailFrames=0;
    double fs=0, delay=0, velocity=0, acceleration=0, desiredVelocity=0;
    float jitter=0;
    bool jitterUp=true;
    float peak=0;
    performance_fx::Ramp inputGain,dry,wet;
    performance_fx::Filter pre,feedbackHigh,feedbackLow,outputLow;
    performance_fx::DcBlock dc;
};

class SpiralEffect final : public EffectProcessorImpl<SpiralState> {
  public:
    static QString getId();
    static EffectManifestPointer getManifest();
    bool processesWhileDisabled() const override { return true; }
    void loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) override;
    void processChannel(SpiralState*, const CSAMPLE*, CSAMPLE*,
            const mixxx::EngineParameters&, EffectEnableState, const GroupFeatureState&) override;
  private:
    EngineEffectParameterPointer m_time,m_depth;
};

class RollState : public EffectState {
  public:
    explicit RollState(const mixxx::EngineParameters& p);
    bool hasTail() const override { return active && capturedPeak>1e-8f; }
    mixxx::SampleBuffer history,loop;
    int write=0, read=0, length=1, requested=1, wait=0;
    bool active=false, wasEnabled=false;
    float capturedPeak=0;
    performance_fx::Ramp wet;
};

class RollEffect final : public EffectProcessorImpl<RollState> {
  public:
    static QString getId();
    static EffectManifestPointer getManifest();
    bool processesWhileDisabled() const override { return true; }
    void loadEngineEffectParameters(const QMap<QString,EngineEffectParameterPointer>& p) override;
    void processChannel(RollState*, const CSAMPLE*, CSAMPLE*,
            const mixxx::EngineParameters&, EffectEnableState, const GroupFeatureState&) override;
  private:
    EngineEffectParameterPointer m_time,m_depth;
};
