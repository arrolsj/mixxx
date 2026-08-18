#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <QDir>
#include <QtDebug>
#include <vector>

#include "analyzer/analyzertrack.h"
#include "analyzer/analyzerwaveform.h"
#include "library/dao/analysisdao.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "waveform/waveformfactory.h"

namespace {

constexpr std::size_t kBigBufSize = 2 * 1920; // Matches the WaveformSummary
constexpr std::size_t kCanarySize = 1024 * 4;
constexpr float kMagicFloat = 1234.567890f;
constexpr float kCanaryFloat = 0.0f;
constexpr int kChannelCount = 2;
constexpr int kSampleRate = 44100;
constexpr int kToneFrameCount = kSampleRate;
constexpr float kToneAmplitude = 0.5f;
const QString kReferenceBuffersPath = QStringLiteral("reference_buffers/");

struct BandPeaks {
    int low;
    int mid;
    int high;
};

class AnalyzerWaveformTest : public MixxxTest {
  protected:
    AnalyzerWaveformTest()
            : m_aw(config(), QSqlDatabase()) {
    }

    void SetUp() override {
        m_pTrack = Track::newTemporary();
        m_pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(kChannelCount),
                mixxx::audio::SampleRate(44100),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromMillis(1000));

        // Memory layout for m_canaryBigBuf looks like
        //   [ canary | big buf | canary ]

        m_canaryBigBuf.resize(kBigBufSize + 2 * kCanarySize);
        for (std::size_t i = 0; i < kCanarySize; i++) {
            m_canaryBigBuf[i] = kCanaryFloat;
        }
        for (std::size_t i = kCanarySize; i < kCanarySize + kBigBufSize; i++) {
            m_canaryBigBuf[i] = kMagicFloat;
        }
        for (std::size_t i = kCanarySize + kBigBufSize; i < 2 * kCanarySize + kBigBufSize; i++) {
            m_canaryBigBuf[i] = kCanaryFloat;
        }
    }

    void assertWaveformReference(
            ConstWaveformPointer pWaveform,
            const QString& reference_title) {
        pWaveform->dump();

        QFile f(getTestDir().filePath(kReferenceBuffersPath + reference_title));
        bool pass = true;
        // If the file is not there, we will fail and write out the .actual
        // reference file.
        const QByteArray actual = pWaveform->toByteArray();

        ASSERT_TRUE(f.open(QFile::ReadOnly));
        const QByteArray reference = f.readAll();

        if (actual.size() == reference.size()) {
            for (int i = 0; i < actual.size(); ++i) {
                if (actual.at(i) != reference.at(i)) {
                    qDebug() << "#" << i << QString::number(actual[i], 16)
                             << QString::number(reference[i], 16);
                    pass = false;
                }
            }
        } else {
            qDebug() << "##" << actual.size() << reference.size();
            pass = false;
        }

        // Fail if either we didn't pass, or the comparison file was empty.
        if (!pass) {
            QString fname_actual = reference_title + ".actual";
            qWarning() << "Buffer does not match" << reference_title
                       << ", actual buffer written to "
                       << "reference_buffers/" + fname_actual;
            QFile actualFile(getTestDir().filePath(kReferenceBuffersPath + fname_actual));
            ASSERT_TRUE(actualFile.open(QFile::WriteOnly));
            actualFile.write(actual);
            actualFile.close();
            EXPECT_TRUE(false);
        }
        f.close();
    }

    BandPeaks analyzeTone(double frequencyHz) {
        TrackPointer pTrack = Track::newTemporary();
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(kChannelCount),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromMillis(1000));

        AnalyzerWaveform analyzer(config(), QSqlDatabase());
        EXPECT_TRUE(analyzer.initialize(
                AnalyzerTrack(pTrack), pTrack->getSampleRate(), kToneFrameCount));

        std::vector<CSAMPLE> samples(kToneFrameCount * kChannelCount);
        const double radiansPerFrame = 2.0 * M_PI * frequencyHz / kSampleRate;
        for (int frame = 0; frame < kToneFrameCount; ++frame) {
            const auto sample = static_cast<CSAMPLE>(
                    kToneAmplitude * std::sin(radiansPerFrame * frame));
            samples[frame * kChannelCount] = sample;
            samples[frame * kChannelCount + 1] = sample;
        }
        EXPECT_TRUE(analyzer.processSamples(samples.data(), samples.size()));

        ConstWaveformPointer pWaveform = pTrack->getWaveform();
        EXPECT_NE(pWaveform, nullptr);
        BandPeaks peaks{0, 0, 0};
        if (pWaveform) {
            for (int i = 0; i < pWaveform->getCompletion(); ++i) {
                peaks.low = std::max(peaks.low, static_cast<int>(pWaveform->getLow(i)));
                peaks.mid = std::max(peaks.mid, static_cast<int>(pWaveform->getMid(i)));
                peaks.high = std::max(peaks.high, static_cast<int>(pWaveform->getHigh(i)));
            }
        }
        analyzer.cleanup();
        return peaks;
    }

    void TearDown() override {
    }

  protected:
    AnalyzerWaveform m_aw;
    TrackPointer m_pTrack;
    std::vector<CSAMPLE> m_canaryBigBuf;
};

// Basic test to make sure we don't alter the input buffer and don't step out of bounds.
TEST_F(AnalyzerWaveformTest, publishesPartialSummaryCompletionWhileAnalyzing) {
    ASSERT_TRUE(m_aw.initialize(AnalyzerTrack(m_pTrack),
            m_pTrack->getSampleRate(),
            kBigBufSize / kChannelCount));

    ConstWaveformPointer pSummary = m_pTrack->getWaveformSummary();
    ASSERT_NE(pSummary, nullptr);
    EXPECT_EQ(pSummary->getCompletion(), 0);

    const SINT halfBufferSize = static_cast<SINT>(kBigBufSize / 2);
    ASSERT_TRUE(m_aw.processSamples(&m_canaryBigBuf[kCanarySize], halfBufferSize));
    const int firstCompletion = pSummary->getCompletion();
    EXPECT_GT(firstCompletion, 0);
    EXPECT_LT(firstCompletion, pSummary->getDataSize());

    ASSERT_TRUE(m_aw.processSamples(
            &m_canaryBigBuf[kCanarySize + halfBufferSize], halfBufferSize));
    EXPECT_GT(pSummary->getCompletion(), firstCompletion);
    EXPECT_LE(pSummary->getCompletion(), pSummary->getDataSize());

    m_aw.cleanup();
}

TEST_F(AnalyzerWaveformTest, canary) {
    m_aw.initialize(AnalyzerTrack(m_pTrack),
            m_pTrack->getSampleRate(),
            kBigBufSize / kChannelCount);
    m_aw.processSamples(&m_canaryBigBuf[kCanarySize], kBigBufSize);
    m_aw.storeResults(m_pTrack);
    m_aw.cleanup();
    std::size_t i = 0;
    for (; i < kCanarySize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kCanaryFloat);
    }
    for (; i < kCanarySize + kBigBufSize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kMagicFloat);
    }
    for (; i < 2 * kCanarySize + kBigBufSize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kCanaryFloat);
    }

    // Small reference, compare bitwise
    assertWaveformReference(m_pTrack->getWaveform(), "AnalyzerWaveformsTest");

    // The summary is always big, so we check only the metadata
    ConstWaveformPointer pWaveformSummary = m_pTrack->getWaveformSummary();
    ASSERT_NE(pWaveformSummary, nullptr);
    EXPECT_EQ(pWaveformSummary->getDataSize(), 3842);
    EXPECT_EQ(pWaveformSummary->getCompletion(), 3842);
    EXPECT_DOUBLE_EQ(pWaveformSummary->getAudioVisualRatio(), 1.0);
}

TEST_F(AnalyzerWaveformTest, usesRekordboxThreeBandFrequencySplit) {
    const BandPeaks bass = analyzeTone(100.0);
    EXPECT_GT(bass.low, bass.mid * 3);
    EXPECT_GT(bass.low, bass.high * 10);

    const BandPeaks midrange = analyzeTone(600.0);
    EXPECT_GT(midrange.mid, midrange.low * 2);
    EXPECT_GT(midrange.mid, midrange.high * 10);

    const BandPeaks treble = analyzeTone(5000.0);
    EXPECT_GT(treble.high, treble.low * 10);
    EXPECT_GT(treble.high, treble.mid * 5);

    const BandPeaks aboveHighBand = analyzeTone(12000.0);
    EXPECT_LT(aboveHighBand.high, treble.high * 3 / 4);
}

TEST_F(AnalyzerWaveformTest, storesLinearBandAmplitudes) {
    EXPECT_FLOAT_EQ(scaleSignal(0.25f, Low), 0.25f);
    EXPECT_FLOAT_EQ(scaleSignal(0.25f, Mid), 0.25f);
    EXPECT_FLOAT_EQ(scaleSignal(0.25f, High), 0.25f);
}

TEST_F(AnalyzerWaveformTest, invalidatesPreRekordboxWaveformCache) {
    EXPECT_EQ(WaveformFactory::waveformVersionToVersionClass(WAVEFORM_5_VERSION),
            WaveformFactory::VC_REMOVE);
    EXPECT_EQ(WaveformFactory::waveformSummaryVersionToVersionClass(
                      WAVEFORMSUMMARY_5_VERSION),
            WaveformFactory::VC_REMOVE);
    EXPECT_EQ(WaveformFactory::waveformVersionToVersionClass(
                      WaveformFactory::currentWaveformVersion()),
            WaveformFactory::VC_USE);
}

} // namespace
