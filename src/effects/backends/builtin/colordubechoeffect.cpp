#include "effects/backends/builtin/colordubechoeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "util/sample.h"

namespace {
constexpr double kCenter = 0.5;
constexpr double kQ = 0.7071067811865476;

void setBranch(ColorDubEchoGroupState::Branch* branch,
        double distance, bool left, double sampleRate, int rampFrames) {
    branch->requestedDelayFrames = (55.0 + std::max(0.0, distance - 0.2) *
            (left ? 167.5 : 118.75)) * sampleRate / 1000.0;
    branch->feedback = left ? 0.87f + 0.1f * std::max(0.0, distance - 0.2)
                           : (distance < 0.2 ? 0.88f : 0.88f + 0.1f * distance);
    const float send = std::min(distance * (10.0 / 3.0), 1.0);
    if (send != branch->targetSend) {
        branch->targetSend = send;
        branch->sendStep = (send - branch->send) / rampFrames;
        branch->sendRemaining = rampFrames;
    }
}

CSAMPLE readDelay(const ColorDubEchoGroupState::Branch& branch,
        int writeFrame, int channel, int channels, double delayFrames) {
    const int frames = branch.delay.size() / channels;
    const int whole = static_cast<int>(delayFrames);
    const float fraction = delayFrames - whole;
    const int recent = (writeFrame + frames - whole) % frames;
    const int older = (recent + frames - 1) % frames;
    const CSAMPLE a = branch.delay[recent * channels + channel];
    return a + fraction * (branch.delay[older * channels + channel] - a);
}
} // namespace

ColorDubEchoGroupState::Branch::Branch(int samples, int blockSamples)
        : delay(samples), work(blockSamples) {
    delay.clear();
    work.clear();
}

ColorDubEchoGroupState::ColorDubEchoGroupState(const mixxx::EngineParameters& parameters)
        : EffectState(parameters),
          branches{Branch(static_cast<int>(parameters.sampleRate() * 0.25) *
                                  parameters.channelCount(), parameters.samplesPerBuffer()),
                  Branch(static_cast<int>(parameters.sampleRate() * 0.25) *
                                  parameters.channelCount(), parameters.samplesPerBuffer())},
          leftPre(std::make_unique<EngineFilterBiquad1Band>(parameters.sampleRate(), 1375.0, kQ)),
          rightPre(std::make_unique<EngineFilterBiquad1High>(parameters.sampleRate(), 2500.0, kQ, false)),
          rightPost(std::make_unique<EngineFilterBiquad1Low>(parameters.sampleRate(), 12000.0, kQ, false)) {
    leftPre->assumeSettled();
    rightPre->assumeSettled();
    rightPost->assumeSettled();
    for (auto& branch : branches) {
        branch.delayFrames = branch.nextDelayFrames = branch.requestedDelayFrames =
                parameters.sampleRate() * 0.055;
    }
}

ColorDubEchoGroupState::~ColorDubEchoGroupState() = default;

QString ColorDubEchoEffect::getId() {
    return QStringLiteral("org.mixxx.effects.colordubecho");
}

EffectManifestPointer ColorDubEchoEffect::getManifest() {
    EffectManifestPointer manifest(new EffectManifest());
    manifest->setId(getId());
    manifest->setName(QObject::tr("Color Dub Echo"));
    manifest->setShortName(QObject::tr("Color Dub Echo"));
    manifest->setAuthor("The LastCUE Box Team");
    manifest->setVersion("3.0");
    manifest->setDescription(QObject::tr("Dual filtered dub echo with a single Color control"));
    manifest->setEffectRampsFromDry(true);
    manifest->setMetaknobDefault(kCenter);
    auto color = manifest->addParameter();
    color->setId("color");
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr("Center stops new repeats; existing echoes decay naturally"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(kCenter);
    color->setRange(0.0, kCenter, 1.0);
    return manifest;
}

void ColorDubEchoEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value("color");
}

void ColorDubEchoEffect::processChannel(ColorDubEchoGroupState* state,
        const CSAMPLE* input, CSAMPLE* output, const mixxx::EngineParameters& parameters,
        EffectEnableState enableState, const GroupFeatureState& /*groupFeatures*/) {
    const int target = enableState == EffectEnableState::Disabling ? 127 :
            static_cast<int>(255 * std::clamp(m_pColorParameter->value(), 0.0, 1.0));
    const int channels = parameters.channelCount();
    const int ringFrames = state->branches[0].delay.size() / channels;
    const int rampFrames = std::max(1, static_cast<int>(parameters.sampleRate() * 0.0033));
    const int controlFrames = std::max(1, static_cast<int>(parameters.sampleRate() / 1500.0));
    state->leftPre->process(input, state->branches[0].work.data(), parameters.samplesPerBuffer());
    state->rightPre->process(input, state->branches[1].work.data(), parameters.samplesPerBuffer());
    for (int frame = 0; frame < parameters.framesPerBuffer(); ++frame) {
        if (--state->controlCountdown <= 0) {
            state->color += (target > state->color) - (target < state->color);
            state->controlCountdown = controlFrames;
            const double left = state->color < 116 ? (116.0 - state->color) / 116.0 : 0;
            const double right = state->color >= 139 ? (state->color - 139.0) / 116.0 : 0;
            // Auxiliary Parameter is fixed at its default (0.5 / code 127).
            setBranch(&state->branches[0], left, true, parameters.sampleRate(), rampFrames);
            setBranch(&state->branches[1], right, false, parameters.sampleRate(), rampFrames);
        }
        for (auto& branch : state->branches) {
            if (branch.sendRemaining > 0 && --branch.sendRemaining == 0) {
                branch.send = branch.targetSend;
            } else if (branch.sendRemaining > 0) {
                branch.send += branch.sendStep;
            }
            if (branch.fadeRemaining == 0 && branch.requestedDelayFrames != branch.delayFrames) {
                branch.nextDelayFrames = branch.requestedDelayFrames;
                branch.fadeRemaining = rampFrames;
            }
            const float fade = 1.0f - static_cast<float>(branch.fadeRemaining) / rampFrames;
            for (int channel = 0; channel < channels; ++channel) {
                const int index = frame * channels + channel;
                CSAMPLE delayed = readDelay(branch, state->writeFrame, channel, channels, branch.delayFrames);
                if (branch.fadeRemaining > 0) {
                    delayed += fade * (readDelay(branch, state->writeFrame, channel, channels,
                            branch.nextDelayFrames) - delayed);
                }
                branch.delay[state->writeFrame * channels + channel] =
                        branch.work[index] * branch.send + branch.feedback * delayed;
                branch.work[index] = delayed;
            }
            if (branch.fadeRemaining > 0 && --branch.fadeRemaining == 0) {
                branch.delayFrames = branch.nextDelayFrames;
            }
        }
        state->writeFrame = (state->writeFrame + 1) % ringFrames;
    }
    auto& left = state->branches[0].work;
    auto& right = state->branches[1].work;
    state->rightPost->process(right.data(), right.data(), parameters.samplesPerBuffer());
    for (int i = 0; i < parameters.samplesPerBuffer(); ++i) {
        // Default branch gain is 0.7; the wrapper attenuates the left branch again.
        output[i] = input[i] + 0.49f * left[i] + 0.7f * right[i];
    }
    if (enableState == EffectEnableState::Disabling) {
        for (auto& branch : state->branches) {
            branch.delay.clear();
            branch.send = branch.targetSend = branch.sendStep = 0;
            branch.sendRemaining = branch.fadeRemaining = 0;
            branch.delayFrames = branch.nextDelayFrames = branch.requestedDelayFrames =
                    parameters.sampleRate() * 0.055;
        }
        state->color = 127;
        state->controlCountdown = state->writeFrame = 0;
        state->leftPre->pauseFilter();
        state->rightPre->pauseFilter();
        state->rightPost->pauseFilter();
    }
}
