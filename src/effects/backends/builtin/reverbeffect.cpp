#include "effects/backends/builtin/reverbeffect.h"

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
double normalizedDepth(double value) {
    const double x = std::clamp(value, 0.0, 1.0);
    if (x < 124.0 / 255) return x * 255 / 248;
    if (x <= 131.0 / 255) return .5;
    return .5 + (x - 131.0 / 255) * 255 / 248;
}

// Measured -5..-35 dB impulse-envelope decay of this existing CAPS network.
// Storage is initialized at 44.1 kHz by MixxxPlateX2::init; compensate its
// sample-counted delays when the engine uses a different sample rate.
float plateDecayParameter(double seconds, double sampleRate) {
    static constexpr double time[] = {.8, 1, 1.25, 1.5318088, 1.75, 2,
            2.5125652, 4, 8.7009239, 16, 29.40036, 60, 100.63781, 220};
    static constexpr double feedback[] = {.0403402039, .0569401062, .0730517761,
            .1318837830, .2810387878, .4262879333, .5604297913, .7197156189,
            .8747294128, .9335614197, .9639538879, .9823846619, .9895860901,
            .9953228210};
    const double t = seconds * sampleRate / 44100;
    double gain = feedback[0];
    if (t >= time[13]) {
        gain = std::pow(feedback[13], time[13]/t);
    } else {
        for (int i = 1; i < 14; ++i) {
            if (t <= time[i]) {
                const double fraction = std::clamp((1/t - 1/time[i-1]) /
                        (1/time[i] - 1/time[i-1]), 0.0, 1.0);
                gain = std::exp(std::log(feedback[i-1]) * (1-fraction) +
                        std::log(feedback[i]) * fraction);
                break;
            }
        }
    }
    return static_cast<float>(std::clamp(gain, 0.0, .9999) / .89);
}
} // namespace

double ReverbEffect::timeSeconds(double normalized) {
    const double x = std::clamp(normalized, 0.0, 1.0);
    const double n = x < 124.0/255 ? 1 + 49*x*255/124
            : x <= 131.0/255 ? 50 : 50 + 50*(x-131.0/255)*255/124;
    return ((((2.8728029519697884e-6*n - .00033391331089660525)*n
            + .01680831052362919)*n - .22553139925003052)*n + 1.7408629655838013);
}

double ReverbEffect::inputLevel(double normalized) {
    const double upper = std::clamp((normalized - 131.0/255)*255/124, 0.0, 1.0);
    return .2 - .1*upper;
}

// static
QString ReverbEffect::getId() {
    return "org.mixxx.effects.reverb";
}

// static
EffectManifestPointer ReverbEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(.5);

    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Reverb"));
    pManifest->setAuthor("The Mixxx Team, CAPS Plugins");
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "Emulates the sound of the signal bouncing off the walls of a room"));

    EffectManifestParameterPointer decay = pManifest->addParameter();
    decay->setId("decay");
    decay->setName(QObject::tr("Time"));
    decay->setShortName(QObject::tr("Time"));
    decay->setDescription(QObject::tr(
            "Reverb decay time on the 1-100 reference scale; center is 50."));
    decay->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    decay->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    decay->setRange(0, 0.5, 1);

    EffectManifestParameterPointer bandwidth = pManifest->addParameter();
    bandwidth->setId("bandwidth");
    bandwidth->setName(QObject::tr("Bandwidth"));
    bandwidth->setShortName(QObject::tr("BW"));
    bandwidth->setDescription(QObject::tr(
            "Bandwidth of the low pass filter at the input.\n"
            "Higher values result in less attenuation of high frequencies."));
    bandwidth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    bandwidth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    bandwidth->setRange(0, 1, 1);

    EffectManifestParameterPointer damping = pManifest->addParameter();
    damping->setId("damping");
    damping->setName(QObject::tr("Damping"));
    damping->setShortName(QObject::tr("Damping"));
    damping->setDescription(
            QObject::tr("Higher damping values cause high frequencies to decay "
                        "more quickly than low frequencies."));
    damping->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    damping->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    damping->setRange(0, 0, 1);

    EffectManifestParameterPointer send = pManifest->addParameter();
    send->setId("send_amount");
    send->setName(QObject::tr("Level"));
    send->setShortName(QObject::tr("Level"));
    send->setDescription(QObject::tr(
            "Dry and wet output balance; maximum removes the dry signal."));
    send->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    send->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    send->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    send->setDefaultLinkInversion(EffectManifestParameter::LinkInversion::NotInverted);
    send->setRange(0, 0.5, 1);

    return pManifest;
}

void ReverbEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pDecayParameter = parameters.value("decay");
    m_pBandWidthParameter = parameters.value("bandwidth");
    m_pDampingParameter = parameters.value("damping");
    m_pSendParameter = parameters.value("send_amount");
}

void ReverbEffect::processChannel(
        ReverbGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);

    const auto decay = plateDecayParameter(timeSeconds(m_pDecayParameter->value()),
            engineParameters.sampleRate());
    const auto bandwidth = static_cast<sample_t>(m_pBandWidthParameter->value());
    const auto damping = static_cast<sample_t>(m_pDampingParameter->value());
    const auto gains = performance_fx::mix(normalizedDepth(m_pSendParameter->value()));
    const float networkLevel = static_cast<float>(inputLevel(m_pDecayParameter->value())) * .7079457640647888f;
    const bool on = performance_fx::enabled(enableState);
    const int ramp = performance_fx::rampFrames(engineParameters.sampleRate());
    pState->dry.set(on ? gains[0] : 1, ramp);
    pState->wet.set(on ? gains[1] : 0, ramp);
    pState->limiter.configure(engineParameters.sampleRate());

    if (pState->sampleRate != engineParameters.sampleRate()) {
        pState->reverb.setSamplerate(engineParameters.sampleRate());
        pState->sampleRate = engineParameters.sampleRate();
    }

    // Reinitialize the effect when turning it on to prevent replaying the old buffer
    // from the last time the effect was enabled..
    if (enableState == EffectEnableState::Enabling) {
        pState->reverb.activate();
    }

    pState->reverb.processBuffer(pInput,
            pOutput,
            engineParameters.samplesPerBuffer(),
            bandwidth,
            decay,
            damping,
            1,
            1);

    // Keep recording while enabled at LEVEL=0, and mix the existing tail at
    // the output so a LEVEL move does not wait for new reverberation to arrive.
    for (int frame = 0; frame < engineParameters.framesPerBuffer(); ++frame) {
        const float dry = pState->dry.next(), wet = pState->wet.next();
        for (int channel = 0; channel < 2; ++channel) {
            const int i = frame * 2 + channel;
            // Reference TIME reduces network input .2 -> .1 above center;
            // -3 dB output trim is independent of LEVEL's wet balance.
            pOutput[i] = dry*pInput[i] + wet*networkLevel*pOutput[i];
        }
        if (wet > 0) pState->limiter.process(pOutput+frame*2);
        else pState->limiter.gain=1;
    }
}
