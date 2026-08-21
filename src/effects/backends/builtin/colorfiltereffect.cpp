#include "effects/backends/builtin/colorfiltereffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "util/rampingvalue.h"
#include "util/sample.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 128.0 / 255.0;
constexpr double kMinimumLowPass = 70.0;
constexpr double kMaximumLowPass = 18000.0;
constexpr double kMinimumHighPass = 20.0;
constexpr double kMaximumHighPass = 12000.0;
constexpr double kFilterQ = 1.1;

double logarithmicRange(double minimum, double maximum, double amount) {
    return std::exp(std::log(minimum) +
            std::clamp(amount, 0.0, 1.0) * std::log(maximum / minimum));
}

double colorByte(double color) {
    return std::clamp(color, 0.0, 1.0) * 255.0;
}

double lowPassGain(double color) {
    const double value = colorByte(color);
    if (value <= 109.0) {
        return 1.0;
    }
    if (value <= 125.0) {
        return std::clamp(1.0 - (value - 110.0) / 16.0, 0.0, 1.0);
    }
    return 0.0;
}

double highPassGain(double color) {
    const double value = colorByte(color);
    if (value < 131.0) {
        return 0.0;
    }
    if (value <= 146.0) {
        return (value - 130.0) / 16.0;
    }
    return 1.0;
}

double lowPassCutoff(double color) {
    return logarithmicRange(kMinimumLowPass,
            kMaximumLowPass,
            colorByte(color) / 125.0);
}

double highPassCutoff(double color) {
    return logarithmicRange(kMinimumHighPass,
            kMaximumHighPass,
            (colorByte(color) - 131.0) / 124.0);
}
} // namespace

ColorFilterGroupState::ColorFilterGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          lowPassCutoff(kMaximumLowPass),
          highPassCutoff(kMinimumHighPass),
          lowPassActive(false),
          highPassActive(false),
          lowPassBuffer(engineParameters.samplesPerBuffer()),
          highPassBuffer(engineParameters.samplesPerBuffer()),
          lowPassFilter(std::make_unique<EngineFilterBiquad1Low>(
                  engineParameters.sampleRate(), lowPassCutoff, kFilterQ, true)),
          highPassFilter(std::make_unique<EngineFilterBiquad1High>(
                  engineParameters.sampleRate(), highPassCutoff, kFilterQ, true)) {
}

ColorFilterGroupState::~ColorFilterGroupState() = default;

QString ColorFilterEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colorfilter");
}

EffectManifestPointer ColorFilterEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Color Filter"));
    pManifest->setShortName(QObject::tr("Color Filter"));
    pManifest->setAuthor("The LastCUE Box Team");
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "Rekordbox-style low-pass, dry, and high-pass Color FX routing"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(kCenter);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Low-pass on the left, dry in the center, high-pass on the right"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);
    return pManifest;
}

void ColorFilterEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value(kColorParameterId);
}

void ColorFilterEffect::processChannel(
        ColorFilterGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);

    const double requestedColor = m_pColorParameter
            ? std::clamp(m_pColorParameter->value(), 0.0, 1.0)
            : kCenter;
    const double targetColor = enableState == EffectEnableState::Disabling
            ? kCenter
            : requestedColor;
    const bool targetLowPassActive = lowPassGain(targetColor) > 0.0;
    const bool targetHighPassActive = highPassGain(targetColor) > 0.0;

    if (targetLowPassActive) {
        const double cutoff = lowPassCutoff(targetColor);
        if (cutoff != pState->lowPassCutoff) {
            pState->lowPassFilter->setFrequencyCorners(
                    engineParameters.sampleRate(), cutoff, kFilterQ);
            pState->lowPassCutoff = cutoff;
        }
        pState->lowPassFilter->process(pInput,
                pState->lowPassBuffer.data(),
                engineParameters.samplesPerBuffer());
    } else if (pState->lowPassActive) {
        pState->lowPassFilter->processAndPauseFilter(pInput,
                pState->lowPassBuffer.data(),
                engineParameters.samplesPerBuffer());
    }

    if (targetHighPassActive) {
        const double cutoff = highPassCutoff(targetColor);
        if (cutoff != pState->highPassCutoff) {
            pState->highPassFilter->setFrequencyCorners(
                    engineParameters.sampleRate(), cutoff, kFilterQ);
            pState->highPassCutoff = cutoff;
        }
        pState->highPassFilter->process(pInput,
                pState->highPassBuffer.data(),
                engineParameters.samplesPerBuffer());
    } else if (pState->highPassActive) {
        pState->highPassFilter->processAndPauseFilter(pInput,
                pState->highPassBuffer.data(),
                engineParameters.samplesPerBuffer());
    }

    RampingValue<CSAMPLE_GAIN> colorRamp(
            pState->previousColor, targetColor, engineParameters.samplesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i);
        const CSAMPLE_GAIN lowGain = lowPassGain(color);
        const CSAMPLE_GAIN highGain = highPassGain(color);
        const CSAMPLE_GAIN dryGain = std::max<CSAMPLE_GAIN>(
                0.0f, 1.0f - lowGain - highGain);
        CSAMPLE output = pInput[i] * dryGain;
        if (lowGain > 0.0f) {
            output += pState->lowPassBuffer[i] * lowGain;
        }
        if (highGain > 0.0f) {
            output += pState->highPassBuffer[i] * highGain;
        }
        pOutput[i] = output;
    }

    pState->previousColor = targetColor;
    pState->lowPassActive = targetLowPassActive;
    pState->highPassActive = targetHighPassActive;
}
