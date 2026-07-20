#include "effects/backends/builtin/colorcrusheffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "util/rampingvalue.h"
#include "util/sample.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 0.5;
constexpr double kMinimumHighPass = 20.0;
constexpr double kMaximumHighPass = 12000.0;
constexpr double kFilterQ = 0.707106781;

double highPassForAmount(double amount) {
    const double clampedAmount = std::clamp(amount, 0.0, 1.0);
    return std::exp(std::log(kMinimumHighPass) +
            clampedAmount * std::log(kMaximumHighPass / kMinimumHighPass));
}

CSAMPLE quantize(CSAMPLE sample, CSAMPLE bitDepth) {
    if (bitDepth >= 15.999f) {
        return sample;
    }
    const CSAMPLE scale = std::pow(2.0f, bitDepth) / 2.0f;
    const CSAMPLE gainCorrection = (17.0f - bitDepth) / 8.0f;
    return std::floor(SampleUtil::clampSample(sample * gainCorrection) * scale + 0.5f) /
            scale / gainCorrection;
}
} // namespace

ColorCrushGroupState::ColorCrushGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          heldLeft(0.0f),
          heldRight(0.0f),
          accumulator(1.0f),
          highPassCutoff(kMinimumHighPass),
          previousRightAmount(0.0),
          crushedBuffer(engineParameters.samplesPerBuffer()),
          filteredBuffer(engineParameters.samplesPerBuffer()),
          highPassFilter(std::make_unique<EngineFilterBiquad1High>(
                  engineParameters.sampleRate(), kMinimumHighPass, kFilterQ, true)) {
}

ColorCrushGroupState::~ColorCrushGroupState() = default;

QString ColorCrushEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colorcrush");
}

EffectManifestPointer ColorCrushEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Color Crush"));
    pManifest->setShortName(QObject::tr("Color Crush"));
    pManifest->setAuthor("The LastCUE Box Team");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "DJ mixer style crush: stronger distortion on the left, high-pass crush on the right"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Crush is off in the center; distortion increases left and high-pass filtering increases right"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);

    return pManifest;
}

void ColorCrushEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value(kColorParameterId);
}

void ColorCrushEffect::processChannel(
        ColorCrushGroupState* pState,
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
    const double targetRightAmount = std::max(0.0, 2.0 * (targetColor - kCenter));

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    const int channelCount = engineParameters.channelCount();
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); i += channelCount) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i);
        const CSAMPLE_GAIN bipolar = 2.0 * (color - kCenter);
        const CSAMPLE_GAIN leftAmount = std::max<CSAMPLE_GAIN>(0.0, -bipolar);
        const CSAMPLE_GAIN rightAmount = std::max<CSAMPLE_GAIN>(0.0, bipolar);
        const CSAMPLE_GAIN amount = leftAmount + rightAmount;

        // Counter-clockwise emphasizes destruction. Clockwise uses a milder
        // crush before the high-pass filter, matching DJ mixer Color FX behavior.
        const CSAMPLE bitDepth = 16.0f -
                12.0f * leftAmount - 8.0f * rightAmount;
        const CSAMPLE downsample = std::exp(
                std::log(0.08f) * leftAmount + std::log(0.25f) * rightAmount);
        pState->accumulator += downsample;
        if (pState->accumulator >= 1.0f) {
            pState->accumulator -= 1.0f;
            pState->heldLeft = quantize(pInput[i], bitDepth);
            pState->heldRight = channelCount > 1
                    ? quantize(pInput[i + 1], bitDepth)
                    : pState->heldLeft;
        }

        pState->crushedBuffer[i] =
                pInput[i] + (pState->heldLeft - pInput[i]) * amount;
        if (channelCount > 1) {
            pState->crushedBuffer[i + 1] =
                    pInput[i + 1] + (pState->heldRight - pInput[i + 1]) * amount;
        }
    }

    const double targetCutoff = highPassForAmount(targetRightAmount);
    if (targetRightAmount > 0.0) {
        if (pState->highPassCutoff != targetCutoff) {
            pState->highPassFilter->setFrequencyCorners(
                    engineParameters.sampleRate(), targetCutoff, kFilterQ);
            pState->highPassCutoff = targetCutoff;
        }
        pState->highPassFilter->process(pState->crushedBuffer.data(),
                pState->filteredBuffer.data(),
                engineParameters.samplesPerBuffer());
    } else if (pState->previousRightAmount > 0.0) {
        pState->highPassFilter->processAndPauseFilter(pState->crushedBuffer.data(),
                pState->filteredBuffer.data(),
                engineParameters.samplesPerBuffer());
        pState->highPassCutoff = kMinimumHighPass;
    } else {
        SampleUtil::copy(pState->filteredBuffer.data(),
                pState->crushedBuffer.data(),
                engineParameters.samplesPerBuffer());
    }

    // The HPF itself ramps coefficients, while this blend makes the center a
    // guaranteed bit-identical dry point and prevents clicks around 12 o'clock.
    RampingValue<CSAMPLE_GAIN> outputColorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const CSAMPLE_GAIN rightAmount =
                std::max<CSAMPLE_GAIN>(0.0, 2.0 * (outputColorRamp.getNth(i) - kCenter));
        pOutput[i] = pState->crushedBuffer[i] +
                (pState->filteredBuffer[i] - pState->crushedBuffer[i]) * rightAmount;
    }

    pState->previousColor = targetColor;
    pState->previousRightAmount = targetRightAmount;
}
