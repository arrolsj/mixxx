#include "effects/backends/builtin/colornoiseeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "util/rampingvalue.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 0.5;
constexpr double kMinimumLowPass = 300.0;
constexpr double kMaximumLowPass = 16000.0;
constexpr double kMinimumHighPass = 40.0;
constexpr double kMaximumHighPass = 5000.0;
constexpr double kFilterQ = 0.707106781;
constexpr CSAMPLE_GAIN kMaximumNoiseGain = 0.28;

double logarithmicRange(double minimum, double maximum, double amount) {
    return std::exp(std::log(minimum) +
            std::clamp(amount, 0.0, 1.0) * std::log(maximum / minimum));
}

double leftDistance(double color) {
    const double value = std::clamp(color, 0.0, 1.0) * 255.0;
    return value < 124.0 ? (124.0 - value) / 124.0 : 0.0;
}

double rightDistance(double color) {
    const double value = std::clamp(color, 0.0, 1.0) * 255.0;
    return value > 131.0 ? (value - 131.0) / 124.0 : 0.0;
}

double nonlinearToneGain(double distance) {
    const double square = distance * distance;
    const double fourth = square * square;
    const double eighth = fourth * fourth;
    return std::min(1.0, 0.7 + 0.1 * eighth);
}
} // namespace

ColorNoiseGroupState::ColorNoiseGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          lowPassCutoff(kMaximumLowPass),
          highPassCutoff(kMinimumHighPass),
          noiseBuffer(engineParameters.samplesPerBuffer()),
          lowPassBuffer(engineParameters.samplesPerBuffer()),
          highPassBuffer(engineParameters.samplesPerBuffer()),
          generator(std::random_device{}()),
          lowPassFilter(std::make_unique<EngineFilterBiquad1Low>(
                  engineParameters.sampleRate(), lowPassCutoff, kFilterQ, false)),
          highPassFilter(std::make_unique<EngineFilterBiquad1High>(
                  engineParameters.sampleRate(), highPassCutoff, kFilterQ, false)) {
}

ColorNoiseGroupState::~ColorNoiseGroupState() = default;

QString ColorNoiseEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colornoise");
}

EffectManifestPointer ColorNoiseEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Color Noise"));
    pManifest->setShortName(QObject::tr("Color Noise"));
    pManifest->setAuthor("The LastCUE Box Team");
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "Rekordbox-style noise with separate low-pass and high-pass color paths"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Noise is off in the center; cutoff moves from low to high frequencies"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);

    return pManifest;
}

void ColorNoiseEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value(kColorParameterId);
}

void ColorNoiseEffect::processChannel(
        ColorNoiseGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);

    const auto requestedColor = m_pColorParameter
            ? std::clamp(m_pColorParameter->value(), 0.0, 1.0)
            : kCenter;
    const auto targetColor = enableState == EffectEnableState::Disabling
            ? kCenter
            : requestedColor;
    const double targetLeftDistance = leftDistance(targetColor);
    const double targetRightDistance = rightDistance(targetColor);
    const double targetLowPassCutoff = logarithmicRange(
            kMaximumLowPass, kMinimumLowPass, targetLeftDistance);
    const double targetHighPassCutoff = logarithmicRange(
            kMinimumHighPass, kMaximumHighPass, targetRightDistance);
    if (pState->lowPassCutoff != targetLowPassCutoff) {
        pState->lowPassFilter->setFrequencyCorners(
                engineParameters.sampleRate(), targetLowPassCutoff, kFilterQ);
        pState->lowPassCutoff = targetLowPassCutoff;
    }
    if (pState->highPassCutoff != targetHighPassCutoff) {
        pState->highPassFilter->setFrequencyCorners(
                engineParameters.sampleRate(), targetHighPassCutoff, kFilterQ);
        pState->highPassCutoff = targetHighPassCutoff;
    }

    std::uniform_real_distribution<CSAMPLE> noiseDistribution(-1.0f, 1.0f);
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        pState->noiseBuffer[i] = noiseDistribution(pState->generator);
    }
    pState->lowPassFilter->process(pState->noiseBuffer.data(),
            pState->lowPassBuffer.data(),
            engineParameters.samplesPerBuffer());
    pState->highPassFilter->process(pState->noiseBuffer.data(),
            pState->highPassBuffer.data(),
            engineParameters.samplesPerBuffer());

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i);
        const CSAMPLE_GAIN leftAmount = leftDistance(color);
        const CSAMPLE_GAIN rightAmount = rightDistance(color);
        const CSAMPLE_GAIN amount = leftAmount + rightAmount;
        const CSAMPLE filteredNoise = leftAmount > 0.0f
                ? pState->lowPassBuffer[i]
                : pState->highPassBuffer[i];
        pOutput[i] = pInput[i] + filteredNoise * amount *
                nonlinearToneGain(amount) * kMaximumNoiseGain;
    }

    pState->previousColor = targetColor;
}
