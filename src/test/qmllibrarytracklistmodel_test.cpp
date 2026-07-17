#include <gtest/gtest.h>

#include <QAbstractTableModel>
#include <QSqlDatabase>
#include <QVariant>
#include <QVector>

#include <utility>

#include "library/trackmodel.h"
#include "qml/qmllibrarytracklistmodel.h"
#include "test/mixxxtest.h"
#include "util/duration.h"

namespace {

class FakeTrackModel final : public QAbstractTableModel, public TrackModel {
  public:
    struct Row {
        QString title;
        QString artist;
        QString album;
        QString albumArtist;
        QString key;
        QVariant bpmDisplay;
        QVariant bpmEdit;
        QVariant bpmUser;
        QVariant durationDisplay;
        QVariant durationEdit;
        QVariant durationUser;
        QUrl fileUrl;
    };

    FakeTrackModel(QVector<Row> rows, QString key)
            : TrackModel(QSqlDatabase(), "qml-track-list-test"),
              m_rows(std::move(rows)),
              m_key(std::move(key)) {
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        const Row& row = m_rows.at(index.row());
        switch (index.column()) {
        case TitleColumn:
            return role == Qt::DisplayRole ? QVariant(row.title) : QVariant();
        case ArtistColumn:
            return role == Qt::DisplayRole ? QVariant(row.artist) : QVariant();
        case AlbumColumn:
            return role == Qt::DisplayRole ? QVariant(row.album) : QVariant();
        case AlbumArtistColumn:
            return role == Qt::DisplayRole ? QVariant(row.albumArtist) : QVariant();
        case KeyColumn:
            return role == Qt::DisplayRole ? QVariant(row.key) : QVariant();
        case BpmColumn:
            return numericData(role, row.bpmDisplay, row.bpmEdit, row.bpmUser);
        case DurationColumn:
            return numericData(
                    role, row.durationDisplay, row.durationEdit, row.durationUser);
        default:
            return {};
        }
    }

    TrackPointer getTrack(const QModelIndex& index) const override {
        Q_UNUSED(index);
        return {};
    }

    TrackPointer getTrackByRef(const TrackRef& trackRef) const override {
        Q_UNUSED(trackRef);
        return {};
    }

    QUrl getTrackUrl(const QModelIndex& index) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        return m_rows.at(index.row()).fileUrl;
    }

    QString getTrackLocation(const QModelIndex& index) const override {
        return getTrackUrl(index).toLocalFile();
    }

    TrackId getTrackId(const QModelIndex& index) const override {
        Q_UNUSED(index);
        return {};
    }

    CoverInfo getCoverInfo(const QModelIndex& index) const override {
        Q_UNUSED(index);
        return {};
    }

    const QVector<int> getTrackRows(TrackId trackId) const override {
        Q_UNUSED(trackId);
        return {};
    }

    void search(const QString& searchText) override {
        m_search = searchText;
    }

    const QString currentSearch() const override {
        return m_search;
    }

    bool isColumnInternal(int column) override {
        Q_UNUSED(column);
        return false;
    }

    bool isColumnHiddenByDefault(int column) override {
        Q_UNUSED(column);
        return false;
    }

    SortColumnId sortColumnIdFromColumnIndex(int index) const override {
        switch (index) {
        case TitleColumn:
            return SortColumnId::Title;
        case ArtistColumn:
            return SortColumnId::Artist;
        case AlbumColumn:
            return SortColumnId::Album;
        case AlbumArtistColumn:
            return SortColumnId::AlbumArtist;
        case BpmColumn:
            return SortColumnId::Bpm;
        case KeyColumn:
            return SortColumnId::Key;
        case DurationColumn:
            return SortColumnId::Duration;
        default:
            return SortColumnId::Invalid;
        }
    }

    int columnIndexFromSortColumnId(SortColumnId column) const override {
        switch (column) {
        case SortColumnId::Title:
            return TitleColumn;
        case SortColumnId::Artist:
            return ArtistColumn;
        case SortColumnId::Album:
            return AlbumColumn;
        case SortColumnId::AlbumArtist:
            return AlbumArtistColumn;
        case SortColumnId::Bpm:
            return BpmColumn;
        case SortColumnId::Key:
            return KeyColumn;
        case SortColumnId::Duration:
            return DurationColumn;
        default:
            return -1;
        }
    }

    void select() override {
        ++m_selectCallCount;
    }

    QString modelKey(bool noSearch) const override {
        Q_UNUSED(noSearch);
        return m_key;
    }

    bool updateTrackGenre(Track* pTrack, const QString& genre) const override {
        Q_UNUSED(pTrack);
        Q_UNUSED(genre);
        return false;
    }

#if defined(__EXTRA_METADATA__)
    bool updateTrackMood(Track* pTrack, const QString& mood) const override {
        Q_UNUSED(pTrack);
        Q_UNUSED(mood);
        return false;
    }
#endif

    int selectCallCount() const {
        return m_selectCallCount;
    }

  private:
    enum Column {
        TitleColumn,
        ArtistColumn,
        AlbumColumn,
        AlbumArtistColumn,
        BpmColumn,
        KeyColumn,
        DurationColumn,
        ColumnCount,
    };

    static QVariant numericData(int role,
            const QVariant& display,
            const QVariant& edit,
            const QVariant& user) {
        switch (role) {
        case Qt::DisplayRole:
            return display;
        case Qt::EditRole:
            return edit;
        case Qt::UserRole:
            return user;
        default:
            return {};
        }
    }

    const QVector<Row> m_rows;
    const QString m_key;
    QString m_search;
    int m_selectCallCount{0};
};

class QmlLibraryTrackListModelTest : public MixxxTest {
};

TEST_F(QmlLibraryTrackListModelTest, SwitchesSourceAndMapsTrackRoles) {
    const QUrl firstUrl(QStringLiteral("file:///C:/Music/First.flac"));
    FakeTrackModel::Row firstRow;
    firstRow.title = QStringLiteral("First Title");
    firstRow.artist = QStringLiteral("Artist One");
    firstRow.album = QStringLiteral("Album One");
    firstRow.albumArtist = QStringLiteral("Various Artists");
    firstRow.key = QStringLiteral("8A");
    firstRow.bpmDisplay = QStringLiteral("128.50");
    firstRow.bpmEdit = 128.5;
    firstRow.durationDisplay = QStringLiteral("4:05");
    firstRow.durationEdit =
            QVariant::fromValue(mixxx::Duration::fromSeconds(245.25));
    firstRow.fileUrl = firstUrl;

    FakeTrackModel::Row extraRow = firstRow;
    extraRow.title = QStringLiteral("Extra Title");
    extraRow.fileUrl = QUrl(QStringLiteral("file:///C:/Music/Extra.flac"));

    QVector<FakeTrackModel::Row> firstRows;
    firstRows.append(firstRow);
    firstRows.append(extraRow);
    FakeTrackModel firstModel(std::move(firstRows), QStringLiteral("first"));

    const QUrl secondUrl(QStringLiteral("file:///D:/USB/Second.wav"));
    FakeTrackModel::Row secondRow;
    secondRow.title = QStringLiteral("Second Title");
    secondRow.artist = QStringLiteral("Artist Two");
    secondRow.key = QStringLiteral("11B");
    secondRow.bpmDisplay = QStringLiteral("122.25");
    secondRow.bpmEdit = QStringLiteral("not numeric");
    secondRow.bpmUser = 122.25;
    secondRow.durationDisplay = QStringLiteral("6:07");
    secondRow.durationEdit = QStringLiteral("not a duration");
    secondRow.durationUser = 367.5;
    secondRow.fileUrl = secondUrl;

    QVector<FakeTrackModel::Row> secondRows;
    secondRows.append(secondRow);
    FakeTrackModel secondModel(std::move(secondRows), QStringLiteral("second"));

    mixxx::qml::QmlLibraryTrackListModel model(&firstModel);
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.columnCount(), 1);
    EXPECT_EQ(firstModel.selectCallCount(), 1);

    const QModelIndex firstIndex = model.index(0, 0);
    EXPECT_QSTRING_EQ(QStringLiteral("First Title"),
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::TitleRole)
                    .toString());
    EXPECT_QSTRING_EQ(QStringLiteral("Artist One"),
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::ArtistRole)
                    .toString());
    EXPECT_QSTRING_EQ(QStringLiteral("8A"),
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::KeyRole)
                    .toString());
    EXPECT_DOUBLE_EQ(128.5,
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::BpmRole)
                    .toDouble());
    EXPECT_DOUBLE_EQ(245.25,
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::DurationRole)
                    .toDouble());
    EXPECT_EQ(firstUrl,
            model.data(firstIndex,
                         mixxx::qml::QmlLibraryTrackListModel::FileUrlRole)
                    .toUrl());

    EXPECT_TRUE(model.setTrackModel(&secondModel));
    EXPECT_EQ(model.sourceModel(), &secondModel);
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(secondModel.selectCallCount(), 1);

    const QModelIndex secondIndex = model.index(0, 0);
    EXPECT_QSTRING_EQ(QStringLiteral("Second Title"),
            model.data(secondIndex,
                         mixxx::qml::QmlLibraryTrackListModel::TitleRole)
                    .toString());
    EXPECT_DOUBLE_EQ(122.25,
            model.data(secondIndex,
                         mixxx::qml::QmlLibraryTrackListModel::BpmRole)
                    .toDouble());
    EXPECT_DOUBLE_EQ(367.5,
            model.data(secondIndex,
                         mixxx::qml::QmlLibraryTrackListModel::DurationRole)
                    .toDouble());
    EXPECT_EQ(secondUrl,
            model.data(secondIndex,
                         mixxx::qml::QmlLibraryTrackListModel::FileUrlRole)
                    .toUrl());

    const QVariantMap row = model.get(0).toMap();
    EXPECT_QSTRING_EQ(QStringLiteral("Second Title"), row.value("title").toString());
    EXPECT_DOUBLE_EQ(122.25, row.value("bpm").toDouble());
    EXPECT_DOUBLE_EQ(367.5, row.value("duration").toDouble());
    EXPECT_EQ(secondUrl, row.value("fileUrl").toUrl());

    const auto roles = model.roleNames();
    EXPECT_EQ(QByteArray("title"),
            roles.value(mixxx::qml::QmlLibraryTrackListModel::TitleRole));
    EXPECT_EQ(QByteArray("bpm"),
            roles.value(mixxx::qml::QmlLibraryTrackListModel::BpmRole));
    EXPECT_EQ(QByteArray("duration"),
            roles.value(mixxx::qml::QmlLibraryTrackListModel::DurationRole));
    EXPECT_EQ(QByteArray("fileUrl"),
            roles.value(mixxx::qml::QmlLibraryTrackListModel::FileUrlRole));

    EXPECT_TRUE(model.setTrackModel(nullptr));
    EXPECT_EQ(model.sourceModel(), nullptr);
    EXPECT_EQ(model.rowCount(), 0);
}

} // namespace
