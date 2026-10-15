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

constexpr audio::SampleRate kSampleRate(44100);

// Rekordbox stores beat times in whole milliseconds and the tempo in BPM * 100.
// The grid itself may run at a tempo the 0.01 BPM field only approximates.
QVector<GridBeat> exportedGrid(double firstBeatMs, int beats, int tempo, double gridBpm = 0) {
    const double beatMs = 60000.0 / (gridBpm > 0 ? gridBpm : tempo / 100.0);
    QVector<GridBeat> grid;
    for (int i = 0; i < beats; ++i) {
        const double ms = std::round(firstBeatMs + i * beatMs);
        grid.append({audio::FramePos(kSampleRate.value() * ms / 1000.0), tempo});
    }
    return grid;
}

void expectOnExportedBeats(const BeatsPointer& beats, const QVector<GridBeat>& grid) {
    for (const auto& beat : grid) {
        // Within the export's millisecond rounding.
        EXPECT_NEAR(beat.position.value(),
                beats->findClosestBeat(beat.position).value(),
                kSampleRate.value() * 0.0005 + 1);
    }
}

TEST(RekordboxBeatGridTest, ConstantTempoGridHasNoBpmJitter) {
    // 128 BPM beats are 468.75 ms apart; the rounded times alternate.
    const auto grid = exportedGrid(1000.0, 64, 12800);
    const auto beats = beatsFromGrid(kSampleRate, grid, 1, QString());
    ASSERT_TRUE(beats);
    const double bpm = beats->getBpmAroundPosition(grid[4].position, 4).value();
    EXPECT_NEAR(128.0, bpm, 0.01);
    for (const auto& beat : grid) {
        EXPECT_NEAR(bpm, beats->getBpmAroundPosition(beat.position, 4).value(), 1e-6);
    }
    expectOnExportedBeats(beats, grid);
}

TEST(RekordboxBeatGridTest, RoundedTempoFieldDoesNotDriftLongTracks) {
    // A 127.996 BPM grid exported with tempo 128.00: a grid built from the
    // field would be 10 ms off after 600 beats.
    const auto grid = exportedGrid(1000.0, 600, 12800, 127.996);
    const auto beats = beatsFromGrid(kSampleRate, grid, 1, QString());
    ASSERT_TRUE(beats);
    EXPECT_NEAR(127.996, beats->getBpmAroundPosition(grid[300].position, 4).value(), 1e-3);
    expectOnExportedBeats(beats, grid);
}

TEST(RekordboxBeatGridTest, EditedGridKeepsExportedPositions) {
    // A grid moved by hand partway through is not one straight line.
    auto grid = exportedGrid(1000.0, 64, 12800);
    for (int i = 32; i < grid.size(); ++i) {
        grid[i].position = grid[i].position + kSampleRate.value() * 0.020;
    }
    const auto beats = beatsFromGrid(kSampleRate, grid, 1, QString());
    ASSERT_TRUE(beats);
    for (const auto& beat : grid) {
        EXPECT_NEAR(beat.position.value(), beats->findClosestBeat(beat.position).value(), 1.0);
    }
}

TEST(RekordboxBeatGridTest, TempoChangesBecomeConstantSections) {
    auto grid = exportedGrid(1000.0, 16, 12000);
    const double secondStartMs = 1000.0 + 16 * 500.0;
    grid += exportedGrid(secondStartMs, 16, 12600);
    // Export starts on beat 3: two preroll beats anchor the first downbeat.
    const auto beats = beatsFromGrid(kSampleRate, grid, 3, QString());
    ASSERT_TRUE(beats);
    ASSERT_EQ(1u, beats->getMarkers().size());
    EXPECT_DOUBLE_EQ(kSampleRate.value() * 0.0, beats->getMarkers().front().position().value());
    EXPECT_EQ(18, beats->getMarkers().front().beatsTillNextMarker());
    EXPECT_NEAR(120.0, beats->getBpmAroundPosition(grid[6].position, 4).value(), 1e-6);
    EXPECT_NEAR(126.0, beats->getLastMarkerBpm().value(), 0.01);
    EXPECT_NEAR(126.0, beats->getBpmAroundPosition(grid[24].position, 4).value(), 0.01);
    expectOnExportedBeats(beats, grid);
}

TEST(RekordboxBeatGridTest, BeatBeforeTrackStartStillFitsConstantGrid) {
    // A 140 BPM export whose first beat is at 25 ms, read with a 26 ms MP3
    // decoder offset. Clamping that beat to 1 ms put it 2 ms off the line
    // and the whole grid fell back to a jittering beat map.
    const double beatMs = 60000.0 / 140.0;
    QVector<GridBeat> grid;
    for (int i = 0; i < 262; ++i) {
        grid.append(gridBeatFromExport(kSampleRate,
                static_cast<int>(std::round(25.0 + i * beatMs)),
                26,
                14000));
    }
    EXPECT_DOUBLE_EQ(-kSampleRate.value() / 1000.0, grid.first().position.value());
    const auto beats = beatsFromGrid(kSampleRate, grid, 4, QString());
    ASSERT_TRUE(beats);
    EXPECT_TRUE(beats->getMarkers().empty());
    EXPECT_NEAR(140.0, beats->getLastMarkerBpm().value(), 0.01);
    expectOnExportedBeats(beats, grid);
}

TEST(RekordboxBeatGridTest, BeatMapKeepsBeatsBeforeTrackStartAtOneMillisecond) {
    auto grid = exportedGrid(-1.0, 8, 12800);
    grid[4].tempo = 0;
    const auto beats = beatsFromGrid(kSampleRate, grid, 1, QString());
    ASSERT_TRUE(beats);
    EXPECT_DOUBLE_EQ(kSampleRate.value() / 1000.0,
            beats->findClosestBeat(audio::kStartFramePos).value());
}

TEST(RekordboxBeatGridTest, MissingTempoKeepsExportedPositions) {
    auto grid = exportedGrid(1000.0, 8, 12800);
    grid[4].tempo = 0;
    const auto beats = beatsFromGrid(kSampleRate, grid, 1, QString());
    ASSERT_TRUE(beats);
    for (const auto& beat : grid) {
        EXPECT_NEAR(beat.position.value(), beats->findClosestBeat(beat.position).value(), 1.0);
    }
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
    const QByteArray tags = analyzeTag("PQTZ", grid) + cueTag(false, cueMs) + cueTag(true, cueMs);
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
