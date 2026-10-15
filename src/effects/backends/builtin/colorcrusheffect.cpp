#include "effects/backends/builtin/colorcrusheffect.h"

#include "engine/effects/engineeffectparameter.h"

namespace {
const QString kColorParameterId = QStringLiteral("color");
constexpr double kCenter = 0.5;
// PARAM is fixed at its default (128), where the optional mantissa mask is bypassed.
constexpr std::array<int, 128> kHighPassHz = {
        20,20,22,23,24,25,26,27,28,30,31,32,34,35,37,38,
        40,42,44,46,48,50,52,54,57,59,62,64,67,70,73,77,
        80,84,87,91,95,99,104,108,113,118,123,129,135,141,147,153,
        160,167,175,182,190,199,208,217,226,236,247,258,269,281,294,307,
        320,334,349,365,381,398,415,434,453,473,494,516,539,563,587,613,
        641,669,699,730,762,796,831,868,906,946,988,1032,1078,1125,1175,1227,
        1281,1338,1398,1459,1524,1592,1662,1736,1812,1893,1977,2064,2156,2251,2351,2455,
        2563,2677,2796,2919,3049,3184,3325,3472,3626,3786,3954,4129,4129,4129,4129,4129};
}

ColorCrushGroupState::ColorCrushGroupState(const mixxx::EngineParameters& parameters)
        : EffectState(parameters) {
}

QString ColorCrushEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colorcrush");
}

EffectManifestPointer ColorCrushEffect::getManifest() {
    EffectManifestPointer manifest(new EffectManifest());
    manifest->setId(getId());
    manifest->setName(QObject::tr("Color Crush"));
    manifest->setShortName(QObject::tr("Color Crush"));
    manifest->setAuthor("The LastCUE Box Team");
    manifest->setVersion("3.0");
    manifest->setDescription(QObject::tr("Stereo sample hold with high-pass color"));
    manifest->setEffectRampsFromDry(true);
    manifest->setMetaknobDefault(kCenter);
    auto color = manifest->addParameter();
    color->setId(kColorParameterId);
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr("Sample hold in both directions; high-pass increases right"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);
    return manifest;
}

void ColorCrushEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value(kColorParameterId);
}

void ColorCrushEffect::processChannel(ColorCrushGroupState* s,
        const CSAMPLE* input, CSAMPLE* output,
        const mixxx::EngineParameters& p, EffectEnableState enable,
        const GroupFeatureState& features) {
    Q_UNUSED(features);
    const double fs = p.sampleRate();
    const double color = performance_fx::enabled(enable) && m_pColorParameter
            ? std::clamp(m_pColorParameter->value(), 0.0, 1.0) : kCenter;
    const int target = static_cast<int>(std::round(color * 255));
    const int interval = std::max(1, static_cast<int>(std::round(fs / 3000)));
    s->wet.set(color == kCenter ? 0 : 1, performance_fx::rampFrames(fs));
    for (SINT frame = 0; frame < p.framesPerBuffer(); ++frame) {
        if (s->countdown <= 0) {
            s->color += (target > s->color) - (target < s->color);
            s->countdown = interval;
        }
        --s->countdown;
        const int code = std::max(0, 2 * s->color - 255);
        if (s->filterCode != code || s->sampleRate != fs) {
            const float resonance = code <= 3 ? 0 : static_cast<float>(
                    1.299829006 * (1 - std::pow(1 - (code - 3) / 252.0, 16)));
            s->highPass.configure(fs, kHighPassHz[code >> 1], true, resonance);
            s->filterCode = code;
            s->sampleRate = fs;
        }
        if (s->phase < 1) {
            s->held = {input[2 * frame], input[2 * frame + 1]};
            const double holdMs = std::min(0.5, std::abs(2 * s->color / 255.0 - 1) * 0.5);
            s->phase += static_cast<float>(holdMs * fs * 0.001);
        } else {
            s->phase -= 1;
        }
        const float filterDry = code <= 3 ? 1 : code > 129 ? 0 :
                static_cast<float>(std::pow(std::max(0.0, 1 - code / 126.0), 2));
        const float wet = s->wet.next();
        for (int c = 0; c < 2; ++c) {
            const auto filtered = s->highPass.process(s->held[c], c);
            const float crushed = filterDry * s->held[c] + (1 - filterDry) * filtered;
            const int i = 2 * frame + c;
            output[i] = wet == 0 ? input[i] : input[i] + wet * (crushed - input[i]);
        }
    }
}
