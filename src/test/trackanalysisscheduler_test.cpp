#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <vector>

#include "analyzer/analyzerprogress.h"
#include "analyzer/trackanalysisscheduler.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "track/track.h"

// LastCUE Box: A newly loaded track must not wait behind the analysis of
// tracks that are no longer loaded into any player.

namespace {

class FakeEnvironment : public TrackAnalysisSchedulerEnvironment {
  public:
    explicit FakeEnvironment(const QMap<int, TrackPointer>* pTracks)
            : m_pTracks(pTracks) {
    }

    TrackPointer loadTrackById(TrackId trackId) const override {
        return m_pTracks->value(trackId.toVariant().toInt());
    }

  private:
    const QMap<int, TrackPointer>* const m_pTracks;
};

struct ProgressEvent {
    TrackId trackId;
    AnalyzerProgress progress;
};

// Long mono noise so that the analysis is still running after the first
// busy progress has been received and the scheduler has been suspended.
bool writeNoiseWav(const QString& path, int seconds) {
    constexpr quint32 kSampleRate = 8000;
    const quint32 frameCount = kSampleRate * seconds;
    const quint32 dataBytes = frameCount * 2;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << kSampleRate
        << quint32(kSampleRate * 2) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(dataBytes);
    quint32 state = 0x12345678;
    QByteArray block(static_cast<int>(kSampleRate * 2), Qt::Uninitialized);
    for (quint32 second = 0; second < static_cast<quint32>(seconds); ++second) {
        for (quint32 i = 0; i < kSampleRate; ++i) {
            state = state * 1664525u + 1013904223u;
            const qint16 sample = static_cast<qint16>((state >> 16) / 4);
            block[2 * i] = static_cast<char>(sample & 0xff);
            block[2 * i + 1] = static_cast<char>((sample >> 8) & 0xff);
        }
        out.writeRawData(block.constData(), block.size());
    }
    return out.status() == QDataStream::Ok;
}

class TrackAnalysisSchedulerTest : public MixxxTest {
  protected:
    static void SetUpTestSuite() {
        if (!SoundSourceProxy::isFileSuffixSupported(QStringLiteral("wav"))) {
            SoundSourceProxy::registerProviders();
        }
    }

    void SetUp() override {
        ASSERT_TRUE(m_tempDir.isValid());
        m_scheduler = TrackAnalysisScheduler::createInstance(
                std::make_unique<FakeEnvironment>(&m_tracks),
                1,
                mixxx::DbConnectionPoolPtr(),
                config(),
                AnalyzerModeFlags::None);
        QObject::connect(m_scheduler.get(),
                &TrackAnalysisScheduler::trackProgress,
                [this](TrackId trackId, AnalyzerProgress progress) {
                    m_events.push_back({trackId, progress});
                });
    }

    void TearDown() override {
        m_scheduler.reset();
    }

    TrackId addTrack(int id, int seconds) {
        const QString path = m_tempDir.filePath(QStringLiteral("noise-%1.wav").arg(id));
        EXPECT_TRUE(writeNoiseWav(path, seconds));
        const TrackId trackId{QVariant(id)};
        TrackPointer pTrack = Track::newDummy(path, trackId);
        m_tracks.insert(id, pTrack);
        return trackId;
    }

    bool waitFor(const std::function<bool()>& condition, int timeoutMillis) {
        QElapsedTimer timer;
        timer.start();
        while (!condition()) {
            if (timer.elapsed() > timeoutMillis) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return true;
    }

    bool hasBusy(TrackId trackId) const {
        for (const auto& event : m_events) {
            if (event.trackId == trackId && event.progress >= kAnalyzerProgressNone &&
                    event.progress < kAnalyzerProgressDone) {
                return true;
            }
        }
        return false;
    }

    bool hasDone(TrackId trackId, AnalyzerProgress progress) const {
        for (const auto& event : m_events) {
            if (event.trackId == trackId && event.progress == progress) {
                return true;
            }
        }
        return false;
    }

    bool hasAnyEvent(TrackId trackId) const {
        for (const auto& event : m_events) {
            if (event.trackId == trackId) {
                return true;
            }
        }
        return false;
    }

    // Starts the analysis of the given track and freezes it in the middle.
    void startAndFreeze(TrackId trackId) {
        ASSERT_TRUE(m_scheduler->scheduleTrack(trackId));
        m_scheduler->resume();
        ASSERT_TRUE(waitFor([&] { return hasBusy(trackId); }, 60000));
        m_scheduler->suspend();
        ASSERT_FALSE(hasDone(trackId, kAnalyzerProgressDone))
                << "The analysis finished before it could be suspended";
    }

    QTemporaryDir m_tempDir;
    QMap<int, TrackPointer> m_tracks;
    TrackAnalysisScheduler::Pointer m_scheduler =
            TrackAnalysisScheduler::NullPointer();
    std::vector<ProgressEvent> m_events;
};

TEST_F(TrackAnalysisSchedulerTest, ReplacedTrackYieldsToNewlyLoadedTrack) {
    const TrackId replaced = addTrack(1, 1200);
    const TrackId waiting = addTrack(2, 2);
    const TrackId loaded = addTrack(3, 2);

    startAndFreeze(replaced);

    // Track 2 was queued for a player that has since loaded track 3.
    ASSERT_TRUE(m_scheduler->scheduleTrack(waiting));
    ASSERT_TRUE(m_scheduler->scheduleTrack(loaded));
    m_scheduler->prioritizeLoadedTracks(loaded, QList<TrackId>{loaded});
    m_scheduler->resume();

    ASSERT_TRUE(waitFor([&] { return hasDone(loaded, kAnalyzerProgressDone); },
            60000));
    // The replaced track was abandoned without completing.
    EXPECT_TRUE(hasDone(replaced, kAnalyzerProgressUnknown));
    EXPECT_FALSE(hasDone(replaced, kAnalyzerProgressDone));
    // The queued track of the replaced load was never analyzed.
    EXPECT_FALSE(hasAnyEvent(waiting));
}

TEST_F(TrackAnalysisSchedulerTest, StillLoadedTrackKeepsAnalyzing) {
    const TrackId first = addTrack(1, 120);
    const TrackId second = addTrack(2, 2);

    startAndFreeze(first);

    ASSERT_TRUE(m_scheduler->scheduleTrack(second));
    m_scheduler->prioritizeLoadedTracks(second, QList<TrackId>{first, second});
    m_scheduler->resume();

    ASSERT_TRUE(waitFor([&] { return hasDone(second, kAnalyzerProgressDone); },
            120000));
    EXPECT_TRUE(hasDone(first, kAnalyzerProgressDone));
    EXPECT_FALSE(hasDone(first, kAnalyzerProgressUnknown));
}

} // namespace
