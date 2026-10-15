#include <gtest/gtest.h>

#include <QMap>
#include <cmath>

#include "effects/backends/builtin/colordubechoeffect.h"
#include "effects/backends/builtin/colornoiseeffect.h"
#include "effects/backends/builtin/flangereffect.h"
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
    ColorFxHarness(int sampleRate = 48000, int framesPerBuffer = kFramesPerBuffer)
            : m_engineParameters(mixxx::audio::SampleRate(sampleRate), framesPerBuffer),
              m_pManifest(Effect::getManifest()),
              m_pColor(new EngineEffectParameter(
                      m_pManifest->parameters().first())),
              m_state(m_engineParameters),
              m_input(m_engineParameters.samplesPerBuffer()),
              m_output(m_engineParameters.samplesPerBuffer()) {
        for (const auto& manifest : m_pManifest->parameters()) {
            m_parameters.insert(manifest->id(), manifest->id() == m_pColor->id() ?
                    m_pColor : EngineEffectParameterPointer(new EngineEffectParameter(manifest)));
        }
        m_effect.loadEngineEffectParameters(m_parameters);
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

    void setParameter(const QString& id, double value) {
        m_parameters.value(id)->setValue(value);
    }

    void process(double color, EffectEnableState enable = EffectEnableState::Enabled) {
        m_pColor->setValue(color);
        m_effect.processChannel(&m_state,
                m_input.data(),
                m_output.data(),
                m_engineParameters,
                enable,
                m_features);
    }

  private:
    const mixxx::EngineParameters m_engineParameters;
    EffectManifestPointer m_pManifest;
    EngineEffectParameterPointer m_pColor;
    Effect m_effect;
    State m_state;
    QMap<QString, EngineEffectParameterPointer> m_parameters;
    GroupFeatureState m_features;
    mixxx::SampleBuffer m_input;
    mixxx::SampleBuffer m_output;
};

using DubEchoHarness = ColorFxHarness<ColorDubEchoEffect, ColorDubEchoGroupState>;
using NoiseHarness = ColorFxHarness<ColorNoiseEffect, ColorNoiseGroupState>;
using FlangerHarness = ColorFxHarness<FlangerEffect, FlangerGroupState>;

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

TEST(ColorFxEffectTest, DubEchoUsesDefaultMillisecondsAndKeepsStereoSeparate) {
    for (const double color : {0.0, 1.0}) {
      for (const int sampleRate : {44100, 48000, 96000}) {
        DubEchoHarness harness(sampleRate);
        harness.setBeatLengthSeconds(0.5);
        const SINT delayFrames = static_cast<SINT>((color == 0 ? 0.189 : 0.150) * sampleRate);
        SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
        for (int warmup = 0; warmup < 50; ++warmup) harness.process(color);
        const SINT impulseFrames = 64;
        SINT frame = 0;
        SINT firstEchoFrame = -1;
        while (frame < delayFrames + 4 * kFramesPerBuffer) {
            const SINT bufferStart = frame;
            for (SINT i = 0; i < harness.samplesPerBuffer();
                    i += mixxx::kEngineChannelCount) {
                const CSAMPLE value = frame++ < impulseFrames ? 0.5f : 0.0f;
                harness.input()[i] = value;
                harness.input()[i + 1] = 0;
            }
            harness.process(color);
            for (SINT i = 0; i < harness.samplesPerBuffer();
                    i += mixxx::kEngineChannelCount) {
                const SINT outputFrame = bufferStart + i / mixxx::kEngineChannelCount;
                const CSAMPLE wet = harness.output()[i] - harness.input()[i];
                ASSERT_TRUE(std::isfinite(wet));
                ASSERT_EQ(harness.output()[i + 1], 0);
                if (outputFrame < delayFrames - 1) {
                    ASSERT_EQ(wet, 0.0f) << "early echo at frame " << outputFrame;
                } else if (firstEchoFrame < 0 && wet != 0.0f) {
                    firstEchoFrame = outputFrame;
                }
            }
        }
        // The color ramps away from the dry center during the first buffer,
        // so the earliest frames may send almost nothing into the delay.
        EXPECT_GE(firstEchoFrame, delayFrames - 1) << "color " << color;
        EXPECT_LE(firstEchoFrame, delayFrames + 1) << "color " << color;
      }
    }
}

TEST(ColorFxEffectTest, DubEchoCenterRetainsTailAndIgnoresTempo) {
    DubEchoHarness a, b;
    a.setBeatLengthSeconds(0.25);
    b.setBeatLengthSeconds(1.0);
    SINT frame = 0;
    for (int block = 0; block < 80; ++block) {
        fillSine(a.input(), a.samplesPerBuffer(), &frame);
        SampleUtil::copy(b.input(), a.input(), a.samplesPerBuffer());
        a.process(1); b.process(1);
        for (int i = 0; i < a.samplesPerBuffer(); ++i) ASSERT_EQ(a.output()[i], b.output()[i]);
    }
    SampleUtil::clear(a.input(), a.samplesPerBuffer());
    double tail = 0;
    for (int block = 0; block < 50; ++block) {
        a.process(0.5);
        if (block > 20) {
            for (int i = 0; i < a.samplesPerBuffer(); ++i) tail += std::abs(a.output()[i]);
        }
    }
    EXPECT_GT(tail, 0.001);
    a.process(0.5, EffectEnableState::Disabling);
    a.process(0.5);
    for (int i = 0; i < a.samplesPerBuffer(); ++i) EXPECT_NEAR(a.output()[i], 0, 1e-7);
}

TEST(ColorFxEffectTest, DubEchoHasOnlyOneExternalParameter) {
    const auto parameters = ColorDubEchoEffect::getManifest()->parameters();
    ASSERT_EQ(parameters.size(), 1);
    EXPECT_EQ(parameters.first()->id(), QStringLiteral("color"));
}

TEST(ColorFxEffectTest, FlangerLinearFeedbackAndFractionalDelay) {
    for (double manual : {1.0, 1.01}) {
        FlangerHarness harness;
        harness.setParameter("width", 0);
        harness.setParameter("manual", manual);
        harness.setParameter("regen", 0.266666591);
        SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
        for (int warmup = 0; warmup < 20; ++warmup) harness.process(0.75);
        harness.input()[0] = 2;
        harness.process(0.75);
        EXPECT_NEAR(harness.output()[0], 1.5, 1e-6);
        const double frac = manual * 48 - 48;
        EXPECT_NEAR(harness.output()[48 * 2], 1.5 * (1 - frac), 1e-5);
        EXPECT_NEAR(harness.output()[49 * 2], 1.5 * frac, 1e-5);
        if (manual == 1.0) {
            EXPECT_NEAR(harness.output()[96 * 2], 1.5 * 0.266666591, 1e-5);
        }
        for (int i = 1; i < harness.samplesPerBuffer(); i += 2) ASSERT_EQ(harness.output()[i], 0);
    }
}

TEST(ColorFxEffectTest, FlangerDryAndDisableReset) {
    FlangerHarness harness;
    harness.setParameter("mix", 0);
    SINT frame = 0;
    fillSine(harness.input(), harness.samplesPerBuffer(), &frame);
    harness.process(0.75);
    for (int i = 0; i < harness.samplesPerBuffer(); ++i) ASSERT_EQ(harness.output()[i], harness.input()[i]);
    harness.process(0.75, EffectEnableState::Disabling);
    SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
    harness.setParameter("mix", 1);
    harness.process(0.75);
    for (int i = 0; i < harness.samplesPerBuffer(); ++i) ASSERT_EQ(harness.output()[i], 0);
}

} // namespace

TEST(FxModulationTest, DefaultSweepQuarterPointsAndFractionalBeatAtAllRates) {
    for (int rate : {44100, 48000, 96000}) {
        const mixxx::EngineParameters engine(mixxx::audio::SampleRate(rate), 257);
        FlangerGroupState state(engine);
        FlangerEffect effect;
        QMap<QString, EngineEffectParameterPointer> parameters;
        const auto manifest = FlangerEffect::getManifest();
        for (const auto& p : manifest->parameters()) {
            parameters.insert(p->id(), EngineEffectParameterPointer(new EngineEffectParameter(p)));
        }
        parameters.value("speed")->setValue(.75);
        parameters.value("regen")->setValue(0);
        parameters.value("mix")->setValue(1);
        effect.loadEngineEffectParameters(parameters);
        GroupFeatureState features;
        features.beat_length = GroupFeatureBeatLength{.5, 1.0};
        mixxx::SampleBuffer input(engine.samplesPerBuffer()), output(engine.samplesPerBuffer());
        const int period = static_cast<int>(rate * .375);
        const double maxDelay = 2.6666667461395264 * rate / 1000;
        // Standard triangle: these points are 1/4, 3/4, 3/4, 1/4 through its delay range.
        const double fractions[] = {.25, .75, .75, .25};
        int checks = 0;
        for (int start = 0; start < 2 * period; start += 257) {
            for (int frame = 0; frame < 257; ++frame) {
                input[2 * frame] = (start + frame) * 1e-6f;
                input[2 * frame + 1] = 0;
            }
            effect.processChannel(&state, input.data(), output.data(), engine,
                    EffectEnableState::Enabled, features);
            for (int point = 0; point < 4; ++point) {
                const int n = period + (2 * point + 1) * period / 8;
                if (start <= n && n < start + 257) {
                    const double inferredDelay = n -
                            (output[2 * (n - start)] / .75 - input[2 * (n - start)]) / 1e-6;
                    const double expectedDelay = 1 + fractions[point] * (maxDelay - 1);
                    EXPECT_NEAR(inferredDelay, expectedDelay, .12) << "rate=" << rate << " point=" << point;
                    EXPECT_EQ(output[2 * (n - start) + 1], 0);
                    ++checks;
                }
            }
        }
        EXPECT_EQ(checks, 4);
    }
}
