#include <gtest/gtest.h>

#include <QMap>
#include <cmath>
#include <vector>
#include <thread>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

#include "control/controlobject.h"
#include "effects/backends/builtin/beatfxeffect.h"
#include "effects/backends/builtin/colorcrusheffect.h"
#include "effects/backends/builtin/reverbeffect.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectparameter.h"
#include "effects/effectknobparameterslot.h"
#include "effects/presets/effectparameterpreset.h"
#include "engine/effects/engineeffect.h"
#include "engine/effects/engineeffectchain.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
template<typename Effect, typename State>
struct Harness {
    mixxx::EngineParameters engine;
    Effect effect;
    State state;
    QMap<QString, EngineEffectParameterPointer> parameters;
    GroupFeatureState features;
    mixxx::SampleBuffer input, output;
    Harness(int rate = 48000, int frames = 257)
            : engine(mixxx::audio::SampleRate(rate), frames), state(engine),
              input(engine.samplesPerBuffer()), output(engine.samplesPerBuffer()) {
        const auto manifest = Effect::getManifest();
        for (const auto& p : manifest->parameters()) {
            parameters.insert(p->id(), EngineEffectParameterPointer(new EngineEffectParameter(p)));
        }
        effect.loadEngineEffectParameters(parameters);
        features.beat_length = GroupFeatureBeatLength{0.5, 1};
        input.clear(); output.clear();
    }
    void set(const QString& key, double value) { parameters.value(key)->setValue(value); }
    void process(EffectEnableState enable = EffectEnableState::Enabled) {
        effect.processChannel(&state, input.data(), output.data(), engine, enable, features);
    }
    float peak() const {
        float value = 0;
        for (SINT i = 0; i < output.size(); ++i) value = std::max(value, std::abs(output[i]));
        return value;
    }
};
using Delay = Harness<BeatDelayEffect, BeatDelayState>;
using Echo = Harness<BeatEchoEffect, BeatDelayState>;
using Spiral = Harness<SpiralEffect, SpiralState>;
using Roll = Harness<RollEffect, RollState>;
using Crush = Harness<ColorCrushEffect, ColorCrushGroupState>;
using Reverb = Harness<ReverbEffect, ReverbGroupState>;

TEST(BeatFxEffectTest, EchoAndReverbStayBelowFullScaleWithSustainedInput) {
    for (int rate : {44100, 48000, 96000}) {
      for (double depth : {.5, .75, 1.}) {
        Echo echo(rate, 257); Reverb reverb(rate, 257);
        echo.set("depth", depth); echo.set("delay_time", .125);
        reverb.set("send_amount", depth); reverb.set("decay", 1);
        float peak=0;
        for (int block=0; block<static_cast<int>(rate*6/257); ++block) {
            for (int f=0;f<257;++f) {
                const double phase=2*3.14159265358979323846*96*(block*257+f)/rate;
                echo.input[2*f]=reverb.input[2*f]=.9f*static_cast<float>(std::sin(phase));
                echo.input[2*f+1]=reverb.input[2*f+1]=.45f*static_cast<float>(std::sin(phase));
            }
            echo.process(); reverb.process();
            peak=std::max(peak,echo.peak());
            ASSERT_LE(echo.peak(), .980001f);
            ASSERT_LE(reverb.peak(), .980001f);
            for (int i=0;i<echo.output.size();i+=2)
                ASSERT_NEAR(echo.output[i+1], echo.output[i]*.5f, 1e-6);
        }
        EXPECT_GT(peak,.5f);
      }
    }
}

TEST(BeatFxEffectTest, LimiterIsTransparentBelowThresholdAndReleasesAcrossCallbacks) {
    performance_fx::PeakLimiter limiter;
    limiter.configure(48000);
    float quiet[]{.2f,-.1f}; limiter.process(quiet);
    EXPECT_FLOAT_EQ(quiet[0],.2f); EXPECT_FLOAT_EQ(quiet[1],-.1f);
    float loud[]{4,-2}; limiter.process(loud);
    EXPECT_NEAR(loud[0],.98f,1e-7); EXPECT_NEAR(loud[1],-.49f,1e-7);
    const float gain=limiter.gain;
    for(int i=0;i<2400;++i) { float zero[]{0,0}; limiter.process(zero); }
    EXPECT_GT(limiter.gain,gain); EXPECT_LT(limiter.gain,1);
}

TEST(BeatFxEffectTest, ReverbUsesReferenceTimeAndOwnsItsDryWetMix) {
    const auto manifest = ReverbEffect::getManifest();
    EXPECT_FALSE(manifest->addDryToWet());
    EXPECT_DOUBLE_EQ(manifest->metaknobDefault(), .5);
    EXPECT_DOUBLE_EQ(manifest->parameters()[0]->getDefault(), .5);
    EXPECT_NEAR(ReverbEffect::timeSeconds(0), 1.5318088, 1e-6);
    EXPECT_NEAR(ReverbEffect::timeSeconds(.5), 8.7009239, 1e-6);
    EXPECT_NEAR(ReverbEffect::timeSeconds(1), 100.63781, 1e-5);
    EXPECT_EQ(ReverbEffect::timeSeconds(124./255), ReverbEffect::timeSeconds(131./255));
    EXPECT_EQ(ReverbEffect::timeSeconds(-1), ReverbEffect::timeSeconds(0));
    EXPECT_EQ(ReverbEffect::timeSeconds(2), ReverbEffect::timeSeconds(1));
    // Original 142db40b0 ISA oracle + 142df70b0 input-level instructions.
    EXPECT_DOUBLE_EQ(ReverbEffect::inputLevel(0), .2);
    EXPECT_DOUBLE_EQ(ReverbEffect::inputLevel(.5), .2);
    EXPECT_NEAR(ReverbEffect::inputLevel(132./255), .1991935484111309, 1e-8);
    EXPECT_NEAR(ReverbEffect::inputLevel(192./255), .1508064568042755, 1e-8);
    EXPECT_DOUBLE_EQ(ReverbEffect::inputLevel(1), .1);
    for (int i = 0; i <= 1000; ++i) {
        EXPECT_GT(ReverbEffect::timeSeconds(i/1000.), .8);
        EXPECT_LE(ReverbEffect::timeSeconds(i/1000.), 101);
    }
}

TEST(BeatFxEffectTest, CrossThreadFxParameterWaitsForTheGuiEventQueue) {
    const QString group = "[FxDispatchProbe]";
    auto manifest = BeatEchoEffect::getManifest()->parameters()[1];
    EffectParameterPointer parameter(new EffectParameter(nullptr, {}, manifest, EffectParameterPreset()));
    EffectKnobParameterSlot slot(group, 0);
    slot.loadParameter(parameter);
    const ConfigKey key(group, "parameter1");
    ASSERT_DOUBLE_EQ(parameter->getValue(), .5);
    QElapsedTimer timer;
    timer.start();
    std::thread controller([&] { ControlObject::set(key, 1); });
    controller.join();
    EXPECT_DOUBLE_EQ(ControlObject::get(key), 1);
    // Deliberately model a busy GUI; the atomic CO already has the new value.
    QThread::msleep(80);
    EXPECT_DOUBLE_EQ(parameter->getValue(), .5);
    QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(parameter->getValue(), 1);
    qInfo() << "FX_DISPATCH_GUI_GATE_MS" << timer.elapsed();
}

TEST(BeatFxEffectTest, ReverbFullLevelRemovesDryAndMiddleMatchesReferenceGains) {
    for (int rate : {44100, 48000, 96000}) {
        Reverb dry(rate), middle(rate), wet(rate);
        dry.set("send_amount", 0); middle.set("send_amount", .5); wet.set("send_amount", 1);
        for (int b = 0; b < 8; ++b) { dry.process(); middle.process(); wet.process(); }
        dry.input[0] = middle.input[0] = wet.input[0] = .125f;
        dry.input[1] = middle.input[1] = wet.input[1] = .0625f;
        for (int b = 0; b < 100; ++b) {
            dry.process(); middle.process(); wet.process();
            if (!b) EXPECT_EQ(wet.output[0], 0);
            for (int i = 0; i < dry.output.size(); ++i) {
                EXPECT_FLOAT_EQ(dry.output[i], dry.input[i]);
                EXPECT_NEAR(middle.output[i], dry.input[i] + wet.output[i]*(2.f/3), 1e-6);
                ASSERT_TRUE(std::isfinite(wet.output[i]));
            }
            dry.input.clear(); middle.input.clear(); wet.input.clear();
        }
    }
}

TEST(BeatFxEffectTest, ReverbLevelControlsTheExistingTailWithoutRefillingTheNetwork) {
    Reverb h(44100, 512);
    h.set("send_amount", 0);
    for (int b = 0; b < 90; ++b) {
        for (int f = 0; f < 512; ++f) {
            h.input[2*f] = h.input[2*f+1] = .05f*std::sin((b*512+f)*.027);
        }
        h.process();
    }
    h.input.clear(); h.set("send_amount", 1); h.process();
    EXPECT_GT(h.peak(), 1e-4);
    for (int b=0; b<8; ++b) h.process(EffectEnableState::Disabling);
    EXPECT_EQ(h.peak(), 0);
    h.process(EffectEnableState::Enabling);
    EXPECT_EQ(h.peak(), 0);
}

template<typename H>
std::vector<float> impulse(int rate, int block, double beats, int total) {
    H h(rate, block);
    h.set("delay_time", beats);
    for (int b = 0; b < 8; ++b) h.process();
    std::vector<float> result;
    while (static_cast<int>(result.size()) < total) {
        h.input.clear();
        if (result.empty()) h.input[0] = .3f;
        h.process();
        for (int f = 0; f < block; ++f) {
            result.push_back(h.output[2 * f]);
            EXPECT_EQ(h.output[2 * f + 1], 0);
        }
    }
    result.resize(total);
    return result;
}

TEST(BeatFxEffectTest, DelayAndEchoHaveIndependentManifestsWithDefaultAuxiliaries) {
    EXPECT_NE(BeatDelayEffect::getId(), BeatEchoEffect::getId());
    for (const auto& m : {BeatDelayEffect::getManifest(), BeatEchoEffect::getManifest(),
                 SpiralEffect::getManifest(), RollEffect::getManifest()}) {
        ASSERT_EQ(m->parameters().size(), 2);
        EXPECT_EQ(m->parameters()[0]->id(), "delay_time");
        EXPECT_DOUBLE_EQ(m->parameters()[0]->getDefault(), 1);
        EXPECT_DOUBLE_EQ(m->parameters()[1]->getDefault(), .5);
    }
    EXPECT_EQ(ColorCrushEffect::getManifest()->parameters().size(), 1);
}

TEST(BeatFxEffectTest, DelayTimesAreAudiblyDifferentAtAllSupportedRates) {
    for (int rate : {44100, 48000, 96000}) {
        for (double beats : {.25, .5, 1.0, 2.0}) {
            const int n = static_cast<int>(std::lround(rate * .5 * beats));
            const auto pcm = impulse<Delay>(rate, 257, beats, n + 100);
            EXPECT_FLOAT_EQ(pcm[0], .3f);
            for (int i = 1; i < n; ++i) ASSERT_EQ(pcm[i], 0);
            EXPECT_NEAR(pcm[n], .2, 1e-6);
        }
    }
}

TEST(BeatFxEffectTest, EchoRepeatsWhileDelayHasNoFeedbackAndDoesNotLeakChannels) {
    const int n = 3000;
    const auto echo = impulse<Echo>(48000, 257, .125, 3 * n + 100);
    const auto delay = impulse<Delay>(48000, 257, .125, 3 * n + 100);
    EXPECT_NEAR(echo[n], .2, 1e-6);
    EXPECT_NEAR(echo[2 * n], .14, 2e-6);
    EXPECT_NEAR(echo[3 * n], .098, 2e-6);
    EXPECT_LT(std::abs(delay[2 * n]), 2e-6);
}

TEST(BeatFxEffectTest, EchoIsIndependentOfCallbackPartition) {
    EXPECT_EQ(impulse<Echo>(44100, 127, .25, 20000),
            impulse<Echo>(44100, 512, .25, 20000));
}

TEST(BeatFxEffectTest, DisabledDelayStillRecordsAndReenablePreservesHistory) {
    Delay h(48000, 1);
    h.set("delay_time", .125);
    for (int i = 0; i < 200; ++i) h.process(EffectEnableState::Disabled);
    h.input[0] = .3f;
    h.process(EffectEnableState::Disabled);
    h.input.clear();
    for (int i = 1; i < 3000; ++i) h.process(EffectEnableState::Enabled);
    h.process();
    EXPECT_NEAR(h.output[0], .2, 1e-6);
}

TEST(BeatFxEffectTest, OffEchoRetainsTailWhileOffDelayIsDry) {
    Echo echo; Delay delay;
    echo.set("delay_time", .125); delay.set("delay_time", .125);
    for (int b = 0; b < 4; ++b) { echo.process(); delay.process(); }
    echo.input[0] = delay.input[0] = .3f;
    echo.process(); delay.process();
    echo.input.clear(); delay.input.clear();
    float echoPeak = 0, delayPeak = 0;
    for (int b = 0; b < 35; ++b) {
        echo.process(EffectEnableState::Disabled); delay.process(EffectEnableState::Disabled);
        echoPeak = std::max(echoPeak, echo.peak()); delayPeak = std::max(delayPeak, delay.peak());
    }
    EXPECT_GT(echoPeak, .19f); EXPECT_EQ(delayPeak, 0);
    EXPECT_TRUE(echo.state.hasTail());
}

TEST(BeatFxEffectTest, LoadingDisabledEchoDoesNotCreatePhantomRepeats) {
    Echo h; h.set("delay_time", .125);
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < h.input.size(); ++i) h.input[i] = .2f;
        h.process(EffectEnableState::Disabled);
        for (int i = 0; i < h.input.size(); ++i) EXPECT_EQ(h.output[i], h.input[i]);
    }
    h.input.clear();
    for (int b = 0; b < 40; ++b) {
        h.process(EffectEnableState::Disabled); EXPECT_EQ(h.peak(), 0);
    }
    EXPECT_FALSE(h.state.hasTail());
}

TEST(BeatFxEffectTest, SpiralAddsDefaultPitchLatencyAndRetainsOffTail) {
    Spiral h(48000, 257); h.set("delay_time", .125);
    for (int b = 0; b < 8; ++b) h.process();
    h.input[0] = h.input[1] = .3f; h.process(); h.input.clear();
    int first = -1; float tail = 0;
    for (int b = 1; b < 40; ++b) {
        h.process(EffectEnableState::Disabled);
        for (int f = 0; f < 257; ++f) {
            if (first < 0 && std::abs(h.output[2 * f]) > 1e-7) first = b * 257 + f;
        }
        tail = std::max(tail, h.peak());
    }
    EXPECT_GE(first, 4438); EXPECT_LE(first, 4450);
    EXPECT_GT(tail, .01); EXPECT_TRUE(h.state.hasTail());
}

TEST(BeatFxEffectTest, SpiralTimeChangeMovesReadHeadInsteadOfSnapping) {
    Spiral h; h.process(); const double old = h.state.delay;
    h.set("delay_time", .25); h.process();
    EXPECT_LT(h.state.delay, old); EXPECT_GT(h.state.delay, 6001);
    for (int b = 0; b < 3000; ++b) {
        h.process(); ASSERT_TRUE(std::isfinite(h.state.delay));
        ASSERT_GE(h.state.delay, 48); ASSERT_LE(h.state.velocity, .8);
    }
    EXPECT_NEAR(h.state.delay, 6000, 1);
}

TEST(BeatFxEffectTest, SpiralUsesNativeShorteningAccelerationAndNearTargetBrakes) {
    Spiral h;
    h.process(); h.set("delay_time", .25); h.process();
    // Mode-0 48 kHz trace calculated from the sealed native instructions:
    // error=-18000, target speed=-.1961764678, shortening coefficient=10/(3*fs).
    EXPECT_NEAR(h.state.delay, 23999.552734375, .002);
    EXPECT_NEAR(h.state.velocity, -.0035012050066143274, 1e-8);
    for (const double direction : {-1.0, 1.0}) {
        Spiral brake(48000, 1); brake.process();
        brake.state.delay = 24000 - direction * 20;
        brake.state.velocity = direction * .1;
        brake.process();
        EXPECT_NEAR(brake.state.velocity, direction * .0955, 1e-7);
        EXPECT_NEAR(brake.state.desiredVelocity, direction * .01, 1e-9);
    }
}

TEST(BeatFxEffectTest, RollLoopsFullBeatThroughPauseAndClearsOnRetrigger) {
    Roll h(48000, 256); h.set("delay_time", .125);
    for (int b = 0; b < 30; ++b) {
        for (int i = 0; i < h.input.size(); ++i) h.input[i] = .2f;
        h.process();
    }
    EXPECT_TRUE(h.state.hasTail()); EXPECT_EQ(h.state.length, 3000);
    h.input.clear();
    for (int b = 0; b < 30; ++b) { h.process(); EXPECT_GT(h.peak(), .19); }
    h.process(EffectEnableState::Disabling);
    for (int b = 0; b < 30; ++b) h.process(EffectEnableState::Disabled);
    EXPECT_EQ(h.peak(), 0); EXPECT_FALSE(h.state.hasTail());
    h.process(EffectEnableState::Enabling);
    for (int b = 0; b < 30; ++b) { h.process(); EXPECT_EQ(h.peak(), 0); }
    EXPECT_FALSE(h.state.hasTail());
}

TEST(BeatFxEffectTest, RollTimeChangeAppliesAtLoopBoundary) {
    Roll h(48000, 1); h.set("delay_time", .125);
    h.input[0] = h.input[1] = .2f;
    for (int i = 0; i < 3100; ++i) h.process();
    EXPECT_EQ(h.state.length, 3000);
    h.set("delay_time", .25); h.process(); EXPECT_EQ(h.state.length, 3000);
    for (int i = 0; i < 3000; ++i) h.process();
    EXPECT_EQ(h.state.length, 6000);
}

TEST(BeatFxEffectTest, CrushDefaultParameterDoesNotQuantizePcmAndCenterIsExact) {
    Crush h;
    h.set("color", 0);
    for (int b = 0; b < 20; ++b) {
        for (int f = 0; f < 257; ++f) {
            h.input[2 * f] = .1234567f; h.input[2 * f + 1] = -.2345678f;
        }
        h.process();
    }
    EXPECT_FLOAT_EQ(h.output[400], .1234567f);
    EXPECT_FLOAT_EQ(h.output[401], -.2345678f);
    h.set("color", .5);
    for (int b = 0; b < 12; ++b) h.process();
    for (int i = 0; i < h.input.size(); ++i) EXPECT_EQ(h.output[i], h.input[i]);
}

TEST(BeatFxEffectTest, CrushSampleHoldIsStereoAndFiniteAcrossColorSweep) {
    Crush h;
    double previous = 0;
    for (double color : {0.0, 1.0, .25, .75, .5}) {
        h.set("color", color);
        int repeated = 0;
        for (int b = 0; b < 50; ++b) {
            for (int f = 0; f < 257; ++f) {
                h.input[2 * f] = static_cast<float>(.1 * std::sin((b * 257 + f) * .017));
                h.input[2 * f + 1] = -h.input[2 * f];
            }
            h.process();
            for (int f = 0; f < 257; ++f) {
                ASSERT_TRUE(std::isfinite(h.output[2 * f]));
                ASSERT_LT(std::abs(h.output[2 * f]), 3);
                EXPECT_NEAR(h.output[2 * f], -h.output[2 * f + 1], 1e-6);
                if (h.output[2 * f] == previous) ++repeated;
                previous = h.output[2 * f];
            }
        }
        if (color == 0) {
            EXPECT_GT(repeated, 10000);
        }
    }
}

TEST(BeatFxEffectTest, FullEngineChainProcessesDisabledEchoAndPausedTail) {
    EffectsBackendManagerPointer backend(new EffectsBackendManager());
    ChannelHandleFactory handles;
    const QString group = QStringLiteral("[Channel1]");
    const auto handle = handles.getOrCreateHandle(group);
    const QSet<ChannelHandleAndGroup> channels{ChannelHandleAndGroup(handle, group)};
    auto manifest = backend->getManifest(BeatEchoEffect::getId(), EffectBackendType::BuiltIn);
    ASSERT_TRUE(manifest);
    EngineEffect effect(manifest, backend, channels, channels, channels);
    EngineEffectChain chain(QStringLiteral("[EffectRack1_EffectUnit1]"), channels, channels);
    auto pipes = TwoWayMessagePipe<EffectsRequest*, EffectsResponse>::makeTwoWayMessagePipe(32, 32);
    auto* responses = pipes.second.get();
    EffectsRequest request;
    request.type = EffectsRequest::ADD_EFFECT_TO_CHAIN;
    request.AddEffectToChain.pEffect = &effect; request.AddEffectToChain.iIndex = 0;
    ASSERT_TRUE(chain.processEffectsRequest(request, responses));
    request.type = EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS;
    request.SetEffectChainParameters.enabled = true;
    request.SetEffectChainParameters.mix_mode = EffectChainMixMode::DrySlashWet;
    request.SetEffectChainParameters.mix = 1;
    ASSERT_TRUE(chain.processEffectsRequest(request, responses));
    request.type = EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL;
    request.EnableInputChannelForChain.channelHandle = handle;
    ASSERT_TRUE(chain.processEffectsRequest(request, responses));
    request.type = EffectsRequest::SET_EFFECT_PARAMETERS;
    request.SetEffectParameters.enabled = true;
    ASSERT_TRUE(effect.processEffectsRequest(request, responses));
    request.type = EffectsRequest::SET_PARAMETER_PARAMETERS;
    request.SetParameterParameters.iParameter = 0; request.value = .125;
    ASSERT_TRUE(effect.processEffectsRequest(request, responses));
    mixxx::SampleBuffer input(512), output(512); input.clear(); output.clear();
    GroupFeatureState features; features.beat_length = GroupFeatureBeatLength{.5, 1};
    for (int b = 0; b < 4; ++b) chain.process(handle, handle, input.data(), output.data(),
            512, mixxx::audio::SampleRate(48000), features, false);
    input[0] = .3f;
    chain.process(handle, handle, input.data(), output.data(), 512,
            mixxx::audio::SampleRate(48000), features, false);
    input.clear(); EXPECT_TRUE(chain.hasTail(handle));
    request.type = EffectsRequest::SET_EFFECT_PARAMETERS; request.SetEffectParameters.enabled = false;
    ASSERT_TRUE(effect.processEffectsRequest(request, responses));
    float peak = 0;
    for (int b = 0; b < 35; ++b) {
        ASSERT_TRUE(chain.process(handle, handle, input.data(), output.data(), 512,
                mixxx::audio::SampleRate(48000), features, true));
        for (int i = 0; i < 512; ++i) peak = std::max(peak, std::abs(output[i]));
    }
    EXPECT_GT(peak, .19);
    EXPECT_TRUE(chain.hasTail(handle));
}
} // namespace
