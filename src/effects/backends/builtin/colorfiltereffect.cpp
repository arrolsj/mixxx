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
constexpr double kPi = 3.14159265358979323846;
constexpr double kFilterQ = 2.0;

int colorByte(double color) {
    return static_cast<int>(std::round(std::clamp(color, 0.0, 1.0) * 255.0));
}

// The reference uses an unprewarped bilinear transform. Convert its analog
// control frequency for the existing RBJ/Fidlib filters to obtain the same poles.
double digitalFrequency(double nominalHz, double sampleRate) {
    return sampleRate / kPi * std::atan(kPi * nominalHz / sampleRate);
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

double lowPassCutoff(double color, double sampleRate) {
    const double c = colorByte(color);
    const double nominal = c < 126 ? 100.0 + 0.012497654184699059 * c * c * c : 25100.0;
    return digitalFrequency(nominal, sampleRate);
}

double nominalHighPass(double color) {
    const double c = colorByte(color) - 130.0;
    return c > 0 ? 20.0 + 0.004099230282008648 * c * c * c : 20.0;
}

double highPassQ(double color) {
    const double nominal = nominalHighPass(color);
    double damping = 0.5;
    if (colorByte(color) > 130 && nominal < 249.89999389648438) {
        const double amount = 1.0 - (nominal - 20.0) / 230.0;
        damping = std::clamp(4.0 * amount * amount * amount + 0.029999999329447746,
                0.5, 2.0);
    }
    return 1.0 / damping;
}
} // namespace

ColorFilterGroupState::ColorFilterGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousColor(kCenter),
          lowPassCutoff(::lowPassCutoff(kCenter, engineParameters.sampleRate())),
          highPassCutoff(digitalFrequency(20.0, engineParameters.sampleRate())),
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
    pManifest->setVersion("3.0");
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
        const double cutoff = lowPassCutoff(targetColor, engineParameters.sampleRate());
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
        const double cutoff = digitalFrequency(nominalHighPass(targetColor), engineParameters.sampleRate());
        if (cutoff != pState->highPassCutoff) {
            pState->highPassFilter->setFrequencyCorners(
                    engineParameters.sampleRate(), cutoff, highPassQ(targetColor));
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
            pState->previousColor, targetColor, engineParameters.framesPerBuffer());
    for (SINT i = 0; i < engineParameters.samplesPerBuffer(); ++i) {
        const CSAMPLE_GAIN color = colorRamp.getNth(i / mixxx::kEngineChannelCount);
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
