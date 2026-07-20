#include "effects/backends/builtin/colordubechoeffect.h"

#include <algorithm>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/rampingvalue.h"
#include "util/sample.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 0.5;
constexpr double kCenterDelayBeats = 0.75;
constexpr double kLongestDelayBeats = 1.5;
constexpr double kShortestDelayBeats = 0.25;
constexpr double kFallbackSecondsPerBeat = 0.5;
constexpr CSAMPLE_GAIN kSendGain = 0.95f;
constexpr CSAMPLE_GAIN kFeedbackGain = 0.72f;
constexpr CSAMPLE_GAIN kMaximumWetGain = 0.85f;

void incrementRing(int* pIndex, int increment, int length) {
    *pIndex = (*pIndex + increment) % length;
}

void decrementRing(int* pIndex, int decrement, int length) {
    *pIndex = (*pIndex + length - decrement) % length;
}

double delayBeatsForColor(double color) {
    const double clampedColor = std::clamp(color, 0.0, 1.0);
    if (clampedColor < kCenter) {
        const double leftAmount = 2.0 * (kCenter - clampedColor);
        return kCenterDelayBeats +
                leftAmount * (kLongestDelayBeats - kCenterDelayBeats);
    }
    const double rightAmount = 2.0 * (clampedColor - kCenter);
    return kCenterDelayBeats -
            rightAmount * (kCenterDelayBeats - kShortestDelayBeats);
}
} // namespace

constexpr int ColorDubEchoGroupState::kMaxDelaySeconds;

ColorDubEchoGroupState::ColorDubEchoGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          previousDelaySamples(0),
          writePosition(0),
          delayBuffer(kMaxDelaySeconds * engineParameters.sampleRate() *
                  engineParameters.channelCount()) {
    delayBuffer.clear();
}

ColorDubEchoGroupState::~ColorDubEchoGroupState() = default;

QString ColorDubEchoEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colordubecho");
}

EffectManifestPointer ColorDubEchoEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Color Dub Echo"));
    pManifest->setShortName(QObject::tr("Color Dub Echo"));
    pManifest->setAuthor("The LastCUE Box Team");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "DJ mixer style dub echo: longer repeats on the left and shorter repeats on the right"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Echo is off in the center; turn left for a longer delay or right for a shorter delay"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);

    return pManifest;
}

void ColorDubEchoEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value(kColorParameterId);
}

void ColorDubEchoEffect::processChannel(
        ColorDubEchoGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const auto requestedColor = m_pColorParameter
            ? std::clamp(m_pColorParameter->value(), 0.0, 1.0)
            : kCenter;
    const auto targetColor = enableState == EffectEnableState::Disabling
            ? kCenter
            : requestedColor;
    if (targetColor == kCenter && pState->previousColor == kCenter) {
        SampleUtil::copy(
                pOutput, pInput, engineParameters.samplesPerBuffer());
        return;
    }

    const double delayBeats = delayBeatsForColor(targetColor);
    double delaySeconds = delayBeats * kFallbackSecondsPerBeat;
    if (groupFeatures.beat_length.has_value()) {
        delaySeconds = delayBeats * groupFeatures.beat_length->seconds;
    }
    const int channelCount = engineParameters.channelCount();
    int delaySamples = static_cast<int>(delaySeconds *
            engineParameters.sampleRate()) *
            channelCount;
    delaySamples = std::clamp(
            delaySamples,
            channelCount,
            static_cast<int>(pState->delayBuffer.size()));

    int previousReadPosition = pState->writePosition;
    decrementRing(&previousReadPosition,
            pState->previousDelaySamples,
            pState->delayBuffer.size());
    int readPosition = pState->writePosition;
    decrementRing(&readPosition, delaySamples, pState->delayBuffer.size());

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.framesPerBuffer());
    int frame = 0;
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); i += channelCount) {
        const CSAMPLE_GAIN color = colorRamp.getNth(frame++);
        const CSAMPLE_GAIN bipolar = 2.0 * (color - kCenter);
        const CSAMPLE_GAIN leftAmount = std::max<CSAMPLE_GAIN>(0.0, -bipolar);
        const CSAMPLE_GAIN rightAmount = std::max<CSAMPLE_GAIN>(0.0, bipolar);
        const CSAMPLE_GAIN amount = leftAmount + rightAmount;
        const CSAMPLE_GAIN delayCrossfade =
                static_cast<CSAMPLE_GAIN>(i) /
                engineParameters.samplesPerBuffer();

        for (int channel = 0; channel < channelCount; ++channel) {
            CSAMPLE bufferedSample = pState->delayBuffer[readPosition + channel];
            if (readPosition != previousReadPosition) {
                bufferedSample = bufferedSample * delayCrossfade +
                        pState->delayBuffer[previousReadPosition + channel] *
                                (1.0f - delayCrossfade);
            }
            pState->delayBuffer[pState->writePosition + channel] =
                    SampleUtil::clampSample(pInput[i + channel] * amount * kSendGain +
                            bufferedSample * kFeedbackGain);
            pOutput[i + channel] = pInput[i + channel] +
                    bufferedSample * amount * kMaximumWetGain;
        }

        if (readPosition != previousReadPosition) {
            incrementRing(&previousReadPosition,
                    channelCount,
                    pState->delayBuffer.size());
        }
        incrementRing(&readPosition, channelCount, pState->delayBuffer.size());
        incrementRing(&pState->writePosition,
                channelCount,
                pState->delayBuffer.size());
    }

    pState->previousColor = targetColor;
    pState->previousDelaySamples = delaySamples;
    if (targetColor == kCenter) {
        pState->delayBuffer.clear();
        pState->previousDelaySamples = 0;
    }
}
