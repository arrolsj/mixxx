// Tests for enginebuffer.cpp

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QString>
#include <QTest>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <soundtouch/SoundTouch.h>
#include <vector>

#include "control/controlobject.h"
#include "engine/bufferscalers/enginebufferscalest.h"
#include "engine/bufferscalers/enginebufferscaletransient.h"
#include "engine/bufferscalers/transientwsola.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/enginebufferscalerubberband.h"
#endif
#include "engine/controls/ratecontrol.h"
#include "engine/readaheadmanager.h"
#include "mixer/basetrackplayer.h"
#include "preferences/usersettings.h"
#include "test/mixxxtest.h"
#include "test/mockedenginebackendtest.h"
#include "test/signalpathtest.h"
#include "util/sample.h"

// In case any of the test in this file fail. You can use the audioplot.py tool
// in the tools folder to visually compare the results of the enginebuffer
// with the golden test data.

namespace {
const QString kAppGroup = QStringLiteral("[App]");

// A loop boundary may return zero once. A second zero makes the scaler pad
// one input buffer with silence, then the reader resumes its audio stream.
class SoundTouchPaddingReader : public ReadAheadManager {
  public:
    explicit SoundTouchPaddingReader(bool returnZero)
            : m_returnZero(returnZero) {
    }

    SINT getNextSamples(double rate, CSAMPLE* buffer, SINT requestedSamples) override {
        Q_UNUSED(rate);
        if (m_silent || m_reads++ < (m_returnZero ? 2 : 1)) {
            SampleUtil::clear(buffer, requestedSamples);
            return m_returnZero ? 0 : requestedSamples;
        }
        for (SINT i = 0; i < requestedSamples; ++i) {
            // The signal period differs from the input buffer length and
            // channels differ, so extra silent frames shift the output.
            buffer[i] = static_cast<CSAMPLE>((m_samples++ % 10007) - 5003) / 10007;
        }
        return requestedSamples;
    }

    SINT samplesRead() const {
        return m_samples;
    }

    void restartWithSilence() {
        m_reads = 0;
        m_samples = 0;
        m_silent = true;
    }

  private:
    const bool m_returnZero;
    bool m_silent{false};
    int m_reads{0};
    SINT m_samples{0};
};

class EngineBufferScaleSTTest : public MixxxTest {
};

TEST_F(EngineBufferScaleSTTest, RepeatedZeroReadPadsExactlyOneStereoBuffer) {
    for (const double tempo : {0.75, 1.0, 1.25, -0.75, -1.25}) {
        SCOPED_TRACE(tempo);
        SoundTouchPaddingReader zeroReader(true);
        SoundTouchPaddingReader silenceReader(false);
        EngineBufferScaleST zeroScaler(&zeroReader);
        EngineBufferScaleST silenceScaler(&silenceReader);
        for (auto* scaler : {&zeroScaler, &silenceScaler}) {
            scaler->setSampleRate(mixxx::audio::SampleRate(44100));
            double tempoRatio = tempo;
            double pitchRatio = 1.0;
            scaler->setScaleParameters(1.0, &tempoRatio, &pitchRatio);
            scaler->clear();
        }

        // Both paths use the linked SoundTouch library; no version-dependent
        // golden audio or mock of its frames-based putSamples API is needed.
        constexpr SINT kOutputSamples = 16384;
        std::vector<CSAMPLE> actual(kOutputSamples);
        std::vector<CSAMPLE> expected(kOutputSamples);
        EXPECT_DOUBLE_EQ(silenceScaler.scaleBuffer(expected.data(), kOutputSamples),
                zeroScaler.scaleBuffer(actual.data(), kOutputSamples));
        EXPECT_EQ(silenceReader.samplesRead(), zeroReader.samplesRead());
        EXPECT_EQ(expected, actual);
    }
}

// Renders the reader signal through a bare SoundTouch instance, fed in the
// same 512-frame chunks as EngineBufferScaleST.
std::vector<CSAMPLE> renderBareSoundTouch(double tempo, int sequenceMs, int seekWindowMs) {
    soundtouch::SoundTouch st;
    st.setChannels(2);
    st.setRate(1.0);
    st.setPitch(1.0);
    st.setSetting(SETTING_USE_QUICKSEEK, 1);
    st.setSetting(SETTING_SEQUENCE_MS, sequenceMs);
    st.setSetting(SETTING_SEEKWINDOW_MS, seekWindowMs);
    st.setSetting(SETTING_OVERLAP_MS, 8);
    st.setSampleRate(44100);
    st.setTempo(0.1);
    st.setTempo(1.0);
    st.clear();
    st.setTempo(tempo);

    SoundTouchPaddingReader reader(false);
    std::vector<CSAMPLE> input(1024);
    std::vector<CSAMPLE> output(32768);
    SINT received = 0;
    while (received < 16384) {
        received += st.receiveSamples(output.data() + 2 * received, 16384 - received);
        if (received < 16384) {
            reader.getNextSamples(1.0, input.data(), 1024);
            st.putSamples(input.data(), 512);
        }
    }
    return output;
}

TEST_F(EngineBufferScaleSTTest, UsesShortFixedSequencesThatKeepDrumAttacks) {
    constexpr double kTempo = 1.04;
    SoundTouchPaddingReader reader(false);
    EngineBufferScaleST scaler(&reader);
    scaler.setSampleRate(mixxx::audio::SampleRate(44100));
    double tempoRatio = kTempo;
    double pitchRatio = 1.0;
    scaler.setScaleParameters(1.0, &tempoRatio, &pitchRatio);
    scaler.clear();
    std::vector<CSAMPLE> actual(32768);
    scaler.scaleBuffer(actual.data(), 32768);

    EXPECT_EQ(renderBareSoundTouch(kTempo, 30, 10), actual);
    // 0 selects SoundTouch's automatic (long) sequence and seek window.
    EXPECT_NE(renderBareSoundTouch(kTempo, 0, 0), actual);
}

// Low sine bed with short noise bursts ("drum hits") every 250 ms.
std::vector<CSAMPLE> makeClickTrain(SINT frames, SINT period, std::vector<SINT>* pClicks) {
    std::vector<CSAMPLE> signal(2 * frames);
    uint32_t noise = 12345;
    for (SINT i = 0; i < frames; ++i) {
        float value = 0.1f * std::sin(2 * M_PI * 110 * i / 44100.0);
        const SINT phase = i % period;
        if (i >= period && phase < 132) {
            noise = noise * 1664525u + 1013904223u;
            const float burst = static_cast<float>(noise >> 8) / (1 << 24) * 2 - 1;
            value += 0.8f * burst * (1.0f - static_cast<float>(phase) / 132);
            if (phase == 0) {
                pClicks->push_back(i);
            }
        }
        signal[2 * i] = value;
        signal[2 * i + 1] = value;
    }
    return signal;
}

std::vector<CSAMPLE> stretchAll(TransientWsola* pStretch, const std::vector<CSAMPLE>& input) {
    std::vector<CSAMPLE> output;
    std::vector<CSAMPLE> block(1024);
    const SINT frames = static_cast<SINT>(input.size() / 2);
    for (SINT pos = 0; pos < frames;) {
        const SINT n = std::min<SINT>({512, frames - pos, pStretch->freeInputFrames()});
        pStretch->putSamples(input.data() + 2 * pos, n);
        pos += n;
        SINT got;
        while ((got = pStretch->receiveSamples(block.data(), 512)) > 0) {
            output.insert(output.end(), block.begin(), block.begin() + 2 * got);
        }
    }
    return output;
}

class TransientWsolaTest : public MixxxTest {
};

TEST_F(TransientWsolaTest, UnityTempoAndRateAreTransparent) {
    std::vector<SINT> clicks;
    const std::vector<CSAMPLE> input = makeClickTrain(44100, 11025, &clicks);
    TransientWsola stretch(44100);
    const std::vector<CSAMPLE> output = stretchAll(&stretch, input);
    ASSERT_GT(output.size(), input.size() / 2);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), input.begin()));
}

TEST_F(TransientWsolaTest, KeepsLongTermTempo) {
    std::vector<SINT> clicks;
    const std::vector<CSAMPLE> input = makeClickTrain(5 * 44100, 11025, &clicks);
    TransientWsola stretch(44100);
    stretch.setTempo(1.16);
    const SINT produced = static_cast<SINT>(stretchAll(&stretch, input).size() / 2);
    // Output lags by at most the lookahead plus one deferred join.
    const double expected = 5 * 44100 / 1.16;
    EXPECT_GT(produced, expected - 0.15 * 44100);
    EXPECT_LE(produced, expected + 1);
}

TEST_F(TransientWsolaTest, SpeedUpKeepsEveryDrumHitIntact) {
    for (const double tempo : {0.84, 1.08, 1.16}) {
        SCOPED_TRACE(tempo);
        std::vector<SINT> clicks;
        const std::vector<CSAMPLE> input = makeClickTrain(8 * 44100, 11025, &clicks);
        TransientWsola stretch(44100);
        stretch.setTempo(tempo);
        const std::vector<CSAMPLE> output = stretchAll(&stretch, input);
        EXPECT_GT(stretch.stats().onsets, 0);
        // Every burst must appear once with its full attack: its first
        // 2 ms are copied, not crossfaded or skipped.
        int found = 0;
        for (SINT click : clicks) {
            const double outPos = click / tempo;
            float peak = 0;
            float inputPeak = 0;
            for (SINT i = 0; i < 88; ++i) {
                inputPeak = std::max(inputPeak, std::fabs(input[2 * (click + i)]));
            }
            const SINT from = static_cast<SINT>(outPos) - 2205;
            const SINT to = static_cast<SINT>(outPos) + 2205;
            if (from < 0 || 2 * to >= static_cast<SINT>(output.size())) {
                continue;
            }
            for (SINT i = from; i < to; ++i) {
                peak = std::max(peak, std::fabs(output[2 * i]));
            }
            EXPECT_GE(peak, 0.97f * inputPeak) << "burst at " << click;
            ++found;
        }
        EXPECT_GE(found, static_cast<int>(clicks.size()) - 2);
    }
}

TEST_F(TransientWsolaTest, PlacesDrumHitsOnTime) {
    // Sparse hits over a sustained bed are where timing errors are audible.
    for (const double tempo : {0.84, 0.92, 1.08, 1.16}) {
        SCOPED_TRACE(tempo);
        std::vector<SINT> clicks;
        const std::vector<CSAMPLE> input = makeClickTrain(8 * 44100, 21336, &clicks);
        TransientWsola stretch(44100);
        stretch.setTempo(tempo);
        const std::vector<CSAMPLE> output = stretchAll(&stretch, input);
        double sum = 0;
        int count = 0;
        for (SINT click : clicks) {
            // The burst start is the first loud sample near its nominal time.
            const SINT nominal = static_cast<SINT>(std::lround(click / tempo));
            const SINT from = nominal - 1323;
            if (from < 0 || 2 * (nominal + 1323) >= static_cast<SINT>(output.size())) {
                continue;
            }
            SINT found = -1;
            for (SINT i = from; i < nominal + 1323; ++i) {
                if (std::fabs(output[2 * i]) > 0.5f) {
                    found = i;
                    break;
                }
            }
            ASSERT_GE(found, 0) << "burst at " << click;
            const SINT error = found - nominal;
            // 4 ms: the sync join's search window plus the burst's rise.
            EXPECT_LE(std::abs(error), 176) << "burst at " << click;
            sum += error;
            ++count;
        }
        ASSERT_GT(count, 10);
        EXPECT_LE(std::fabs(sum / count), 88.0); // No systematic offset (2 ms).
        EXPECT_GT(stretch.stats().syncedOnsets, 0);
    }
}

class EngineBufferScaleTransientTest : public MixxxTest {
};

TEST_F(EngineBufferScaleTransientTest, PitchAndTrackRateProduceFiniteAudio) {
    for (const auto& [baseRate, tempo, pitch] : std::vector<std::tuple<double, double, double>>{
                 {1.0, 1.04, 1.0}, {48000.0 / 44100, 0.92, 1.0}, {1.0, 1.0, 1.06}}) {
        SCOPED_TRACE(QString("%1 %2 %3").arg(baseRate).arg(tempo).arg(pitch).toStdString());
        SoundTouchPaddingReader reader(false);
        EngineBufferScaleTransient scaler(&reader);
        scaler.setSampleRate(mixxx::audio::SampleRate(44100));
        double tempoRatio = tempo;
        double pitchRatio = pitch;
        scaler.setScaleParameters(baseRate, &tempoRatio, &pitchRatio);
        scaler.clear();
        std::vector<CSAMPLE> output(1024);
        double consumed = 0;
        for (int block = 0; block < 64; ++block) {
            consumed += scaler.scaleBuffer(output.data(), 1024);
            for (CSAMPLE sample : output) {
                ASSERT_TRUE(std::isfinite(sample));
                ASSERT_LE(std::fabs(sample), 1.5f);
            }
        }
        EXPECT_NEAR(consumed, 64 * 512 * baseRate * tempo, 1e-6 * consumed);
    }
}
#ifdef __RUBBERBAND__
class EngineBufferScaleRubberBandTest : public MixxxTest {
};

TEST_F(EngineBufferScaleRubberBandTest, RepeatedFasterSelectionPreservesBufferedAudio) {
    for (const double tempo : {0.92, 1.0, 1.08, -0.92, -1.08}) {
        SCOPED_TRACE(tempo);
        SoundTouchPaddingReader referenceReader(false);
        SoundTouchPaddingReader actualReader(false);
        EngineBufferScaleRubberBand reference(&referenceReader);
        EngineBufferScaleRubberBand actual(&actualReader);
        for (auto* scaler : {&reference, &actual}) {
            scaler->setSampleRate(mixxx::audio::SampleRate(44100));
            scaler->useEngineFiner(false);
            double ratio = tempo;
            double pitch = 1.0;
            scaler->setScaleParameters(1.0, &ratio, &pitch);
            scaler->clear();
        }
        std::vector<CSAMPLE> expected(1024);
        std::vector<CSAMPLE> observed(1024);
        for (int block = 0; block < 12; ++block) {
            if (block == 4 || block == 8) {
                actual.useEngineFiner(false);
            }
            EXPECT_DOUBLE_EQ(reference.scaleBuffer(expected.data(), expected.size()),
                    actual.scaleBuffer(observed.data(), observed.size()));
            EXPECT_EQ(expected, observed);
            EXPECT_EQ(referenceReader.samplesRead(), actualReader.samplesRead());
        }
    }
}

TEST_F(EngineBufferScaleRubberBandTest, ClearDiscardsPreviousAudioAndRetainsTransportRate) {
    SoundTouchPaddingReader firstReader(false);
    EngineBufferScaleRubberBand first(&firstReader);
    first.setSampleRate(mixxx::audio::SampleRate(44100));
    double ratio = 1.08;
    double pitch = 1.0;
    first.setScaleParameters(1.0, &ratio, &pitch);
    first.clear();
    std::vector<CSAMPLE> observed(1024);
    for (int block = 0; block < 8; ++block) {
        first.scaleBuffer(observed.data(), observed.size());
    }
    // R2 reset clears audio buffers, but is not a promise of identical
    // phase state to a new instance. Test the actual transport contract:
    // queued sound must disappear immediately when the new input is silent.
    firstReader.restartWithSilence();
    first.clear();
    for (int block = 0; block < 8; ++block) {
        EXPECT_NEAR(512 * ratio,
                first.scaleBuffer(observed.data(), observed.size()), 1e-9);
        for (const auto sample : observed) {
            EXPECT_FLOAT_EQ(0.0f, sample);
        }
    }
}
#endif
}

class EngineBufferTest : public MockedEngineBackendTest {};

class EngineBufferE2ETest : public SignalPathTest {};

TEST_F(EngineBufferTest, DisableKeylockResetsPitch) {
    // To prevent one-slider users from getting stuck on a key,
    // KeyunlockMode::ResetLockedKey resets the musical pitch.
    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
            1.0); // KeylockMode::LockCurrentKey
    ControlObject::set(ConfigKey(m_sGroup1, "keyunlockMode"),
            0.0); // KeyunlockMode::ResetLockedKey
    ControlObject::set(ConfigKey(m_sGroup1, "file_bpm"), 128.0);
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), 0.5);
    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    // We require a buffer process to see that the keylock state has changed.
    ProcessBuffer();
    ASSERT_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch")));
}

TEST_F(EngineBufferTest, DisableKeylockKeepsPitch) {
    // Pitch must not change when unlocking with KeyunlockMode::KeepLockedKey.
    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
            1.0); // KeylockMode::LockCurrentKey
    ControlObject::set(ConfigKey(m_sGroup1, "keyunlockMode"),
            1.0); // KeyunlockMode::KeepLockedKey
    ControlObject::set(ConfigKey(m_sGroup1, "file_bpm"), 128.0);
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), 0.5);
    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    // We require a buffer process to see that the keylock state has changed.
    ProcessBuffer();
    ASSERT_NEAR(0.5, ControlObject::get(ConfigKey(m_sGroup1, "pitch")), 1e-10);
}

TEST_F(EngineBufferTest, TrackLoadResetsPitch) {
    // When a new track is loaded, the pitch value should be reset.
    config()->set(ConfigKey("[Controls]","SpeedAutoReset"),
                  ConfigValue(BaseTrackPlayer::RESET_PITCH));
    ControlObject::set(ConfigKey(m_sGroup1, "file_bpm"), 128.0);
    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 0.5);
    ProcessBuffer();
    ASSERT_NEAR(0.5, ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust")), 1e-10);

    m_pMixerDeck1->loadFakeTrack(false, 0.0);
    ASSERT_NEAR(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust")), 1e-10);
}

TEST_F(EngineBufferTest, PitchRoundtrip) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
            0.0); // KeylockMode::LockOriginalKey
    ControlObject::set(ConfigKey(m_sGroup1, "keyunlockMode"),
            0.0); // KeyunlockMode::ResetLockedKey
    ProcessBuffer();
    // we are in kPakmOffsetScaleReseting mode
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.5);
    ProcessBuffer();
    // pitch_adjust must not change
    ASSERT_DOUBLE_EQ(0.0,
                     ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust")));

    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 0.5);
    ProcessBuffer();
    // rate must not change
    ASSERT_DOUBLE_EQ(0.5, ControlObject::get(ConfigKey(m_sGroup1, "rate")));

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    // pitch is reset, as well as pitch_adjust
    // speed must not change, pitch and pitch_adjust must be 0
    ASSERT_DOUBLE_EQ(0.5, ControlObject::get(ConfigKey(m_sGroup1, "rate")));
    ASSERT_DOUBLE_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch")));
    ASSERT_DOUBLE_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust")));

    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
            1.0); // KeylockMode::LockCurrentKey
    ProcessBuffer();
    // rate must not change
    ASSERT_DOUBLE_EQ(0.5, ControlObject::get(ConfigKey(m_sGroup1, "rate")));
    // pitch must reflect the absolute pitch
    ASSERT_DOUBLE_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch")));

    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
            0.0); // KeylockMode::LockOriginalKey
    ProcessBuffer();
    // rate must not change
    ASSERT_DOUBLE_EQ(0.5, ControlObject::get(ConfigKey(m_sGroup1, "rate")));
    // pitch must be 0
    ASSERT_NEAR(0.0, ControlObject::get(ConfigKey(m_sGroup1, "pitch")), 1e-10);
}

#ifdef __RUBBERBAND__
TEST_F(EngineBufferTest, SlowRubberBand) {
    // At very slow speeds, RubberBand needs to reallocate buffers and since
    // this
    // is done in the engine thread it can be a major party-stopper.
    // Make sure slow speeds still use the linear scaler.
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), 2.8);

    // Hack to get a slow, non-scratching direct speed
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 0.0072);

    // With Soundtouch, it should switch the scaler as well
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    // Back to full speed
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 1);
    ProcessBuffer();

    // With Rubberband, and transport stopped it should be still keylock
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::RubberBandFaster));
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);

    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 0.0072);

    // Playing at low rate, the vinyl scaler should be used
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
}
#endif

TEST_F(EngineBufferTest, ScalerNoTransport) {
    // normally use the Vinyl scaler
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    // A non-unity tempo needs the keylock scaler. Unity SoundTouch/R2 now
    // deliberately stays on vinyl, independently of this pause contract.
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.08);
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);

    // Stop and disable keylock: do not change scaler
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);

    // play: we need to use vinyl scaler
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    SCOPED_TRACE("SoundTouch unity and pitch/pause behavior");
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.0);

    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ASSERT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    // A real tempo adjustment needs time stretching, then unity restores
    // the transparent path through the normal scaler transition.
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.08);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);

    // Independent musical pitch adjustment must still be processed at
    // unity tempo; MT cannot silently disable the pitch control.
    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 0.5);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 0.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);

#ifdef __RUBBERBAND__
    SCOPED_TRACE("R2 unity bypass and R3 behavior unchanged");

    for (const auto engine : {EngineBuffer::KeylockEngine::RubberBandFaster,
                 EngineBuffer::KeylockEngine::RubberBandFiner}) {
        ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
        ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
                static_cast<double>(engine));
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
        ProcessBuffer();
        ASSERT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
        ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
        ProcessBuffer();
        const auto unityScaler = engine == EngineBuffer::KeylockEngine::RubberBandFiner
                ? m_pMockScaleKeylock1
                : m_pMockScaleVinyl1;
        EXPECT_EQ(unityScaler, m_pChannel1->getEngineBuffer()->m_pScale);
        ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.08);
        ProcessBuffer();
        EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);
        ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.0);
        ProcessBuffer();
        EXPECT_EQ(unityScaler, m_pChannel1->getEngineBuffer()->m_pScale);
    }

#endif
}


TEST_F(EngineBufferTest, VinylScalerRampZero) {
    // scratch in play mode
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 1.0);

    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
    EXPECT_EQ(m_pMockScaleVinyl1->getProcessedTempo(), 1.0);

    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 0.0);

    // we are in scratching mode so a zero rate has to be processed
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
    EXPECT_EQ(m_pMockScaleVinyl1->getProcessedTempo(), 0.0);
}

TEST_F(EngineBufferTest, ReadFadeOut) {
    // Start playing
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);

    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
    EXPECT_EQ(m_pMockScaleVinyl1->getProcessedTempo(), 1.0);

    // pause
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);

    // The scaler need to be processed with the old rate to
    // prepare the crossfade buffer
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
    EXPECT_EQ(m_pMockScaleVinyl1->getProcessedTempo(), 1.0);
}

TEST_F(EngineBufferTest, ResetPitchAdjustUsesLinear) {
    // If the key was adjusted, but keylock is off, and then the key is
    // reset, then the engine should be using the linear scaler.

    // First, we should be using the keylock scaler here.
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), 1.2);
    // Remember that a rate of "0" is "regular playback speed".
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.05);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleKeylock1, m_pChannel1->getEngineBuffer()->m_pScale);

    // Reset pitch adjust, and we should be back to the linear scaler.
    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust_set_default"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(m_pMockScaleVinyl1, m_pChannel1->getEngineBuffer()->m_pScale);
}

TEST_F(EngineBufferE2ETest, SoundTouchCrashTest) {
    // Soundtouch has a bug where a pitch value of zero causes an infinite loop
    // and crash.
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), 1.2);
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.05);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    // Start by playing with soundtouch enabled.
    ProcessBuffer();
    // Pause the buffer.  This causes the pitch to be set to 0.
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), -0.05);
    // Should not crash
    ProcessBuffer();
}

TEST_F(EngineBufferE2ETest, BasicProcessingTest) {
    // Confirm that playing ramps from silence, pausing ramps to silence,
    // and also just confirm that playing works as predicted.
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.05);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "BasicProcessingTestPlay");
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "BasicProcessingTestPlaying");
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "BasicProcessingTestPause");
}

TEST_F(EngineBufferE2ETest, ScratchTest) {
    // Confirm that vinyl scratching smoothly transitions from one direction
    // to the other.
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 1.1);
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos(225), EngineBuffer::SEEK_EXACT);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), -1.1);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "ScratchTestMain");
}

TEST_F(EngineBufferE2ETest, ScratchTestStart) {
    // Confirm that vinyl scratching smoothly transitions from zero speed
    // to an the other speed above the 0.07 threshold.
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 1);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 0.5);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "ScratchTestStart");
}

TEST_F(EngineBufferE2ETest, ReverseTest) {
    // Confirm that pushing the reverse button smoothly transitions.
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "ReverseTest");
}

// DISABLED: This test is too dependent on the sound touch library version.
TEST_F(EngineBufferE2ETest, DISABLED_SoundTouchToggleTest) {
   // Test various cases where SoundTouch toggles on and off.
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.5);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    // Test transition from vinyl to keylock
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "SoundTouchTest");
    // Test transition from keylock to vinyl due to slow speed.
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 0.0072);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "SoundTouchTestSlow");
    // Test transition back to keylock due to regular speed.
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "SoundTouchTestRegular");
}

#ifdef __RUBBERBAND__
// DISABLED: This test is too dependent on the rubber band library version.
TEST_F(EngineBufferE2ETest, DISABLED_RubberbandToggleTest) {
   // Test various cases where Rubberband toggles on and off.
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::RubberBandFaster));
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.5);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    // Test transition from vinyl to keylock
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "RubberbandTest");
    // Test transition from keylock to vinyl due to slow speed.
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 0.0072);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "RubberbandTestSlow");
    // Test transition back to keylock due to regular speed.
    ControlObject::set(ConfigKey(m_sGroup1, "rateSearch"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "RubberbandTestRegular");
}
#endif

// DISABLED: This test is too dependent on the sound touch library version.
// NOTE(uklotzde, 2018-01-10): We have also seen spurious failures on the
// Linux build server under high load. These failures might by caused by
// delayed asynchronous reads from CachingReader. The corresponding chunks
// will be filled with 0-samples by the engine buffer scaler.
TEST_F(EngineBufferE2ETest, DISABLED_KeylockReverseTest) {
    // Confirm that when toggling reverse while keylock is on, interpolation
    // is smooth.
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ControlObject::set(ConfigKey(m_sGroup1, "keylockMode"),
                       0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.5);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "KeylockReverseTest");
}

TEST_F(EngineBufferE2ETest, SeekTest) {
    // Confirm that seeking to a new position smoothly transitions.
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos(500), EngineBuffer::SEEK_EXACT);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "SeekTest");
}

TEST_F(EngineBufferE2ETest, SoundTouchReverseTest) {
    // This test must not crash when changing to reverse while pitch is tweaked
    // Testing issue #8061
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SoundTouch));
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), -1);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1.0);
    ProcessBuffer();
    // Note: we cannot compare a golden buffer here, because the result depends
    // on the uses library version
}

#ifdef __RUBBERBAND__
TEST_F(EngineBufferE2ETest, RubberbandReverseTest) {
    // This test must not crash when changing to reverse while pitch is tweaked
    // Testing issue #8061
    ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::RubberBandFaster));
    ControlObject::set(ConfigKey(m_sGroup1, "pitch"), -1);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1.0);
    ProcessBuffer();
    // Note: we cannot compare a golden buffer here, because the result depends
    // on the uses library version
}
#endif

TEST_F(EngineBufferE2ETest, CueGotoAndStopTest) {
    // Be sure, that the Crossfade buffer is processed only once
    // Issue #8251
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "cue_gotoandstop"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "CueGotoAndStopTest");
}

TEST_F(EngineBufferE2ETest, CueGotoAndPlayTest) {
    // Be sure, cue seek is not overwritten by quantization seek
    // Issue #8249
    ControlObject::set(ConfigKey(m_sGroup1, "quantize"), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, "cue_point"), 0.0);
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos(500), EngineBuffer::SEEK_EXACT);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "cue_gotoandplay"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "CueGotoAndPlayTest");
}

TEST_F(EngineBufferE2ETest, CueStartPlayTest) {
    // Be sure, cue seek is not overwritten by quantization seek
    // Issue #8252
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "start_play"), 1.0);
    ProcessBuffer();
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            kProcessBufferSize,
            "StartPlayTest");
}

TEST_F(EngineBufferE2ETest, CueGotoAndPlayDenon) {
    // Be sure, cue point is not moved
    // enable Denon mode issue #8254
    ControlObject::set(ConfigKey(m_sGroup1, "cue_mode"), 2.0); // CUE_MODE_DENON
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos(500), EngineBuffer::SEEK_EXACT);
    ProcessBuffer();
    double cueBefore = ControlObject::get(ConfigKey(m_sGroup1, "cue_point"));
    ControlObject::set(ConfigKey(m_sGroup1, "cue_gotoandplay"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(cueBefore, ControlObject::get(ConfigKey(m_sGroup1, "cue_point")));
}

TEST_F(EngineBufferTest, RateTempTest) {
    RateControl::setTemporaryRateChangeCoarseAmount(4);
    RateControl::setTemporaryRateChangeFineAmount(2);

    ControlObject::set(ConfigKey(m_sGroup1, "rate_dir"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, m_pChannel1->getEngineBuffer()->m_speed_old);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up"), 1);
    ProcessBuffer();
    EXPECT_EQ(1.04, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up_small"), 1);
    ProcessBuffer();
    EXPECT_EQ(1.02, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up_small"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down"), 1);
    ProcessBuffer();
    EXPECT_EQ(0.96, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down_small"), 1);
    ProcessBuffer();
    EXPECT_EQ(0.98, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down_small"), 0);

    ControlObject::set(ConfigKey(m_sGroup1, "rate_dir"), -1);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up"), 1);
    ProcessBuffer();
    EXPECT_EQ(1.04, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up_small"), 1);
    ProcessBuffer();
    EXPECT_EQ(1.02, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_up_small"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down"), 1);
    ProcessBuffer();
    EXPECT_EQ(0.96, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down"), 0);

    ProcessBuffer();

    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down_small"), 1);
    ProcessBuffer();
    EXPECT_EQ(0.98, m_pChannel1->getEngineBuffer()->m_speed_old);
    ControlObject::set(ConfigKey(m_sGroup1, "rate_temp_down_small"), 0);
}


TEST_F(EngineBufferTest, JogNudgeTakesEffectWithin50ms) {
    // 512 frames at 44.1 kHz: four callbacks are 46 ms.
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    ProcessBuffer();
    ASSERT_EQ(1.0, m_pChannel1->getEngineBuffer()->m_speed_old);

    ControlObject::set(ConfigKey(m_sGroup1, "jog"), 5.0);
    double early = 0;
    double total = 0;
    for (int i = 0; i < 30; ++i) {
        ProcessBuffer();
        const double bend = m_pChannel1->getEngineBuffer()->m_speed_old - 1.0;
        if (i < 4) {
            early += bend;
        }
        total += bend;
    }
    // The nudge moves the track as far as before, just sooner.
    EXPECT_NEAR(0.5, total, 1e-9);
    EXPECT_NEAR(total, early, 1e-9);
    EXPECT_EQ(1.0, m_pChannel1->getEngineBuffer()->m_speed_old);
}

TEST_F(EngineBufferTest, RatePermTest) {
    RateControl::setPermanentRateChangeCoarseAmount(4);
    RateControl::setPermanentRateChangeFineAmount(2);

    ControlObject::set(ConfigKey(m_sGroup1, "rate_dir"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
    ProcessBuffer();
    EXPECT_EQ(1.0, m_pChannel1->getEngineBuffer()->m_speed_old);

    ControlObject::set(ConfigKey(m_sGroup1, "rate_perm_up"), 1);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "rate_perm_up"), 0);
    EXPECT_EQ(1.04, m_pChannel1->getEngineBuffer()->m_speed_old);

    ControlObject::set(ConfigKey(m_sGroup1, "rate_perm_up_small"), 1);
    ProcessBuffer();
    ControlObject::set(ConfigKey(m_sGroup1, "rate_perm_up_small"), 0);
    EXPECT_EQ(1.06, m_pChannel1->getEngineBuffer()->m_speed_old);
}
