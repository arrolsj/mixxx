#ifdef __RUBBERBAND__

#include "effects/backends/builtin/pitchshifteffect.h"

#include <gtest/gtest.h>

#include <QMap>
#include <cmath>
#include <vector>

#include "engine/effects/engineeffectparameter.h"
#include "util/samplebuffer.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

struct PitchHarness {
    mixxx::EngineParameters engine;
    PitchShiftEffect effect;
    PitchShiftGroupState state;
    QMap<QString, EngineEffectParameterPointer> parameters;
    GroupFeatureState features;
    mixxx::SampleBuffer input, output;
    double phase = 0;

    explicit PitchHarness(int rate = 44100, int frames = 512)
            : engine(mixxx::audio::SampleRate(rate), frames),
              state(engine),
              input(engine.samplesPerBuffer()),
              output(engine.samplesPerBuffer()) {
        const auto manifest = PitchShiftEffect::getManifest();
        for (const auto& p : manifest->parameters()) {
            parameters.insert(p->id(), EngineEffectParameterPointer(new EngineEffectParameter(p)));
        }
        effect.loadEngineEffectParameters(parameters);
        input.clear();
        output.clear();
    }
    void set(const QString& key, double value) {
        parameters.value(key)->setValue(value);
    }
    void process(EffectEnableState enable = EffectEnableState::Enabled) {
        effect.processChannel(&state, input.data(), output.data(), engine, enable, features);
    }
    void fillSine(double hz) {
        const int frames = static_cast<int>(engine.framesPerBuffer());
        for (int i = 0; i < frames; ++i) {
            const float value = static_cast<float>(0.5 * std::sin(phase));
            input[2 * i] = value;
            input[2 * i + 1] = value;
            phase += 2 * kPi * hz / engine.sampleRate();
        }
    }
    /// Runs a sine through the effect and returns the left output channel.
    std::vector<float> run(double hz, int totalFrames) {
        std::vector<float> result;
        bool first = true;
        while (static_cast<int>(result.size()) < totalFrames) {
            fillSine(hz);
            process(first ? EffectEnableState::Enabling : EffectEnableState::Enabled);
            first = false;
            for (SINT f = 0; f < engine.framesPerBuffer(); ++f) {
                result.push_back(output[2 * f]);
            }
        }
        result.resize(totalFrames);
        return result;
    }
};

double risingCrossingsPerSecond(const std::vector<float>& signal, int from, int rate) {
    int count = 0;
    for (size_t i = from + 1; i < signal.size(); ++i) {
        if (signal[i - 1] < 0 && signal[i] >= 0) {
            ++count;
        }
    }
    return count * static_cast<double>(rate) / static_cast<double>(signal.size() - from);
}

TEST(PitchShiftEffectTest, ManifestExposesPitchKnobFirstAndLinkedLevel) {
    const auto manifest = PitchShiftEffect::getManifest();
    ASSERT_EQ(manifest->parameters().size(), 2);
    const auto pitch = manifest->parameters().at(0);
    const auto depth = manifest->parameters().at(1);
    EXPECT_EQ(pitch->id(), QStringLiteral("rkb_pitch"));
    EXPECT_EQ(pitch->getDefault(), 0.0);
    EXPECT_EQ(pitch->getMinimum(), -1.0);
    EXPECT_EQ(pitch->getMaximum(), 1.0);
    EXPECT_EQ(pitch->defaultLinkType(), EffectManifestParameter::LinkType::None);
    EXPECT_EQ(depth->id(), QStringLiteral("depth"));
    EXPECT_EQ(depth->getDefault(), 0.5);
    EXPECT_EQ(depth->defaultLinkType(), EffectManifestParameter::LinkType::Linked);
}

TEST(PitchShiftEffectTest, ControlFollowsNativeMappingAndTruncation) {
    EXPECT_EQ(PitchShiftEffect::nativeControl(0.0, 0.5), 0.0f);
    EXPECT_EQ(PitchShiftEffect::nativeControl(1.0, 0.5), 100.0f);
    EXPECT_EQ(PitchShiftEffect::nativeControl(-1.0, 0.5), -100.0f);
    EXPECT_EQ(PitchShiftEffect::nativeControl(1.0, 1.0), 200.0f);
    EXPECT_EQ(PitchShiftEffect::nativeControl(-1.0, 1.0), -200.0f);
    // 200 * 0.0049 * 0.5 = 0.49 truncates to 0 like the native int parameter.
    EXPECT_EQ(PitchShiftEffect::nativeControl(0.0049, 0.5), 0.0f);
    EXPECT_EQ(PitchShiftEffect::nativeControl(-0.0149, 1.0), -2.0f);

    using lastcue::RbxPitchShifter;
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(-300), -1.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(-200), -1.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(-100), -0.5f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(0), 0.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(50), 0.5f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(100), 1.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(150), 2.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(200), 3.0f);
    EXPECT_FLOAT_EQ(RbxPitchShifter::offsetForControl(300), 3.0f);
}

TEST(PitchShiftEffectTest, CenteredKnobPassesInputWithoutLatency) {
    PitchHarness h;
    h.set("rkb_pitch", 0.0);
    bool first = true;
    for (int block = 0; block < 20; ++block) {
        h.fillSine(330);
        h.process(first ? EffectEnableState::Enabling : EffectEnableState::Enabled);
        first = false;
        for (SINT i = 0; i < h.output.size(); ++i) {
            ASSERT_EQ(h.output[i], h.input[i]) << "block " << block << " sample " << i;
        }
    }
}

TEST(PitchShiftEffectTest, StartupPassesInputUntilThirtyMillisecondsAreBuffered) {
    PitchHarness h(44100, 256);
    h.set("rkb_pitch", 1.0);
    const auto out = h.run(220, 1323);
    PitchHarness reference(44100, 256);
    std::vector<float> in;
    while (static_cast<int>(in.size()) < 1323) {
        reference.fillSine(220);
        for (int f = 0; f < 256; ++f) {
            in.push_back(reference.input[2 * f]);
        }
    }
    for (int i = 0; i < 1323; ++i) {
        ASSERT_EQ(out[i], in[i]) << i;
    }
}

TEST(PitchShiftEffectTest, KnobEndsShiftOneOctaveAtDefaultLevel) {
    for (const int rate : {44100, 48000}) {
        PitchHarness up(rate, 512);
        up.set("rkb_pitch", 1.0);
        const auto high = up.run(220, rate * 2);
        EXPECT_NEAR(risingCrossingsPerSecond(high, rate / 2, rate), 440, 440 * 0.03) << rate;

        PitchHarness down(rate, 512);
        down.set("rkb_pitch", -1.0);
        const auto low = down.run(440, rate * 2);
        EXPECT_NEAR(risingCrossingsPerSecond(low, rate / 2, rate), 220, 220 * 0.03) << rate;
    }
}

TEST(PitchShiftEffectTest, OutputStaysContinuousAcrossSplices) {
    PitchHarness h(44100, 512);
    h.set("rkb_pitch", 0.6);
    const auto out = h.run(110, 44100 * 2);
    // A 110 Hz sine shifted up stays well below a 0.25 sample-to-sample step
    // unless a block repeats or a splice jumps without its crossfade.
    float largest = 0;
    for (size_t i = 44100 / 2; i < out.size(); ++i) {
        largest = std::max(largest, std::abs(out[i] - out[i - 1]));
    }
    EXPECT_LT(largest, 0.25f);
}

TEST(PitchShiftEffectTest, LowestSpeedIsGatedToSilence) {
    PitchHarness h(44100, 512);
    h.set("rkb_pitch", -1.0);
    h.set("depth", 1.0);
    const auto out = h.run(220, 44100);
    float tail = 0;
    for (size_t i = out.size() - 4096; i < out.size(); ++i) {
        tail = std::max(tail, std::abs(out[i]));
    }
    EXPECT_EQ(tail, 0.0f);
}

TEST(PitchShiftEffectTest, SilentInputNeverRepeatsStaleAudio) {
    PitchHarness h(44100, 512);
    h.set("rkb_pitch", 0.5);
    h.run(220, 44100);
    // After the signal stops, old audio may only drain from the 30 ms line.
    float late = 0;
    for (int block = 0; block < 40; ++block) {
        h.input.clear();
        h.process();
        if (block >= 10) {
            for (SINT i = 0; i < h.output.size(); ++i) {
                late = std::max(late, std::abs(h.output[i]));
            }
        }
    }
    EXPECT_EQ(late, 0.0f);
}

} // namespace

#endif // __RUBBERBAND__
