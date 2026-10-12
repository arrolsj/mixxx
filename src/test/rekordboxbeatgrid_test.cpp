#include <gtest/gtest.h>

#include <QDataStream>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>

#include "library/rekordbox/rekordboxbeatgrid.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "test/librarytest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace mixxx::rekordbox {
namespace {

TEST(RekordboxBeatGridTest, KeepsGridThatAlreadyStartsOnDownbeat) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
    };

    const auto anchored = withDownbeatAnchor(positions, 1, 500.0);

    EXPECT_EQ(positions, anchored);
}

TEST(RekordboxBeatGridTest, PrependsPrerollWithoutChangingExportedPositions) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
            audio::FramePos(2000),
    };

    for (int firstBeatNumber = 2; firstBeatNumber <= 4; ++firstBeatNumber) {
        const auto anchored = withDownbeatAnchor(positions, firstBeatNumber, 500.0);
        ASSERT_EQ(positions.size() + firstBeatNumber - 1, anchored.size());
        EXPECT_DOUBLE_EQ(
                1000.0 - (firstBeatNumber - 1) * 500.0, anchored.first().value());
        for (int index = 0; index < positions.size(); ++index) {
            EXPECT_EQ(positions[index], anchored[index + firstBeatNumber - 1]);
        }
    }
}

TEST(RekordboxBeatGridTest, RejectsInvalidBarPhaseOrTempo) {
    const QVector<audio::FramePos> positions{
            audio::FramePos(1000),
            audio::FramePos(1500),
    };

    EXPECT_EQ(positions, withDownbeatAnchor(positions, 5, 500.0));
    EXPECT_EQ(positions, withDownbeatAnchor(positions, 3, 0.0));
}

// Synthetic PMAI/PQTZ/PCOB records exercise the production Kaitai parser and
// playlist load path without writing to an exported Rekordbox device.
QByteArray analyzeTag(const char* magic, const QByteArray& body) {
    QByteArray tag;
    QDataStream stream(&tag, QIODevice::WriteOnly);
    stream.writeRawData(magic, 4);
    stream << quint32(24) << quint32(12 + body.size());
    tag.append(body);
    return tag;
}

QByteArray cueTag(bool hotCue, quint32 timeMs) {
    QByteArray body;
    QDataStream stream(&body, QIODevice::WriteOnly);
    stream << quint32(hotCue ? 1 : 0) << quint16(0) << quint16(1) << quint32(1);
    stream.writeRawData("PCPT", 4);
    stream << quint32(28) << quint32(56) << quint32(hotCue ? 1 : 0)
           << quint32(1) << quint32(0) << quint16(0) << quint16(0)
           << quint8(1) << quint8(0) << quint16(0) << timeMs << quint32(0);
    stream.writeRawData(QByteArray(16, '\0').constData(), 16);
    return analyzeTag("PCOB", body);
}

QByteArray analyzeFile(quint32 firstBeatMs, quint32 cueMs) {
    QByteArray grid;
    QDataStream stream(&grid, QIODevice::WriteOnly);
    stream << quint32(0) << quint32(0) << quint32(3);
    for (quint16 i = 0; i < 3; ++i) {
        stream << quint16((i + 2) % 4 + 1) << quint16(12000)
               << quint32(firstBeatMs + i * 500);
    }
    const auto tags = analyzeTag("PQTZ", grid) + cueTag(false, cueMs) + cueTag(true, cueMs);
    QByteArray file;
    QDataStream fileStream(&file, QIODevice::WriteOnly);
    fileStream.writeRawData("PMAI", 4);
    fileStream << quint32(28) << quint32(28 + tags.size());
    file.append(QByteArray(16, '\0'));
    file.append(tags);
    return file;
}

class RekordboxAnalyzeImportTest : public LibraryTest {
  protected:
    void SetUp() override {
        const QString audioPath = getTestDataDir().filePath("track.wav");
        ASSERT_TRUE(QFile::copy(getTestDir().filePath("sine-30.wav"), audioPath));
        m_datPath = getTestDataDir().filePath("ANLZ0000.DAT");
        const QStringList columns{
                "id", "artist", "title", "album", "year", "genre", "tracknumber",
                "location", "comment", "duration", "bitrate", "bpm", "key",
                "rating", "analyze_path", "color"};
        QSqlQuery query(dbConnection());
        ASSERT_TRUE(query.exec("CREATE TABLE rekordbox_library (" + columns.join(',') + ")"))
                << query.lastError().text().toStdString();
        ASSERT_TRUE(query.exec("CREATE TABLE rekordbox_playlist_tracks "
                               "(playlist_id, track_id, position)"));
        query.prepare("INSERT INTO rekordbox_library (id, location, analyze_path, bpm) "
                      "VALUES (1, :location, :analyze, 120)");
        query.bindValue(":location", audioPath);
        query.bindValue(":analyze", m_datPath);
        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
        ASSERT_TRUE(query.exec("INSERT INTO rekordbox_playlist_tracks VALUES (1, 1, 1)"));
        auto source = QSharedPointer<BaseTrackCache>::create(internalCollection(),
                "rekordbox_library", "id", columns, QStringList{}, false);
        m_model = std::make_unique<RekordboxPlaylistModel>(
                nullptr, trackCollectionManager(), source);
        m_model->setPlaylistById(1);
        m_model->select();
        ASSERT_EQ(1, m_model->rowCount());
        // No previous Track or beat cache may mask a skipped DAT grid.
        ASSERT_TRUE(internalCollection()->getTrackDAO().getAllTrackLocations().isEmpty());
    }

    void writeAnalyze(const QString& path, quint32 firstBeatMs, quint32 cueMs) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const auto bytes = analyzeFile(firstBeatMs, cueMs);
        ASSERT_EQ(bytes.size(), file.write(bytes));
    }

    TrackPointer loadTrack() {
        return m_model->getTrack(m_model->index(0, 0));
    }

    void expectGridAndCues(const TrackPointer& track, quint32 cueMs) {
        ASSERT_TRUE(track);
        const auto beats = track->getBeats();
        ASSERT_TRUE(beats);
        const double sampleRate = track->getSampleRate();
        ASSERT_GT(sampleRate, 0);
        // Export starts with beat 3 at 2 s; the 1 s preroll anchors beat 1.
        const auto anchor = *beats->cfirstmarker();
        EXPECT_DOUBLE_EQ(sampleRate, anchor.value());
        EXPECT_DOUBLE_EQ(2 * sampleRate,
                beats->findNthBeat(anchor, 3).value());
        const auto cues = track->getCuePointsForPlayback();
        int memoryCount = 0;
        int hotCount = 0;
        for (const auto& cue : cues) {
            if (cue->getType() == CueType::MemoryCue || cue->getHotCue() == 0) {
                EXPECT_DOUBLE_EQ(sampleRate * cueMs / 1000.0, cue->getPosition().value());
                memoryCount += cue->getType() == CueType::MemoryCue;
                hotCount += cue->getHotCue() == 0;
            }
        }
        EXPECT_EQ(1, memoryCount);
        EXPECT_EQ(1, hotCount);
    }

    QString m_datPath;
    std::unique_ptr<RekordboxPlaylistModel> m_model;
};

TEST_F(RekordboxAnalyzeImportTest, DatOnlyColdLoadImportsGridAndCues) {
    writeAnalyze(m_datPath, 2000, 2250);
    const auto track = loadTrack();
    expectGridAndCues(track, 2250);
    EXPECT_EQ(track, loadTrack());
    expectGridAndCues(track, 2250);
}

TEST_F(RekordboxAnalyzeImportTest, ExtendedCuesDoNotReplaceDatGrid) {
    writeAnalyze(m_datPath, 2000, 2250);
    writeAnalyze(getTestDataDir().filePath("ANLZ0000.EXT"), 9000, 2750);
    expectGridAndCues(loadTrack(), 2750);
}

TEST_F(RekordboxAnalyzeImportTest, ReloadImportsAnalyzeThatArrivesAfterFirstLoad) {
    const auto track = loadTrack();
    ASSERT_TRUE(track);
    writeAnalyze(m_datPath, 2000, 2250);
    EXPECT_EQ(track, loadTrack());
    expectGridAndCues(track, 2250);
    writeAnalyze(getTestDataDir().filePath("ANLZ0000.EXT"), 9000, 2750);
    EXPECT_EQ(track, loadTrack());
    expectGridAndCues(track, 2750);
}

} // namespace
} // namespace mixxx::rekordbox
