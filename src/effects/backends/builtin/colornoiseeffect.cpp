#include "effects/backends/builtin/colornoiseeffect.h"

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
// Default PARAM 0.5 is truncated to code 127 in the Djm900Nxs2 path.
constexpr float kDefaultParamGain = 0.14766541123390198f;

float eighthPower(float value) {
    const float square = value * value;
    const float fourth = square * square;
    return fourth * fourth;
}

float colorGain(int code) {
    if (code < 124) {
        const float shape = code <= 40 ? 1.0f :
                eighthPower(std::fma(-0.00139999995008111f, float(code - 40), 1.0f));
        return float(124 - code) * 0.011220184154808521f * shape;
    }
    if (code < 132) {
        return 0.0f;
    }
    if (code < 157) {
        const float distance = 157 - code;
        return std::fma(-0.320000022649765f,
                (distance * 0.004000000189989805f) * distance, 0.800000011920929f);
    }
    if (code < 182) {
        return std::fma(0.010535363107919693f, float(182 - code), 0.5366159081459045f);
    }
    return float(code - 131) * 0.011220184154808521f *
            eighthPower(std::fma(-0.004000000189989805f, float(code - 180), 1.0f));
}

float toneGain(int code) {
    const float distance = code < 124 ? 123 - code : (code >= 132 ? code - 132 : 0);
    const float shaped = (2.0f * 3.3049109333660454e-5f * distance) * distance;
    return std::min(1.0f, std::fma(0.1f, eighthPower(shaped), 0.7f));
}

double filterQ(int code) {
    if (code >= 117 && code < 127) {
        return 2.0 * std::pow(0.938931, code - 116);
    }
    return code >= 245 ? 2.0 * std::pow(0.938931, code - 244) : 2.0;
}

double filterFrequency(int code, double q, double sampleRate) {
    const double frequency = 100.0 * std::pow(1.040242, code < 128 ? code : code - 128);
    return std::clamp(frequency * q / (q - 0.5), 5.0, 0.45 * sampleRate);
}
} // namespace

ColorNoiseGroupState::ColorNoiseGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          previousGain(0.0f),
          previousToneGain(0.7f),
          bandPassCutoff(100.0 * 2.0 / 1.5),
          bandPassQ(2.0),
          noiseBuffer(engineParameters.samplesPerBuffer()),
          bandPassBuffer(engineParameters.samplesPerBuffer()),
          generator(std::random_device{}()),
          bandPassFilter(std::make_unique<EngineFilterBiquad1Band>(
                  engineParameters.sampleRate(), bandPassCutoff, bandPassQ)) {
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
    pManifest->setVersion("3.0");
    pManifest->setDescription(QObject::tr(
            "Rekordbox-style band-pass noise with Color frequency and level curves"));
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
    // Unlike FILTER, NOISE truncates the normalized control to a byte.
    const int code = static_cast<int>(targetColor * 255.0);
    const CSAMPLE_GAIN targetGain = std::clamp(colorGain(code) * kDefaultParamGain, 0.0f, 1.0f);
    const CSAMPLE_GAIN targetToneGain = toneGain(code);
    const SINT samples = engineParameters.samplesPerBuffer();
    if (targetGain == 0.0f && pState->previousGain == 0.0f) {
        SampleUtil::copy(pOutput, pInput, samples);
        pState->bandPassFilter->pauseFilter();
        pState->previousToneGain = targetToneGain;
        return;
    }

    const double q = filterQ(code);
    const double frequency = filterFrequency(code, q, engineParameters.sampleRate());
    if (frequency != pState->bandPassCutoff || q != pState->bandPassQ) {
        pState->bandPassFilter->setFrequencyCorners(engineParameters.sampleRate(), frequency, q);
        pState->bandPassCutoff = frequency;
        pState->bandPassQ = q;
    }
    RampingValue<CSAMPLE_GAIN> gainRamp(pState->previousGain, targetGain,
            engineParameters.framesPerBuffer());
    RampingValue<CSAMPLE_GAIN> toneRamp(pState->previousToneGain, targetToneGain,
            engineParameters.framesPerBuffer());
    std::uniform_real_distribution<CSAMPLE> noiseDistribution(-1.0f, 1.0f);
    for (SINT i = 0; i < samples; ++i) {
        pState->noiseBuffer[i] = noiseDistribution(pState->generator) *
                gainRamp.getNth(i / mixxx::kEngineChannelCount);
    }
    pState->bandPassFilter->process(pState->noiseBuffer.data(), pState->bandPassBuffer.data(), samples);
    for (SINT i = 0; i < samples; ++i) {
        pOutput[i] = pInput[i] + pState->bandPassBuffer[i] *
                toneRamp.getNth(i / mixxx::kEngineChannelCount);
    }
    pState->previousGain = targetGain;
    pState->previousToneGain = targetToneGain;
}
