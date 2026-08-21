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
constexpr double kMaximumHighPass = 6000.0;
constexpr double kFilterQ = 0.707106781;
constexpr int kCrushStart = 144;

double highPassForAmount(double amount) {
    const double clampedAmount = std::clamp(amount, 0.0, 1.0);
    return std::exp(std::log(kMinimumHighPass) +
            clampedAmount * std::log(kMaximumHighPass / kMinimumHighPass));
}

CSAMPLE applySteppedMask(CSAMPLE sample, int color) {
    if (color <= kCrushStart) {
        return sample;
    }
    // Rekordbox advances the mask in seven discrete steps rather than
    // continuously reducing bit depth.
    const int discardedBits = std::clamp(((8 * color - 1024) >> 7), 1, 7);
    const int bitDepth = 16 - discardedBits;
    const CSAMPLE scale = static_cast<CSAMPLE>(1 << (bitDepth - 1));
    return std::round(SampleUtil::clampSample(sample) * scale) / scale;
}
} // namespace

ColorCrushGroupState::ColorCrushGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          highPassCutoff(kMinimumHighPass),
          previousLeftAmount(0.0),
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
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "Rekordbox-style crush with high-pass color and stepped bit masking"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "High-pass filtering increases left; stepped crushing increases right"));
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
    const int targetColorByte = static_cast<int>(std::round(targetColor * 255.0));
    const double targetLeftAmount = targetColorByte < 124
            ? (124.0 - targetColorByte) / 124.0
            : 0.0;

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    const int channelCount = engineParameters.channelCount();
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); i += channelCount) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i);
        const int value = static_cast<int>(std::round(color * 255.0));
        const CSAMPLE_GAIN rightAmount = value > kCrushStart
                ? static_cast<CSAMPLE_GAIN>(value - kCrushStart) /
                        (255 - kCrushStart)
                : 0.0f;
        for (int channel = 0; channel < channelCount; ++channel) {
            const CSAMPLE dry = pInput[i + channel];
            const CSAMPLE masked = applySteppedMask(dry, value);
            pState->crushedBuffer[i + channel] =
                    dry + (masked - dry) * rightAmount;
        }
    }

    const double targetCutoff = highPassForAmount(targetLeftAmount);
    if (targetLeftAmount > 0.0) {
        if (pState->highPassCutoff != targetCutoff) {
            pState->highPassFilter->setFrequencyCorners(
                    engineParameters.sampleRate(), targetCutoff, kFilterQ);
            pState->highPassCutoff = targetCutoff;
        }
        pState->highPassFilter->process(pInput,
                pState->filteredBuffer.data(),
                engineParameters.samplesPerBuffer());
    } else if (pState->previousLeftAmount > 0.0) {
        pState->highPassFilter->processAndPauseFilter(pInput,
                pState->filteredBuffer.data(),
                engineParameters.samplesPerBuffer());
        pState->highPassCutoff = kMinimumHighPass;
    } else {
        SampleUtil::copy(pState->filteredBuffer.data(), pInput,
                engineParameters.samplesPerBuffer());
    }

    RampingValue<CSAMPLE_GAIN> outputColorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const double value = outputColorRamp.getNth(i) * 255.0;
        const CSAMPLE_GAIN leftAmount = value < 124.0
                ? static_cast<CSAMPLE_GAIN>((124.0 - value) / 124.0)
                : 0.0f;
        pOutput[i] = pState->crushedBuffer[i] +
                (pState->filteredBuffer[i] - pState->crushedBuffer[i]) * leftAmount;
    }

    pState->previousColor = targetColor;
    pState->previousLeftAmount = targetLeftAmount;
}
