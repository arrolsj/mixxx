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
constexpr double kMinimumCutoff = 120.0;
constexpr double kMaximumCutoff = 16000.0;
constexpr double kFilterQ = 0.707106781;
// Keep the generated noise below the deck signal. DJM mixers expose a separate
// parameter knob for noise volume, while LastCUE intentionally uses one knob.
constexpr CSAMPLE_GAIN kMaximumNoiseGain = 0.22;

double cutoffForColor(double color) {
    const double clampedColor = std::clamp(color, 0.0, 1.0);
    return std::exp(std::log(kMinimumCutoff) +
            clampedColor * std::log(kMaximumCutoff / kMinimumCutoff));
}
} // namespace

ColorNoiseGroupState::ColorNoiseGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          cutoff(cutoffForColor(kCenter)),
          noiseBuffer(engineParameters.samplesPerBuffer()),
          filteredNoiseBuffer(engineParameters.samplesPerBuffer()),
          generator(std::random_device{}()),
          lowPassFilter(std::make_unique<EngineFilterBiquad1Low>(
                  engineParameters.sampleRate(), cutoff, kFilterQ, false)) {
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
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "DJ mixer style filtered noise: dark on the left, bright on the right"));
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
    const double targetCutoff = cutoffForColor(targetColor);
    if (pState->cutoff != targetCutoff) {
        pState->lowPassFilter->setFrequencyCorners(
                engineParameters.sampleRate(), targetCutoff, kFilterQ);
        pState->cutoff = targetCutoff;
    }

    std::uniform_real_distribution<CSAMPLE> noiseDistribution(-1.0f, 1.0f);
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        pState->noiseBuffer[i] = noiseDistribution(pState->generator);
    }
    pState->lowPassFilter->process(pState->noiseBuffer.data(),
            pState->filteredNoiseBuffer.data(),
            engineParameters.samplesPerBuffer());

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i);
        const CSAMPLE_GAIN amount = std::abs(2.0 * (color - kCenter));
        pOutput[i] = pInput[i] +
                pState->filteredNoiseBuffer[i] * amount * kMaximumNoiseGain;
    }

    pState->previousColor = targetColor;
}
