#include "effects/backends/builtin/flangereffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {

constexpr float kDefaultFeedback = 0.266666591f;
} // namespace

// static
QString FlangerEffect::getId() {
    return "org.mixxx.effects.flanger";
}

// static
EffectManifestPointer FlangerEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Flanger"));
    pManifest->setShortName(QObject::tr("Flanger"));
    pManifest->setAuthor("The Mixxx Team");
    pManifest->setVersion("2.0");
    pManifest->setDescription(
            QObject::tr("Mixes the input with a delayed, pitch modulated copy "
                        "of itself to create comb filtering"));

    EffectManifestParameterPointer speed = pManifest->addParameter();
    speed->setId("speed");
    speed->setName(QObject::tr("Speed"));
    speed->setShortName(QObject::tr("Speed"));
    speed->setDescription(QObject::tr(
            "Speed of the LFO (low frequency oscillator)\n"
            "32 - 1/4 beats per triangle LFO cycle if tempo is detected\n"
            "1/32 - 4 Hz if no tempo is detected"));
    speed->setValueScaler(EffectManifestParameter::ValueScaler::LogarithmicInverse);
    speed->setRange(kMinLfoBeats, 8, kMaxLfoBeats);

    EffectManifestParameterPointer width = pManifest->addParameter();
    width->setId("width");
    width->setName(QObject::tr("Width"));
    width->setShortName(QObject::tr("Width"));
    width->setDescription(QObject::tr(
            "Delay amplitude of the LFO (low frequency oscillator)"));
    width->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    width->setUnitsHint(EffectManifestParameter::UnitsHint::Millisecond);
    width->setRange(0.0, kMaxLfoWidthMs, kMaxLfoWidthMs);

    EffectManifestParameterPointer manual = pManifest->addParameter();
    manual->setId("manual");
    manual->setName(QObject::tr("Manual"));
    manual->setShortName(QObject::tr("Manual"));
    manual->setDescription(QObject::tr(
            "Delay offset of the LFO (low frequency oscillator).\n"
            "With width at zero, this allows for manually sweeping over the entire delay range."));
    manual->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    manual->setUnitsHint(EffectManifestParameter::UnitsHint::Millisecond);
    manual->setRange(kMinDelayMs, kCenterDelayMs, kMaxDelayMs);

    EffectManifestParameterPointer regen = pManifest->addParameter();
    regen->setId("regen");
    regen->setName(QObject::tr("Regeneration"));
    regen->setShortName(QObject::tr("Regen"));
    regen->setDescription(QObject::tr(
            "How much of the delay output is feed back into the input"));
    regen->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    regen->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    regen->setRange(0.0, kDefaultFeedback, 0.93);

    EffectManifestParameterPointer mix = pManifest->addParameter();
    mix->setId("mix");
    mix->setName(QObject::tr("Mix"));
    mix->setShortName(QObject::tr("Mix"));
    mix->setDescription(QObject::tr(
            "Intensity of the effect"));
    mix->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    mix->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    mix->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    mix->setRange(0.0, 1.0, 1.0);

    EffectManifestParameterPointer triplet = pManifest->addParameter();
    triplet->setId("triplet");
    triplet->setName(QObject::tr("Triplets"));
    triplet->setShortName(QObject::tr("Triplets"));
    triplet->setDescription(QObject::tr(
            "Divide rounded 1/2 beats of the Period parameter by 3."));
    triplet->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    triplet->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    triplet->setRange(0, 0, 1);

    return pManifest;
}

void FlangerEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pSpeedParameter = parameters.value("speed");
    m_pWidthParameter = parameters.value("width");
    m_pManualParameter = parameters.value("manual");
    m_pRegenParameter = parameters.value("regen");
    m_pMixParameter = parameters.value("mix");
    m_pTripletParameter = parameters.value("triplet");
}

void FlangerEffect::processChannel(
        FlangerGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    double lfoPeriodParameter = m_pSpeedParameter->value();
    double lfoPeriodFrames;
    if (groupFeatures.beat_length.has_value()) {
        // lfoPeriodParameter is a number of beats
        lfoPeriodParameter = std::max(lfoPeriodParameter, kMinLfoBeats);
        if (m_pTripletParameter->toBool()) {
            lfoPeriodParameter /= 3.0;
        }
        lfoPeriodFrames = lfoPeriodParameter *
                groupFeatures.beat_length->seconds *
                engineParameters.sampleRate();
    } else {
        // lfoPeriodParameter is a number of seconds
        lfoPeriodFrames = std::max(lfoPeriodParameter, kMinLfoBeats) *
                engineParameters.sampleRate();
    }

    lfoPeriodFrames = std::clamp(lfoPeriodFrames,
            0.01 * engineParameters.sampleRate(), 32.0 * engineParameters.sampleRate());
    lfoPeriodFrames = std::max(1.0, std::floor(lfoPeriodFrames));

    // When the period is changed, the position of the sound shouldn't
    // so time need to be recalculated
    if (pState->previousPeriodFrames != -1.0) {
        pState->lfoFrames = static_cast<unsigned int>(
                lfoPeriodFrames / pState->previousPeriodFrames * pState->lfoFrames);
    }
    pState->previousPeriodFrames = lfoPeriodFrames;

    // lfoPeriodSamples is used to calculate the delay for each channel
    // independently in the loop below, so do not multiply lfoPeriodSamples by
    // the number of channels.

    const auto mix = static_cast<CSAMPLE_GAIN>(m_pMixParameter->value());
    RampingValue<CSAMPLE_GAIN> mixRamped(
            pState->prev_mix, mix, engineParameters.framesPerBuffer());
    pState->prev_mix = mix;

    const auto regen = static_cast<CSAMPLE_GAIN>(m_pRegenParameter->value());
    RampingValue<CSAMPLE_GAIN> regenRamped(
            pState->prev_regen, regen, engineParameters.framesPerBuffer());
    pState->prev_regen = regen;

    // With and Manual is limited by amount of amplitude that remains from width
    // to kMaxDelayMs
    double width = std::clamp(m_pWidthParameter->value(), 0.0, kMaxLfoWidthMs);
    double manual = m_pManualParameter->value();
    double maxManual = kCenterDelayMs + (kMaxLfoWidthMs - width) / 2;
    double minManual = kCenterDelayMs - (kMaxLfoWidthMs - width) / 2;
    manual = math_clamp(manual, minManual, maxManual);

    const int smoothFrames = std::max(1, static_cast<int>(0.05 * engineParameters.sampleRate()));
    if (pState->targetWidth != width) {
        pState->targetWidth = width;
        pState->widthStep = (width - pState->prev_width) / smoothFrames;
        pState->widthRemaining = smoothFrames;
    }
    if (pState->targetManual != manual) {
        pState->targetManual = manual;
        pState->manualStep = (manual - pState->prev_manual) / smoothFrames;
        pState->manualRemaining = smoothFrames;
    }

    CSAMPLE* delayLeft = pState->delayLeft.data();
    CSAMPLE* delayRight = pState->delayRight.data();
    const int bufferLength = pState->delayLeft.size();

    int rampIndex = 0;
    for (SINT i = 0;
            i < engineParameters.samplesPerBuffer();
            i += engineParameters.channelCount()) {
        CSAMPLE_GAIN mix_ramped = mixRamped.getNth(rampIndex);
        CSAMPLE_GAIN regen_ramped = regenRamped.getNth(rampIndex);
        if (pState->widthRemaining > 0) {
            pState->prev_width += pState->widthStep;
            if (--pState->widthRemaining == 0) pState->prev_width = pState->targetWidth;
        }
        if (pState->manualRemaining > 0) {
            pState->prev_manual += pState->manualStep;
            if (--pState->manualRemaining == 0) pState->prev_manual = pState->targetManual;
        }
        const double width_ramped = pState->prev_width;
        const double manual_ramped = pState->prev_manual;
        ++rampIndex;

        const auto halfPeriod = static_cast<unsigned int>(lfoPeriodFrames) / 2;
        const double triangle = (pState->lfoFrames < halfPeriod ? pState->lfoFrames :
                lfoPeriodFrames - pState->lfoFrames) * (4.0 / lfoPeriodFrames) - 1.0;
        const double delayMs = std::clamp(manual_ramped + width_ramped / 2 * triangle,
                1000.0 / engineParameters.sampleRate(), kMaxDelayMs);
        double delayFrames = delayMs * engineParameters.sampleRate() / 1000;

        SINT framePrev =
                (static_cast<SINT>(pState->delayPos) - static_cast<SINT>(floor(delayFrames)) +
                        bufferLength) %
                bufferLength;
        SINT frameNext =
                (static_cast<SINT>(pState->delayPos) - static_cast<SINT>(ceil(delayFrames)) +
                        bufferLength) %
                bufferLength;
        CSAMPLE prevLeft = delayLeft[framePrev];
        CSAMPLE nextLeft = delayLeft[frameNext];

        CSAMPLE prevRight = delayRight[framePrev];
        CSAMPLE nextRight = delayRight[frameNext];

        const CSAMPLE_GAIN frac = static_cast<CSAMPLE_GAIN>(
                delayFrames - std::floor(delayFrames));
        CSAMPLE delayedSampleLeft = prevLeft + frac * (nextLeft - prevLeft);
        CSAMPLE delayedSampleRight = prevRight + frac * (nextRight - prevRight);

        delayLeft[pState->delayPos] = pInput[i] + regen_ramped * delayedSampleLeft;
        delayRight[pState->delayPos] =
                pInput[i + 1] + regen_ramped * delayedSampleRight;

        pState->delayPos = (pState->delayPos + 1) % bufferLength;
        pState->lfoFrames = (pState->lfoFrames + 1) % static_cast<unsigned int>(lfoPeriodFrames);
        const float targetGain = delayMs < 0.5 ? 0.36860001f + 1.26279998f * delayMs : 1.0f;
        const float gainStep = 1.0f / std::max(1.0, 0.0033 * engineParameters.sampleRate());
        pState->outputGain += std::clamp(targetGain - pState->outputGain, -gainStep, gainStep);
        const float dry = mix_ramped < 0.25f ? 1.0f - mix_ramped : 0.75f;
        const float wet = mix_ramped < 0.25f ? 3.0f * mix_ramped : 0.75f;
        const float correction = regen_ramped > 0.8f ?
                1.0f - 3.3254192f * (regen_ramped - 0.8f) : 1.0f;
        pOutput[i] = correction * (dry * pInput[i] + wet * pState->outputGain * delayedSampleLeft);
        pOutput[i + 1] = correction * (dry * pInput[i + 1] + wet * pState->outputGain * delayedSampleRight);
    }

    if (enableState == EffectEnableState::Disabling) {
        pState->delayLeft.clear();
        pState->delayRight.clear();
        pState->previousPeriodFrames = -1;
        pState->prev_regen = 0;
        pState->prev_mix = 0;
        pState->lfoFrames = pState->delayPos = 0;
        pState->prev_width = 0;
        pState->prev_manual = kCenterDelayMs;
        pState->targetWidth = pState->targetManual = -1;
        pState->widthRemaining = pState->manualRemaining = 0;
        pState->outputGain = 1;
    }
}
