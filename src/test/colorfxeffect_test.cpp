#include <gtest/gtest.h>

#include <QMap>
#include <array>
#include <cmath>

#include "control/controlobject.h"
#include "effects/backends/builtin/colordubechoeffect.h"
#include "effects/backends/builtin/colorfiltereffect.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "effects/backends/builtin/colornoiseeffect.h"
#include "effects/backends/builtin/flangereffect.h"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectsbackendmanager.h"
#include "engine/effects/engineeffect.h"
#include "engine/effects/engineeffectchain.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/engine.h"
#include "util/samplebuffer.h"
#include "effects/chains/quickeffectchain.h"
#include "effects/effectslot.h"
#include "test/signalpathtest.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(48000);
constexpr SINT kFramesPerBuffer = 256;
constexpr double kPi = 3.14159265358979323846;

class ColorNoiseSignalPathTest : public BaseSignalPathTest {};

TEST_F(ColorNoiseSignalPathTest, EmptyDeckNoiseObeysFaderMuteAndPfl) {
    ASSERT_FALSE(m_pChannel1->getEngineBuffer()->isTrackLoaded());
    m_pEffectsManager->addDeck(ChannelHandleAndGroup(m_pChannel1->getHandle(), m_sGroup1));
    auto chain=m_pEffectsManager->getQuickEffectChain(m_sGroup1);
    ASSERT_TRUE(chain);
    auto slot=chain->getEffectSlot(0);
    slot->loadEffectWithDefaults(m_pEffectsManager->getBackendManager()->getManifest(
            ColorNoiseEffect::getId(),EffectBackendType::BuiltIn));
    slot->setEnabled(true); slot->setMetaParameter(.25,true);
    ControlObject::set(ConfigKey(m_sGroup1,"volume"),1);
    ControlObject::set(ConfigKey(m_sMainGroup,"headMix"),-1);
    m_pChannel1->setPfl(true);
    auto process=[&] {
        for(int b=0;b<8;++b) { QCoreApplication::processEvents(); m_pEngineMixer->process(kProcessBufferSize); }
    };
    auto peak=[&](const float* buffer) {
        float p=0; for(int i=0;i<kProcessBufferSize;++i) p=std::max(p,std::abs(buffer[i])); return p;
    };
    process();
    EXPECT_TRUE(m_pChannel1->isActive());
    EXPECT_GT(peak(m_pEngineMixer->getMainBuffer()),1e-5);
    EXPECT_GT(peak(m_pEngineMixer->getHeadphoneBuffer()),1e-5);
    ControlObject::set(ConfigKey(m_sGroup1,"volume"),0); process();
    EXPECT_EQ(peak(m_pEngineMixer->getMainBuffer()),0);
    EXPECT_GT(peak(m_pEngineMixer->getHeadphoneBuffer()),1e-5);
    ControlObject::set(ConfigKey(m_sGroup1,"volume"),1);
    ControlObject::set(ConfigKey(m_sGroup1,"mute"),1); process();
    EXPECT_EQ(peak(m_pEngineMixer->getMainBuffer()),0);
    ControlObject::set(ConfigKey(m_sGroup1,"mute"),0);
    slot->setMetaParameter(.5,true); process();
    EXPECT_EQ(peak(m_pEngineMixer->getMainBuffer()),0);
    slot->setEnabled(false); process();
    EXPECT_FALSE(m_pChannel1->isActive());
}

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

    State& state() {
        return m_state;
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
using FilterHarness = ColorFxHarness<ColorFilterEffect, ColorFilterGroupState>;
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


// Frozen outputs of the RKB 7.2.19 native leaves; source identity and complete
// oracle live in outer docs/evidence/rkb-cfx-filter-noise-20261017/.
TEST(ColorFxEffectTest, FilterImpulseMatchesNativeCoefficientOracle) {
    struct Fixture { int code; std::array<double, 5> c; };
    const Fixture fixtures[] = {
        {160, {0.99446558496243209, -1.9889311699248642, -1.9887588024292282, 0.99446558496243209, 0.98910353742050017}},
        {192, {0.96102581066429471, -1.9220516213285894, -1.9123567006082769, 0.96102581066429471, 0.93174654204890217}},
        {224, {0.8463754216483711, -1.6927508432967422, -1.5919939789142084, 0.8463754216483711, 0.79350770767927592}},
        {255, {0.620032575454457, -1.240065150908914, -0.83465106340886308, 0.620032575454457, 0.64547923840896493}},
        {0, {5.0565748695469927e-05, 0.00010113149739093985, -1.9926995872342026, 5.0565748695469927e-05, 0.99290185022898447}},
        {32, {0.001292342279276932, 0.002584684558553864, -1.9592263381042214, 0.001292342279276932, 0.96439570722132917}},
        {64, {0.049101187608616599, 0.098202375217233198, -1.599442859010477, 0.049101187608616599, 0.79584760944494337}},
        {96, {0.31132751270768932, 0.62265502541537865, -0.36298961993172141, 0.31132751270768932, 0.60829967076247871}},
    };
    for (const auto& fixture : fixtures) {
        FilterHarness harness(44100);
        SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
        const double color = fixture.code / 255.0;
        for (int i = 0; i < 4; ++i) harness.process(color);
        double y1 = 0, y2 = 0;
        const double wet = fixture.code < 126 ?
                (fixture.code < 110 ? 1.0 : 1.0 - (fixture.code - 110) / 16.0) :
                (fixture.code > 146 ? 1.0 : (fixture.code - 130) / 16.0);
        int frame = 0;
        for (int buffer = 0; buffer < 8; ++buffer) {
            SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
            if (buffer == 0) harness.input()[0] = 1;
            harness.process(color);
            for (SINT i = 0; i < harness.samplesPerBuffer(); i += 2, ++frame) {
                const auto& c = fixture.c; // b0, b1, a1, b2, a2
                const double y = (frame == 0 ? c[0] : 0) +
                        (frame == 1 ? c[1] : 0) + (frame == 2 ? c[3] : 0) - c[2]*y1 - c[4]*y2;
                y2 = y1; y1 = y;
                const double expected = wet*y + (frame == 0 ? 1.0-wet : 0);
                EXPECT_NEAR(harness.output()[i], expected, 2e-6) << fixture.code << ":" << frame;
                ASSERT_EQ(harness.output()[i+1], 0) << "stereo leakage";
            }
        }
    }
}

TEST(ColorFxEffectTest, NoiseAllCodesMatchNativeGainOracle) {
    const std::array<float, 256> gains = {
0.20544730366f, 0.203790465739f, 0.202133645422f, 0.200476807501f, 0.19881996958f, 0.19716313166f, 0.195506311342f, 0.193849473421f,
        0.192192635501f, 0.190535815183f, 0.188878977262f, 0.187222139342f, 0.185565301421f, 0.183908481103f, 0.182251643183f, 0.180594805262f,
        0.178937967341f, 0.177281147024f, 0.175624309103f, 0.173967471182f, 0.172310650865f, 0.170653812944f, 0.168996975023f, 0.167340137103f,
        0.165683316785f, 0.164026478864f, 0.162369640944f, 0.160712803023f, 0.159055982705f, 0.157399144785f, 0.155742306864f, 0.154085486546f,
        0.152428648626f, 0.150771810705f, 0.149114972784f, 0.147458143665f, 0.145801314546f, 0.144144476625f, 0.142487647506f, 0.140830818387f,
        0.139173980466f, 0.135984494416f, 0.132846735043f, 0.129760007023f, 0.126723676647f, 0.12373703979f, 0.120799585963f, 0.117910575837f,
        0.115069446112f, 0.112275501466f, 0.109528160998f, 0.106826817401f, 0.104170863368f, 0.101559682792f, 0.0989927387771f, 0.0964693360027f,
        0.0939890255897f, 0.0915511650251f, 0.089155235018f, 0.0868006194603f, 0.0844867990607f, 0.0822132281236f, 0.0799793345486f, 0.0777845990446f,
        0.0756285023203f, 0.073510463474f, 0.0714300600315f, 0.0693867330947f, 0.0673799589717f, 0.0654092535775f, 0.0634741020219f, 0.0615740466246f,
        0.0597085812966f, 0.0578772043501f, 0.0560794801085f, 0.0543148848799f, 0.0525830313965f, 0.050883400367f, 0.0492155825197f, 0.0475790849681f,
        0.0459734808372f, 0.0443983432523f, 0.0428532189337f, 0.0413376854073f, 0.0398513334013f, 0.0383937052353f, 0.0369644224431f, 0.0355630735524f,
        0.0341892470909f, 0.0328425359873f, 0.0315225463724f, 0.0302288953792f, 0.0289611825371f, 0.0277190205785f, 0.0265020640426f, 0.0253098904552f,
        0.0241421785601f, 0.0229985432896f, 0.0218786259809f, 0.0207820657706f, 0.0197085127972f, 0.0186576193999f, 0.017629045619f, 0.0166224448936f,
        0.0156374849655f, 0.0146738236745f, 0.0137311573675f, 0.0128091438844f, 0.0119074718706f, 0.0110258156688f, 0.0101638694255f, 0.00932132728705f,
        0.00849787679851f, 0.0076932231082f, 0.00690706256282f, 0.00613911131258f, 0.00538907120515f, 0.00465665894083f, 0.00394159204506f, 0.00324358886841f,
        0.00256237353747f, 0.00189767554228f, 0.00124922341021f, 0.000616752338546f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00926157360224f, 0.0181451120717f, 0.0266506429133f,
        0.0347781452232f, 0.0425276278031f, 0.0498990884525f, 0.0568925227708f, 0.0635079351586f, 0.0697453212151f, 0.0756046853412f, 0.0810860319377f,
        0.0861893434014f, 0.0909146373354f, 0.0952619049382f, 0.0992311550113f, 0.102822378753f, 0.106035576164f, 0.108870756045f, 0.111327909595f,
        0.113407036813f, 0.115108146502f, 0.116431221059f, 0.117376286887f, 0.117943317582f, 0.118132330747f, 0.11657661377f, 0.115020905594f,
        0.113465197419f, 0.111909489243f, 0.110353781067f, 0.108798072891f, 0.107242364716f, 0.10568665654f, 0.104130948364f, 0.102575240189f,
        0.101019532013f, 0.0994638238371f, 0.0979081156614f, 0.0963524074856f, 0.0947966993099f, 0.0932409911342f, 0.0916852829584f, 0.0901295659811f,
        0.0885738578054f, 0.0870181496297f, 0.085462441454f, 0.0839067332782f, 0.0823510251025f, 0.0807953169268f, 0.0792395999495f, 0.0782235672691f,
        0.077181913293f, 0.0761169176212f, 0.0750308774568f, 0.0739259491781f, 0.0728042099497f, 0.0716676093136f, 0.0705181012126f, 0.069357441555f,
        0.0681874302564f, 0.0670096383926f, 0.0658256678446f, 0.0646370588827f, 0.0634451977502f, 0.062251514698f, 0.0610572507439f, 0.059863695314f,
        0.0586719486016f, 0.0574831636089f, 0.0562983701165f, 0.0551185759013f, 0.0539447315301f, 0.0527776995543f, 0.0516183249221f, 0.0504674261772f,
        0.0493256898399f, 0.048193837637f, 0.0470724944781f, 0.0459622852728f, 0.0448637513161f, 0.0437774339031f, 0.0427038303209f, 0.0416433454406f,
        0.0405964237402f, 0.039563426083f, 0.0385447089315f, 0.0375405759392f, 0.0365513109556f, 0.0355771648249f, 0.0346183861907f, 0.0336751312805f,
        0.0327475959284f, 0.0318359209594f, 0.0309402361961f, 0.0300606340548f, 0.0291971937494f, 0.0283500120968f, 0.0275190846963f, 0.0267044335518f,
        0.0259060762663f, 0.0251239908359f, 0.0243581552569f, 0.0236085189203f, 0.0228750312173f, 0.0221576239359f, 0.0214561782552f, 0.0207706171617f,
        0.0201008306362f, 0.0194466888557f, 0.0188080729996f, 0.0181848389425f, 0.0175768271564f, 0.0169839001171f, 0.016405865291f, 0.0158425598494f,
        0.0152938099618f, 0.0147594153931f, 0.0142391968118f, 0.0137329594837f, 0.0132404910717f, 0.0127615913405f, 0.0122960358508f, 0.0118436265678f
    };
    NoiseHarness harness(44100);
    SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
    for (int code = 0; code < 256; ++code) {
        // An interior normalized value avoids introducing a second truncation
        // via floating point division in this test's control fixture.
        harness.process(std::min(1.0, (code + 0.125) / 255.0));
        // Keep the native 256-code fixture; LastCUE Box's listening trim is +6.02 dB.
        EXPECT_NEAR(harness.state().previousGain, 2*gains[code], 8e-8) << code;
    }
}

TEST(ColorFxEffectTest, NoiseBandPassResponseAndToneMatchReference) {
    struct Fixture { int code; double hz; double q; double tone; };
    const Fixture fixtures[] = {
        {0, 133.33333333333334, 2, 0.8},
        {64, 1665.4797108601344, 2, 0.7000008},
        {144, 250.66222424979549, 2, 0.7},
        {192, 1665.4797108601344, 2, 0.700001},
        {255, 19845, 0.9999986, 0.8},
    };
    for (const auto& f : fixtures) {
        NoiseHarness harness(44100);
        SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
        harness.process(std::min(1.0, (f.code + 0.125)/255.0));
        EXPECT_NEAR(harness.state().bandPassCutoff, f.hz, 1e-6) << f.code;
        // Q at the last code is independently checked below from the snapshot.
        if (f.code != 255) EXPECT_NEAR(harness.state().bandPassQ, f.q, 1e-10);
        EXPECT_NEAR(harness.state().previousToneGain, f.tone, 3e-6) << f.code;
        const double q = harness.state().bandPassQ;
        const double t = std::tan(kPi*f.hz/44100.0);
        const double den = 1+t/q+t*t;
        const double b = (t/q)/den, a1 = 2*(t*t-1)/den, a2=(1-t/q+t*t)/den;
        harness.state().bandPassFilter->pauseFilter();
        harness.state().bandPassFilter->assumeSettled();
        harness.input()[0] = 1;
        mixxx::SampleBuffer output(harness.samplesPerBuffer());
        harness.state().bandPassFilter->process(harness.input(), output.data(), harness.samplesPerBuffer());
        double y1=0, y2=0;
        for (SINT i=0; i<harness.samplesPerBuffer(); i+=2) {
            const int frame=i/2;
            const double y=(frame==0 ? b : 0) - (frame==2 ? b : 0) - a1*y1-a2*y2;
            y2=y1; y1=y;
            EXPECT_NEAR(output[i], y, 2e-7) << f.code << ":" << frame;
            ASSERT_EQ(output[i+1], 0);
        }
    }
}

TEST(ColorFxEffectTest, NoisePreservesInputAndDisableSettlesToDry) {
    NoiseHarness silent, music;
    silent.state().generator.seed(11); music.state().generator.seed(11);
    SampleUtil::clear(silent.input(), silent.samplesPerBuffer());
    SINT frame=0;
    for (double color : {0.0, 0.25, 1.0, 0.75, 0.5}) {
        fillSine(music.input(), music.samplesPerBuffer(), &frame);
        silent.process(color); music.process(color);
        for (SINT i=0; i<music.samplesPerBuffer(); ++i) {
            EXPECT_NEAR(music.output()[i]-music.input()[i], silent.output()[i], 4e-8);
        }
    }
    music.process(0, EffectEnableState::Disabling);
    music.process(0, EffectEnableState::Disabling);
    for (SINT i=0; i<music.samplesPerBuffer(); ++i) ASSERT_EQ(music.output()[i], music.input()[i]);
}

TEST(ColorFxEffectTest, FilterAndNoiseRapidSweepIsFiniteAcrossRatesAndBuffers) {
    for (int rate : {44100, 48000, 96000}) {
      for (int frames : {64, 256, 512}) {
        FilterHarness filter(rate, frames); NoiseHarness noise(rate, frames);
        SINT frame=0;
        for (int step=0; step<512; ++step) {
            const int code = step < 256 ? step : 511-step;
            fillSine(filter.input(), filter.samplesPerBuffer(), &frame);
            SampleUtil::copy(noise.input(), filter.input(), filter.samplesPerBuffer());
            const double color=std::min(1.0, (code+0.125)/255.0);
            filter.process(color); noise.process(color);
            for (SINT i=0; i<filter.samplesPerBuffer(); ++i) {
                ASSERT_TRUE(std::isfinite(filter.output()[i]));
                ASSERT_TRUE(std::isfinite(noise.output()[i]));
                ASSERT_LT(std::abs(filter.output()[i]), 4);
                ASSERT_LT(std::abs(noise.output()[i]), 4);
            }
        }
        filter.process(0, EffectEnableState::Disabling);
        filter.process(0, EffectEnableState::Disabling);
        noise.process(0, EffectEnableState::Disabling);
        noise.process(0, EffectEnableState::Disabling);
        for (SINT i=0; i<filter.samplesPerBuffer(); ++i) {
            ASSERT_EQ(filter.output()[i], filter.input()[i]);
            ASSERT_EQ(noise.output()[i], noise.input()[i]);
        }
      }
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
    const auto manifest = ColorDubEchoEffect::getManifest();
    const auto parameters = manifest->parameters();
    EXPECT_FALSE(manifest->effectRampsFromDry());
    ASSERT_EQ(parameters.size(), 1);
    EXPECT_EQ(parameters.first()->id(), QStringLiteral("color"));
}

TEST(ColorFxEffectTest, DubEchoMatchesDefaultMasterControlSnapshots) {
    // Runtime reads from USER/DUB ECHO while moving only the Master COLOR knob.
    // Auxiliary PARAM stayed 0.5; values are captured fixtures, not a curve model.
    struct Snapshot {
        double color;
        int code;
        double leftMs, rightMs;
        float leftFeedback, rightFeedback, leftSend, rightSend;
    };
    const Snapshot snapshots[] = {
            {0.6320000290870667, 161, 55, 55, .87f, .88f, 0, .6321839094161987f},
            {1, 255, 55, 150, .87f, .98f, 0, 1},
            {0, 0, 189, 55, .95f, .88f, 1, 0},
            {.5, 127, 55, 55, .87f, .88f, 0, 0}};
    for (int rate : {44100, 48000, 96000}) {
        for (int frames : {64, 257}) {
            DubEchoHarness harness(rate, frames);
            SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
            for (const auto& snapshot : snapshots) {
                for (int processed = 0; processed < rate / 3; processed += frames)
                    harness.process(snapshot.color);
                const auto& state = harness.state();
                EXPECT_EQ(state.color, snapshot.code);
                EXPECT_NEAR(state.branches[0].delayFrames, rate * snapshot.leftMs / 1000, 1e-5);
                EXPECT_NEAR(state.branches[1].delayFrames, rate * snapshot.rightMs / 1000, 1e-5);
                EXPECT_NEAR(state.branches[0].feedback, snapshot.leftFeedback, 1e-6);
                EXPECT_NEAR(state.branches[1].feedback, snapshot.rightFeedback, 1e-6);
                EXPECT_NEAR(state.branches[0].send, snapshot.leftSend, 1e-6);
                EXPECT_NEAR(state.branches[1].send, snapshot.rightSend, 1e-6);
            }
        }
    }
}

TEST(ColorFxEffectTest, DubEchoBranchOutputGainsMatchDefaultMasterPath) {
    for (int branch : {0, 1}) {
        DubEchoHarness harness;
        SampleUtil::clear(harness.input(), harness.samplesPerBuffer());
        for (int block = 0; block < 8; ++block) harness.process(0.5);
        auto& delay = harness.state().branches[branch].delay;
        for (int i = 0; i < delay.size(); ++i) delay[i] = 1.0f;
        for (int block = 0; block < 4; ++block) harness.process(0.5);
        EXPECT_NEAR(harness.output()[harness.samplesPerBuffer() - 2], branch == 0 ? 0.49f : 0.7f, 1e-5);
    }
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

TEST(ColorFxEffectTest, FlangerFullEngineChainPreservesWetSignalAtUnityMix) {
    for (const double mix : {0.433333, 1.0}) {
        EffectsBackendManagerPointer backend(new EffectsBackendManager());
        ChannelHandleFactory handles;
        const QString group = QStringLiteral("[Channel1]");
        const auto handle = handles.getOrCreateHandle(group);
        const QSet<ChannelHandleAndGroup> channels{ChannelHandleAndGroup(handle, group)};
        const auto manifest = backend->getManifest(
                FlangerEffect::getManifest()->id(), EffectBackendType::BuiltIn);
        ASSERT_TRUE(manifest);
        EngineEffect effect(manifest, backend, channels, channels, channels);
        EngineEffectChain chain(QStringLiteral("[EffectRack1_EffectUnit1]"), channels, channels);
        auto pipes = TwoWayMessagePipe<EffectsRequest*, EffectsResponse>::makeTwoWayMessagePipe(32, 32);
        auto* responses = pipes.second.get();
        EffectsRequest request;
        request.type = EffectsRequest::ADD_EFFECT_TO_CHAIN;
        request.AddEffectToChain.pEffect = &effect;
        request.AddEffectToChain.iIndex = 0;
        ASSERT_TRUE(chain.processEffectsRequest(request, responses));
        request.type = EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS;
        request.SetEffectChainParameters.enabled = true;
        request.SetEffectChainParameters.mix_mode = EffectChainMixMode::DrySlashWet;
        request.SetEffectChainParameters.mix = mix;
        ASSERT_TRUE(chain.processEffectsRequest(request, responses));
        request.type = EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL;
        request.EnableInputChannelForChain.channelHandle = handle;
        ASSERT_TRUE(chain.processEffectsRequest(request, responses));
        request.type = EffectsRequest::SET_EFFECT_PARAMETERS;
        request.SetEffectParameters.enabled = true;
        ASSERT_TRUE(effect.processEffectsRequest(request, responses));
        const QMap<QString, double> values{{QStringLiteral("width"), 0.0},
                {QStringLiteral("manual"), 1.0}, {QStringLiteral("mix"), 1.0}};
        for (int index = 0; index < manifest->parameters().size(); ++index) {
            const auto id = manifest->parameters()[index]->id();
            if (values.contains(id)) {
                request.type = EffectsRequest::SET_PARAMETER_PARAMETERS;
                request.SetParameterParameters.iParameter = index;
                request.value = values.value(id);
                ASSERT_TRUE(effect.processEffectsRequest(request, responses));
            }
        }
        mixxx::SampleBuffer input(kFramesPerBuffer * 2), output(kFramesPerBuffer * 2);
        input.clear(); output.clear();
        GroupFeatureState features;
        features.beat_length = GroupFeatureBeatLength{0.5, 1.0};
        for (int block = 0; block < 30; ++block) {
            chain.process(handle, handle, input.data(), output.data(),
                    input.size(), kSampleRate, features, false);
        }
        input[0] = 0.2f;
        ASSERT_TRUE(chain.process(handle, handle, input.data(), output.data(),
                input.size(), kSampleRate, features, false));
        EXPECT_NEAR(output[0], 0.2 * (1.0 - 0.25 * mix), 1e-6);
        EXPECT_NEAR(output[48 * 2], 0.2 * 0.75 * mix, 1e-6);
        EXPECT_FLOAT_EQ(output[48 * 2 + 1], 0.0f);
    }
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
