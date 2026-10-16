#include "effects/backends/builtin/pitchshifteffect.h"

#include <QString>
#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
const QString kPitchParameterId = QStringLiteral("rkb_pitch");
const QString kDepthParameterId = QStringLiteral("depth");

constexpr float kFloatEpsilon = 1.1920929e-07f;
constexpr float kMaxOffsetStep = 0.05f;
constexpr float kGateOffset = -0.95f;
constexpr double kQuality = 0.70710678118654757;

float smoothstep(float t) {
    const float t2 = t * t;
    return std::fma(t2, 3.0f, -((t2 + t2) * t));
}
} // anonymous namespace

namespace lastcue {

void RbxPitchShifter::Biquad::configure(double sampleRate, double cutoff, bool highPass) {
    const double k = std::tan(
            3.14159265358979323846 / static_cast<int>(sampleRate) *
            std::min(cutoff, sampleRate * 0.5));
    // Same operation order as the native IirHpf2ndOrder/IirLpf2ndOrder.
    const double k2 = k * k;
    const double den = std::fma(k, 1.0 / kQuality, 1.0) + k2;
    const double twoK2MinusTwo = std::fma(k + k, k, -2.0);
    if (highPass) {
        const double norm = 1.0 / den;
        b0 = static_cast<float>(norm);
        b1 = static_cast<float>(norm * -2.0);
        a1 = static_cast<float>(twoK2MinusTwo * norm);
    } else {
        b0 = static_cast<float>(k2 / den);
        b1 = b0 + b0;
        a1 = static_cast<float>(twoK2MinusTwo / den);
    }
    b2 = b0;
    a2 = static_cast<float>((std::fma(-k, 1.0 / kQuality, 1.0) + k2) / den);
    memory = {};
}

float RbxPitchShifter::Biquad::process(float x, int channel) {
    auto& m = memory[channel];
    const float y = b0 * x + b1 * m[0] + b2 * m[1] - a1 * m[2] - a2 * m[3];
    m[1] = m[0];
    m[0] = x;
    m[3] = m[2];
    m[2] = y;
    return y;
}

float RbxPitchShifter::offsetForControl(float control) {
    if (control < -200.0f) {
        return -200.0f * 0.005f;
    }
    const float x = std::min(control, 200.0f);
    if (x < 0.0f) {
        return x * 0.005f;
    }
    if (x < 100.0f) {
        return x * 0.01f;
    }
    return std::fma(x - 100.0f, 0.02f, 1.0f);
}

void RbxPitchShifter::configure(double sampleRate, int maxFramesPerBuffer) {
    m_sampleRate = sampleRate;
    m_minWindow = static_cast<int>(std::lrint(sampleRate * 0.013));
    m_crossfade = static_cast<int>(std::lrint(sampleRate * 0.003));
    m_crossfadeStep = 1.0f / static_cast<float>(m_crossfade);
    m_maxWindow = static_cast<int>(std::lrint(sampleRate * 0.023));
    m_startFill = static_cast<int>(std::lrint(sampleRate * 0.03));
    m_gateFrames = static_cast<int>(std::lrint(static_cast<float>(sampleRate) * 0.001f * 20.0f));
    // The native ring holds one callback plus the crossfade and start fill.
    // A larger ring reads identically because delays never reach its end.
    m_ringFrames = std::max(maxFramesPerBuffer, 1) + m_crossfade + m_startFill;
    m_ring.assign(static_cast<size_t>(m_ringFrames) * 2, 0);
    m_analysis.assign(static_cast<size_t>(std::max(maxFramesPerBuffer, 1)) + 1, 0);
    m_highPass.configure(sampleRate, 30.0, true);
    m_lowPass.configure(sampleRate, 300.0, false);
    reset();
}

void RbxPitchShifter::reset() {
    std::fill(m_ring.begin(), m_ring.end(), 0);
    std::fill(m_analysis.begin(), m_analysis.end(), 0);
    m_write = 0;
    m_filled = 0;
    m_offset = 0;
    m_delay = 0;
    m_jump = 0;
    m_splicing = false;
    m_starting = true;
    m_crossfadeCount = 0;
    m_zeroCrossingAge = 0;
    m_highPass.memory = {};
    m_lowPass.memory = {};
    m_gate.value = m_gate.target = 1;
    m_gate.remaining = 0;
}

void RbxPitchShifter::setControl(float control) {
    m_target = offsetForControl(control);
}

float RbxPitchShifter::read(int channel, int delay) const {
    if (delay < 1 || m_filled < delay) {
        return 0;
    }
    int index = m_write - delay;
    if (index < 0) {
        index += m_ringFrames;
    }
    return m_ring[2 * index + channel];
}

void RbxPitchShifter::readInterpolated(float delay, float* pLeft, float* pRight) const {
    const int whole = static_cast<int>(delay);
    const float t = delay - static_cast<float>(whole);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float w3 = t3 - t;
    const float w0 = (t + t) + t3;
    const float w2 = (t3 - t2) - (t + t);
    const float w1 = (w3 - (t2 + t2)) + 2.0f;
    for (int channel = 0; channel < 2; ++channel) {
        const float s0 = read(channel, whole - 1);
        const float s1 = read(channel, whole);
        const float s2 = read(channel, whole + 1);
        const float s3 = read(channel, whole + 2);
        float even = std::fma(w1, s1, -(w2 * s2));
        even = std::fma(t2, s0, even);
        const float odd = std::fma(w3, s3, -(w0 * s0));
        const float value = std::fma(even, 0.5f, odd * (1.0f / 6.0f));
        *(channel == 0 ? pLeft : pRight) = value;
    }
}

void RbxPitchShifter::controlSplice() {
    if (m_offset != m_target) {
        if (std::abs(m_target - m_offset) > kMaxOffsetStep) {
            m_offset = m_offset < m_target ? m_offset + kMaxOffsetStep
                                           : m_offset - kMaxOffsetStep;
        } else {
            m_offset = m_target;
        }
    }
    const int delay = static_cast<int>(m_delay);
    const int scaled = static_cast<int>(std::lrint(
            static_cast<float>(m_crossfade) * m_offset));
    int expected = delay - static_cast<int>(std::lrint(m_offset));
    if (expected <= 1) {
        expected = 2;
    }
    const int center = scaled + m_minWindow;
    const int high = m_maxWindow + scaled;
    const int crossing = m_zeroCrossingAge;
    int jump = 0;
    bool splice = false;
    if (m_offset < 0) {
        if (expected >= m_maxWindow) {
            jump = center - delay;
            splice = true;
        } else if (expected >= center && crossing > m_minWindow && crossing < delay) {
            jump = crossing - delay;
            splice = true;
        }
    } else if (m_offset > 0) {
        if (expected <= m_minWindow) {
            jump = high - delay;
            splice = true;
        } else if (expected <= center && crossing > center && crossing < high) {
            jump = crossing - delay;
            splice = true;
        }
    } else if (expected < m_minWindow) {
        jump = center - delay;
        splice = true;
    }
    if (splice) {
        m_jump = static_cast<float>(jump);
        m_splicing = true;
        m_crossfadeCount = 0;
    }
}

void RbxPitchShifter::process(CSAMPLE* pInterleaved, int frames) {
    if (frames <= 0 || m_ring.empty()) {
        return;
    }
    if (static_cast<size_t>(frames) + 1 > m_analysis.size()) {
        // Engine buffers never exceed the configured size; stay safe anyway.
        frames = static_cast<int>(m_analysis.size()) - 1;
    }

    // Analysis band for splice points: 30 Hz high-pass, 300 Hz low-pass.
    for (int i = 0; i < frames; ++i) {
        const float left = m_lowPass.process(
                m_highPass.process(pInterleaved[2 * i], 0), 0);
        const float right = m_lowPass.process(
                m_highPass.process(pInterleaved[2 * i + 1], 1), 1);
        m_analysis[i + 1] = right * 0.0859375f + left * 0.9140625f;
    }
    int crossing = -1;
    for (int i = 0; i < frames; ++i) {
        if (!(m_analysis[i] * m_analysis[i + 1] > 0) && m_analysis[i] < 0) {
            crossing = i;
            break;
        }
    }
    if (crossing >= 0 && m_zeroCrossingAge >= m_minWindow) {
        m_zeroCrossingAge = -crossing;
    }
    m_analysis[0] = m_analysis[frames];

    for (int i = 0; i < frames; ++i) {
        const float inLeft = pInterleaved[2 * i];
        const float inRight = pInterleaved[2 * i + 1];
        float outLeft;
        float outRight;
        if (!m_starting && !m_splicing) {
            controlSplice();
        }
        if (m_starting) {
            if (m_filled < m_startFill) {
                outLeft = inLeft;
                outRight = inRight;
                m_delay += 1.0f;
            } else {
                m_delay -= m_offset;
                float left;
                float right;
                readInterpolated(m_delay, &left, &right);
                const float w = smoothstep(
                        static_cast<float>(m_filled - m_startFill) * m_crossfadeStep);
                outLeft = std::fma(w, left, std::fma(-inLeft, w, inLeft));
                outRight = std::fma(w, right, std::fma(-inRight, w, inRight));
                if (w >= 1.0f - m_crossfadeStep) {
                    m_starting = false;
                }
            }
        } else if (m_splicing) {
            m_delay -= m_offset;
            float oldLeft;
            float oldRight;
            float newLeft;
            float newRight;
            readInterpolated(m_delay, &oldLeft, &oldRight);
            readInterpolated(m_delay + m_jump, &newLeft, &newRight);
            const float w = smoothstep(
                    static_cast<float>(m_crossfadeCount) * m_crossfadeStep);
            outLeft = std::fma(1.0f - w, oldLeft, w * newLeft);
            outRight = std::fma(1.0f - w, oldRight, w * newRight);
            ++m_crossfadeCount;
            if (w >= 1.0f - m_crossfadeStep) {
                m_delay = m_jump + m_delay;
                m_splicing = false;
                m_crossfadeCount = 0;
            }
        } else {
            m_delay -= m_offset;
            readInterpolated(m_delay, &outLeft, &outRight);
        }
        m_ring[2 * m_write] = inLeft;
        m_ring[2 * m_write + 1] = inRight;
        m_write = (m_write + 1) % m_ringFrames;
        m_filled = std::min(m_filled + 1, m_ringFrames);
        pInterleaved[2 * i] = outLeft;
        pInterleaved[2 * i + 1] = outRight;
    }
    m_zeroCrossingAge += frames;

    // Silence gate for read speeds below 5 %, ramped over 20 ms.
    const float gateTarget = m_offset < kGateOffset ? 0.0f : 1.0f;
    if (gateTarget != m_gate.target) {
        m_gate.target = gateTarget;
        m_gate.remaining = m_gateFrames;
        m_gate.step = (gateTarget - m_gate.value) / static_cast<float>(m_gateFrames);
    }
    for (int i = 0; i < frames; ++i) {
        const float gain = m_gate.value;
        pInterleaved[2 * i] *= gain;
        pInterleaved[2 * i + 1] *= gain;
        if (m_gate.remaining > 0) {
            if (--m_gate.remaining == 0) {
                m_gate.value = m_gate.target;
            } else {
                m_gate.value += m_gate.step;
            }
        }
    }
}

} // namespace lastcue

PitchShiftGroupState::PitchShiftGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters) {
    audioParametersChanged(engineParameters);
}

void PitchShiftGroupState::audioParametersChanged(
        const mixxx::EngineParameters& engineParameters) {
    shifter.configure(engineParameters.sampleRate(), engineParameters.framesPerBuffer());
    wet.assign(engineParameters.samplesPerBuffer(), 0);
    initialized = false;
}

// static
QString PitchShiftEffect::getId() {
    return QStringLiteral("org.mixxx.effects.pitchshift");
}

//static
EffectManifestPointer PitchShiftEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());

    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Pitch Shift"));
    pManifest->setShortName(QObject::tr("Pitch Shift"));
    pManifest->setAuthor("The LastCUE Box Team");
    pManifest->setVersion("3.0");
    pManifest->setDescription(QObject::tr(
            "Raises or lowers the original pitch of a sound."));
    pManifest->setMetaknobDefault(0.5);

    // The first knob is the PITCH control: -1 OCT .. +1 OCT at the default
    // LEVEL/DEPTH. New ids keep presets of the former RubberBand effect from
    // linking the knob to the metaknob.
    EffectManifestParameterPointer pitch = pManifest->addParameter();
    pitch->setId(kPitchParameterId);
    pitch->setName(QObject::tr("Pitch"));
    pitch->setShortName(QObject::tr("Pitch"));
    pitch->setDescription(QObject::tr(
            "The pitch shift applied to the sound."));
    pitch->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    pitch->setNeutralPointOnScale(0.5);
    pitch->setRange(-1.0, -0.5, 1.0);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId(kDepthParameterId);
    depth->setName(QObject::tr("Level"));
    depth->setShortName(QObject::tr("Level"));
    depth->setDescription(QObject::tr(
            "Scales the range of the Pitch knob."));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    depth->setRange(0.0, 0.5, 1.0);

    return pManifest;
}

void PitchShiftEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pPitchParameter = parameters.value(kPitchParameterId);
    m_pDepthParameter = parameters.value(kDepthParameterId);
}

// static
float PitchShiftEffect::nativeControl(double pitch, double depth) {
    const float p = static_cast<float>((pitch + 1.0) * 0.5);
    const float scaled = p * 200.0f;
    const float control = ((scaled - 100.0f) + scaled) - 100.0f;
    const float result = control * static_cast<float>(depth);
    // The native PITCH parameter is an integer clamped to -200..+200.
    return static_cast<float>(std::clamp(static_cast<int>(result), -200, 200));
}

void PitchShiftEffect::processChannel(
        PitchShiftGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);
    const int frames = static_cast<int>(engineParameters.framesPerBuffer());
    const SINT samples = engineParameters.samplesPerBuffer();
    if (static_cast<SINT>(pState->wet.size()) < samples) {
        pState->wet.resize(samples);
    }

    const double pitch = m_pPitchParameter ? m_pPitchParameter->value() : 0.0;
    const double depth = m_pDepthParameter ? m_pDepthParameter->value() : 0.5;
    const float p = static_cast<float>((pitch + 1.0) * 0.5);
    const bool neutral = std::abs(static_cast<float>(depth)) < kFloatEpsilon ||
            std::abs(p - 0.5f) < kFloatEpsilon;
    const float dryTarget = neutral ? 1.0f : 0.0f;

    if (!pState->initialized || enableState == EffectEnableState::Enabling) {
        pState->shifter.reset();
        pState->dry.value = pState->dry.target = dryTarget;
        pState->dry.remaining = 0;
        pState->wetGain.value = pState->wetGain.target = 1.0f - dryTarget;
        pState->wetGain.remaining = 0;
        pState->initialized = true;
    }

    pState->shifter.setControl(nativeControl(pitch, depth));
    std::copy(pInput, pInput + samples, pState->wet.begin());
    pState->shifter.process(pState->wet.data(), frames);

    // The native host switches between dry and the shifted signal with
    // 1/300 s ramps; a centered knob therefore has no shifter latency.
    const int rampFrames = std::max(1,
            static_cast<int>(0.0033333334f * static_cast<float>(engineParameters.sampleRate())));
    pState->dry.set(dryTarget, rampFrames);
    pState->wetGain.set(1.0f - dryTarget, rampFrames);
    for (int i = 0; i < frames; ++i) {
        const float dry = pState->dry.next();
        const float wet = pState->wetGain.next();
        pOutput[2 * i] = dry * pInput[2 * i] + wet * pState->wet[2 * i];
        pOutput[2 * i + 1] = dry * pInput[2 * i + 1] + wet * pState->wet[2 * i + 1];
    }
}
