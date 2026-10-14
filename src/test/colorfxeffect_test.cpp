#include <gtest/gtest.h>

#include <QMap>
#include <cmath>

#include "effects/backends/builtin/colordubechoeffect.h"
#include "effects/backends/builtin/colornoiseeffect.h"
#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/engine.h"
#include "util/samplebuffer.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(48000);
constexpr SINT kFramesPerBuffer = 256;
constexpr double kPi = 3.14159265358979323846;

template<typename Effect, typename State>
class ColorFxHarness {
  public:
    ColorFxHarness()
            : m_engineParameters(kSampleRate, kFramesPerBuffer),
              m_pManifest(Effect::getManifest()),
              m_pColor(new EngineEffectParameter(
                      m_pManifest->parameters().first())),
              m_state(m_engineParameters),
              m_input(m_engineParameters.samplesPerBuffer()),
              m_output(m_engineParameters.samplesPerBuffer()) {
        QMap<QString, EngineEffectParameterPointer> parameters;
        parameters.insert(m_pColor->id(), m_pColor);
        m_effect.loadEngineEffectParameters(parameters);
    }

    SINT samplesPerBuffer() const {
        return m_engineParameters.samplesPerBuffer();
    }

    CSAMPLE* input() {
        return m_input.data();
    }

    const CSAMPLE* output() const {
        return m_output.data();
    }

    void setBeatLengthSeconds(double seconds) {
        m_features.beat_length = GroupFeatureBeatLength{seconds, 1.0};
    }

    void process(double color) {
        m_pColor->setValue(color);
        m_effect.processChannel(&m_state,
                m_input.data(),
                m_output.data(),
                m_engineParameters,
                EffectEnableState::Enabled,
                m_features);
    }

  private:
    const mixxx::EngineParameters m_engineParameters;
    EffectManifestPointer m_pManifest;
    EngineEffectParameterPointer m_pColor;
    Effect m_effect;
    State m_state;
    GroupFeatureState m_features;
    mixxx::SampleBuffer m_input;
    mixxx::SampleBuffer m_output;
};

using DubEchoHarness = ColorFxHarness<ColorDubEchoEffect, ColorDubEchoGroupState>;
using NoiseHarness = ColorFxHarness<ColorNoiseEffect, ColorNoiseGroupState>;

void fillSine(CSAMPLE* pBuffer, SINT samples, SINT* pFrame) {
    for (SINT i = 0; i < samples; i += mixxx::kEngineChannelCount) {
        const auto value = static_cast<CSAMPLE>(
                0.5 * std::sin(2.0 * kPi * 440.0 * (*pFrame)++ / kSampleRate));
        pBuffer[i] = value;
        pBuffer[i + 1] = -value;
    }
}

TEST(ColorFxEffectTest, NoiseDeadZoneIsBitExactDry) {
    NoiseHarness harness;
    SINT frame = 0;
    // Center, then a dead-zone position that is not the exact center.
    for (const double color : {0.5, 0.5, 127.0 / 255.0, 127.0 / 255.0}) {
        fillSine(harness.input(), harness.samplesPerBuffer(), &frame);
        harness.process(color);
        for (SINT i = 0; i < harness.samplesPerBuffer(); ++i) {
            ASSERT_EQ(harness.output()[i], harness.input()[i]) << "sample " << i;
        }
    }
}

TEST(ColorFxEffectTest, NoiseIsAddedOutsideDeadZone) {
    for (const double color : {0.0, 1.0}) {
        NoiseHarness harness;
        SINT frame = 0;
        double maxDifference = 0.0;
        // Let the ramp from the center settle before measuring.
        for (int buffer = 0; buffer < 4; ++buffer) {
            fillSine(harness.input(), harness.samplesPerBuffer(), &frame);
            harness.process(color);
        }
        for (SINT i = 0; i < harness.samplesPerBuffer(); ++i) {
            const double difference =
                    std::abs(harness.output()[i] - harness.input()[i]);
            ASSERT_TRUE(std::isfinite(difference));
            maxDifference = std::max(maxDifference, difference);
        }
        EXPECT_GT(maxDifference, 0.0) << "color " << color;
        EXPECT_LT(maxDifference, 1.0) << "color " << color;
    }
}

TEST(ColorFxEffectTest, DubEchoCenterIsBitExactDry) {
    DubEchoHarness harness;
    SINT frame = 0;
    for (int buffer = 0; buffer < 3; ++buffer) {
        fillSine(harness.input(), harness.samplesPerBuffer(), &frame);
        harness.process(0.5);
        for (SINT i = 0; i < harness.samplesPerBuffer(); ++i) {
            ASSERT_EQ(harness.output()[i], harness.input()[i]) << "sample " << i;
        }
    }
}

TEST(ColorFxEffectTest, DubEchoRepeatsAfterThreeQuartersOfABeat) {
    for (const double color : {0.0, 1.0}) {
        DubEchoHarness harness;
        harness.setBeatLengthSeconds(0.5);
        const SINT delayFrames = static_cast<SINT>(0.75 * 0.5 * kSampleRate);
        const SINT impulseFrames = 64;
        SINT frame = 0;
        SINT firstEchoFrame = -1;
        while (frame < delayFrames + 4 * kFramesPerBuffer) {
            const SINT bufferStart = frame;
            for (SINT i = 0; i < harness.samplesPerBuffer();
                    i += mixxx::kEngineChannelCount) {
                const CSAMPLE value = frame++ < impulseFrames ? 0.5f : 0.0f;
                harness.input()[i] = value;
                harness.input()[i + 1] = value;
            }
            harness.process(color);
            for (SINT i = 0; i < harness.samplesPerBuffer();
                    i += mixxx::kEngineChannelCount) {
                const SINT outputFrame = bufferStart + i / mixxx::kEngineChannelCount;
                const CSAMPLE wet = harness.output()[i] - harness.input()[i];
                ASSERT_TRUE(std::isfinite(wet));
                if (outputFrame < delayFrames) {
                    ASSERT_EQ(wet, 0.0f) << "early echo at frame " << outputFrame;
                } else if (firstEchoFrame < 0 && wet != 0.0f) {
                    firstEchoFrame = outputFrame;
                }
            }
        }
        // The color ramps away from the dry center during the first buffer,
        // so the earliest frames may send almost nothing into the delay.
        EXPECT_GE(firstEchoFrame, delayFrames) << "color " << color;
        EXPECT_LT(firstEchoFrame, delayFrames + kFramesPerBuffer) << "color " << color;
    }
}

} // namespace
