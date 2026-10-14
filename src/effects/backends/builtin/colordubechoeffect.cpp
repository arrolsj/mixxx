#include "effects/backends/builtin/colordubechoeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/rampingvalue.h"
#include "util/sample.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 0.5;
constexpr double kCenterDelayBeats = 0.75;
constexpr double kFallbackSecondsPerBeat = 0.5;
constexpr CSAMPLE_GAIN kSendGain = 0.9f;
constexpr CSAMPLE_GAIN kFeedbackGain = 0.68f;
constexpr CSAMPLE_GAIN kDelayedMix = 0.7f;
constexpr double kPi = 3.14159265358979323846;

void incrementRing(int* pIndex, int increment, int length) {
    *pIndex = (*pIndex + increment) % length;
}

void decrementRing(int* pIndex, int decrement, int length) {
    *pIndex = (*pIndex + length - decrement) % length;
}

double leftDistance(double color) {
    const double value = std::clamp(color, 0.0, 1.0) * 255.0;
    return value < 116.0 ? (116.0 - value) / 116.0 : 0.0;
}

double rightDistance(double color) {
    const double value = std::clamp(color, 0.0, 1.0) * 255.0;
    return value > 138.0 ? std::max(0.0, (value - 139.0) / 116.0) : 0.0;
}

double filterMix(double distance) {
    if (distance <= 0.2) {
        return 0.0;
    }
    return std::clamp((distance - 0.2) / 0.1, 0.0, 1.0);
}

double logarithmicRange(double minimum, double maximum, double amount) {
    return std::exp(std::log(minimum) +
            std::clamp(amount, 0.0, 1.0) * std::log(maximum / minimum));
}

struct ColorCoefficients {
    CSAMPLE_GAIN leftAmount;
    CSAMPLE_GAIN rightAmount;
    CSAMPLE_GAIN amount;
    CSAMPLE_GAIN directionFilterMix;
    double lowPassAlpha;
    double highPassAlpha;
};

ColorCoefficients coefficientsForColor(CSAMPLE_GAIN color, double sampleRate) {
    ColorCoefficients coefficients;
    coefficients.leftAmount = leftDistance(color);
    coefficients.rightAmount = rightDistance(color);
    coefficients.amount = coefficients.leftAmount + coefficients.rightAmount;
    const double directionDistance =
            std::max(coefficients.leftAmount, coefficients.rightAmount);
    coefficients.directionFilterMix = filterMix(directionDistance);
    const double lowPassCutoff =
            logarithmicRange(8000.0, 700.0, coefficients.leftAmount);
    coefficients.lowPassAlpha =
            1.0 - std::exp(-2.0 * kPi * lowPassCutoff / sampleRate);
    const double highPassCutoff =
            logarithmicRange(80.0, 2600.0, coefficients.rightAmount);
    const double highPassRc = 1.0 / (2.0 * kPi * highPassCutoff);
    coefficients.highPassAlpha = highPassRc / (highPassRc + 1.0 / sampleRate);
    return coefficients;
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
                  engineParameters.channelCount()),
          lowPassState(engineParameters.channelCount()),
          highPassInputState(engineParameters.channelCount()),
          highPassOutputState(engineParameters.channelCount()) {
    delayBuffer.clear();
    lowPassState.clear();
    highPassInputState.clear();
    highPassOutputState.clear();
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
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "Rekordbox-style dub echo with direction-dependent filtering"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Echo is off in the center; left darkens and right thins the repeats"));
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

    double delaySeconds = kCenterDelayBeats * kFallbackSecondsPerBeat;
    if (groupFeatures.beat_length.has_value()) {
        delaySeconds = kCenterDelayBeats * groupFeatures.beat_length->seconds;
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
    // The coefficients cost several exp/log calls. They only change while the
    // color ramps, so a steady knob computes them once per buffer.
    const double sampleRate = engineParameters.sampleRate();
    CSAMPLE_GAIN coefficientColor = colorRamp.getNth(0);
    ColorCoefficients coefficients =
            coefficientsForColor(coefficientColor, sampleRate);
    int frame = 0;
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); i += channelCount) {
        const CSAMPLE_GAIN color = colorRamp.getNth(frame++);
        if (color != coefficientColor) {
            coefficientColor = color;
            coefficients = coefficientsForColor(color, sampleRate);
        }
        const CSAMPLE_GAIN leftAmount = coefficients.leftAmount;
        const CSAMPLE_GAIN amount = coefficients.amount;
        const CSAMPLE_GAIN directionFilterMix = coefficients.directionFilterMix;
        const double lowPassAlpha = coefficients.lowPassAlpha;
        const double highPassAlpha = coefficients.highPassAlpha;
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
            pState->lowPassState[channel] += static_cast<CSAMPLE>(
                    lowPassAlpha * (bufferedSample - pState->lowPassState[channel]));
            const CSAMPLE highPassed = static_cast<CSAMPLE>(highPassAlpha *
                    (pState->highPassOutputState[channel] + bufferedSample -
                            pState->highPassInputState[channel]));
            pState->highPassInputState[channel] = bufferedSample;
            pState->highPassOutputState[channel] = highPassed;
            const CSAMPLE directionFiltered = leftAmount > 0.0f
                    ? pState->lowPassState[channel]
                    : highPassed;
            const CSAMPLE coloredDelay = bufferedSample +
                    (directionFiltered - bufferedSample) * directionFilterMix;
            pState->delayBuffer[pState->writePosition + channel] =
                    SampleUtil::clampSample(pInput[i + channel] * amount * kSendGain +
                            coloredDelay * kFeedbackGain);
            pOutput[i + channel] = pInput[i + channel] +
                    coloredDelay * amount * kDelayedMix;
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
        pState->lowPassState.clear();
        pState->highPassInputState.clear();
        pState->highPassOutputState.clear();
        pState->previousDelaySamples = 0;
    }
}
