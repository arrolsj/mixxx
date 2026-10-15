#include "library/rekordbox/rekordboxfeature.h"

#include <mp3guessenc.h>
#include <rekordbox_anlz.h>
#include <rekordbox_pdb.h>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QTextCodec>
#include <QtDebug>
#include <QtEndian>
#include <algorithm>
#include <utility>

#include "engine/engine.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxbeatgrid.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_rekordboxfeature.cpp"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/sandbox.h"
#include "waveform/waveform.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

#define IS_RECORDBOX_DEVICE "::isRecordboxDevice::"
#define IS_NOT_RECORDBOX_DEVICE "::isNotRecordboxDevice::"

namespace {

const QString kRekordboxLibraryTable = QStringLiteral("rekordbox_library");
const QString kRekordboxPlaylistsTable = QStringLiteral("rekordbox_playlists");
const QString kRekordboxPlaylistTracksTable = QStringLiteral("rekordbox_playlist_tracks");

const QString kPdbPath = QStringLiteral("PIONEER/rekordbox/export.pdb");
const QString kPLaylistPathDelimiter = QStringLiteral("-->");

constexpr quint32 kPdbHeaderSize = 28;
constexpr quint32 kPdbTablePointerSize = 16;
constexpr quint32 kMinimumPdbPageSize = 40;
constexpr quint32 kDeviceNameOffsetInHistoryRow = 30;
constexpr int kMaximumPlaylistDepth = 256;
constexpr int kSafeStopReferenceReleaseCheckIntervalMillis = 20;
constexpr int kSafeStopReferenceReleaseTimeoutMillis = 5000;

QString treeItemDevicePath(const TreeItem* pItem) {
    const QVariantList data = pItem ? pItem->getData().toList() : QVariantList{};
    return data.isEmpty() ? QString() : data.constFirst().toString();
}

QString treeItemDeviceId(const TreeItem* pItem) {
    const QVariantList data = pItem ? pItem->getData().toList() : QVariantList{};
    return data.size() > 2 ? data.at(2).toString() : QString();
}

bool pathIsWithinMount(const QString& path, const QString& mountPoint) {
    if (path.isEmpty() || mountPoint.isEmpty()) {
        return false;
    }
    QString normalizedPath = QDir::cleanPath(path);
    QString normalizedMount = QDir::cleanPath(mountPoint);
    if (!normalizedMount.endsWith(QLatin1Char('/'))) {
        normalizedMount.append(QLatin1Char('/'));
    }
#if defined(__WINDOWS__)
    return normalizedPath.startsWith(normalizedMount, Qt::CaseInsensitive);
#else
    return normalizedPath.startsWith(normalizedMount, Qt::CaseSensitive);
#endif
}

QString getText(rekordbox_pdb_t::device_sql_string_t* deviceString);

QString rekordboxDeviceNameFromDatabase(const QFileInfo& databaseFileInfo) {
    try {
        std::ifstream databaseStream(
                databaseFileInfo.filePath().toStdString(),
                std::ifstream::binary);
        if (!databaseStream.is_open()) {
            return {};
        }

        kaitai::kstream databaseKStream(&databaseStream);
        rekordbox_pdb_t database(&databaseKStream);

        for (const auto* table : *database.tables()) {
            if (table->type() != rekordbox_pdb_t::PAGE_TYPE_HISTORY) {
                continue;
            }

            const quint32 lastPageIndex = table->last_page()->index();
            rekordbox_pdb_t::page_ref_t* currentPageRef = table->first_page();
            QSet<quint32> visitedPageIndexes;
            while (currentPageRef &&
                    !visitedPageIndexes.contains(currentPageRef->index())) {
                const quint32 currentPageIndex = currentPageRef->index();
                visitedPageIndexes.insert(currentPageIndex);

                rekordbox_pdb_t::page_t* page = currentPageRef->body();
                if (page->type() != rekordbox_pdb_t::PAGE_TYPE_HISTORY) {
                    break;
                }

                if (page->is_data_page()) {
                    for (auto* rowGroup : *page->row_groups()) {
                        for (auto* rowRef : *rowGroup->rows()) {
                            if (!rowRef->present() || rowRef->row_base() < 0) {
                                continue;
                            }

                            const quint64 deviceNameOffset =
                                    static_cast<quint64>(currentPageIndex) *
                                            database.len_page() +
                                    static_cast<quint64>(rowRef->row_base()) +
                                    kDeviceNameOffsetInHistoryRow;
                            if (deviceNameOffset >=
                                    static_cast<quint64>(databaseFileInfo.size())) {
                                continue;
                            }

                            databaseKStream.seek(
                                    static_cast<std::streamoff>(deviceNameOffset));
                            rekordbox_pdb_t::device_sql_string_t deviceName(
                                    &databaseKStream, nullptr, &database);
                            const QString name = getText(&deviceName).trimmed();
                            if (!name.isEmpty()) {
                                return name;
                            }
                        }
                    }
                }

                if (currentPageIndex == lastPageIndex) {
                    break;
                }
                currentPageRef = page->next_page();
            }
        }
    } catch (...) {
        qWarning() << "Unable to read Rekordbox device name from"
                   << databaseFileInfo.filePath();
    }

    return {};
}
quint32 readLittleEndianU32(const QByteArray& bytes, qsizetype offset) {
    return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

bool hasReadableRekordboxTablePage(
        QFile& databaseFile,
        quint32 pageSize,
        quint32 tableType,
        quint32 pageIndex) {
    const quint64 pageOffset =
            static_cast<quint64>(pageSize) * static_cast<quint64>(pageIndex);
    if (pageOffset + kMinimumPdbPageSize >
            static_cast<quint64>(databaseFile.size()) ||
            !databaseFile.seek(static_cast<qint64>(pageOffset))) {
        return false;
    }

    const QByteArray pageHeader = databaseFile.read(kMinimumPdbPageSize);
    if (pageHeader.size() != static_cast<qsizetype>(kMinimumPdbPageSize)) {
        return false;
    }

    const quint32 storedPageIndex = readLittleEndianU32(pageHeader, 4);
    return (storedPageIndex == 0 || storedPageIndex == pageIndex) &&
            readLittleEndianU32(pageHeader, 8) == tableType;
}

bool hasValidRekordboxDatabaseHeader(const QFileInfo& databaseFileInfo) {
    QFile databaseFile(databaseFileInfo.filePath());
    if (!databaseFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray header = databaseFile.read(kPdbHeaderSize);
    if (header.size() != static_cast<qsizetype>(kPdbHeaderSize)) {
        return false;
    }

    const quint32 pageSize = readLittleEndianU32(header, 4);
    const quint32 tableCount = readLittleEndianU32(header, 8);
    const qint64 fileSize = databaseFileInfo.size();
    if (pageSize < kMinimumPdbPageSize ||
            fileSize < pageSize ||
            tableCount == 0 ||
            tableCount > (pageSize - kPdbHeaderSize) / kPdbTablePointerSize) {
        return false;
    }

    const qsizetype tableBytes =
            static_cast<qsizetype>(tableCount) * kPdbTablePointerSize;
    const QByteArray tablePointers = databaseFile.read(tableBytes);
    if (tablePointers.size() != tableBytes) {
        return false;
    }

    const quint64 totalPages = static_cast<quint64>(fileSize) / pageSize;
    bool hasTracks = false;
    bool hasPlaylistTree = false;
    bool hasPlaylistEntries = false;
    for (quint32 tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
        const qsizetype offset =
                static_cast<qsizetype>(tableIndex) * kPdbTablePointerSize;
        const quint32 tableType = readLittleEndianU32(tablePointers, offset);
        const bool isRequiredTable =
                tableType == rekordbox_pdb_t::PAGE_TYPE_TRACKS ||
                tableType == rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE ||
                tableType == rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES;
        if (!isRequiredTable) {
            continue;
        }

        const quint32 firstPage = readLittleEndianU32(tablePointers, offset + 8);
        const quint32 lastPage = readLittleEndianU32(tablePointers, offset + 12);
        if (firstPage == 0 || lastPage == 0 ||
                firstPage >= totalPages || lastPage >= totalPages) {
            return false;
        }
        if (!hasReadableRekordboxTablePage(
                    databaseFile, pageSize, tableType, firstPage) ||
                !hasReadableRekordboxTablePage(
                        databaseFile, pageSize, tableType, lastPage)) {
            return false;
        }

        hasTracks |= tableType == rekordbox_pdb_t::PAGE_TYPE_TRACKS;
        hasPlaylistTree |= tableType == rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE;
        hasPlaylistEntries |= tableType == rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES;
    }

    return hasTracks && hasPlaylistTree && hasPlaylistEntries;
}

enum class IDForColor : uint8_t {
    Pink = 1,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple
};

constexpr mixxx::RgbColor kColorForIDPink(0xF870F8);
constexpr mixxx::RgbColor kColorForIDRed(0xF87090);
constexpr mixxx::RgbColor kColorForIDOrange(0xF8A030);
constexpr mixxx::RgbColor kColorForIDYellow(0xF8E331);
constexpr mixxx::RgbColor kColorForIDGreen(0x1EE000);
constexpr mixxx::RgbColor kColorForIDAqua(0x16C0F8);
constexpr mixxx::RgbColor kColorForIDBlue(0x0150F8);
constexpr mixxx::RgbColor kColorForIDPurple(0x9808F8);
constexpr mixxx::RgbColor kColorForIDNoColor(0x0);

struct memory_cue_t {
    mixxx::audio::FramePos startPosition;
    QString comment;
    mixxx::RgbColor::optional_t color;
};

void addOrUpdateMemoryCue(QList<memory_cue_t>* pMemoryCues,
        mixxx::audio::FramePos startPosition,
        const QString& comment,
        mixxx::RgbColor::optional_t color) {
    for (auto& memoryCue : *pMemoryCues) {
        if (memoryCue.startPosition != startPosition) {
            continue;
        }
        if (!comment.isEmpty()) {
            memoryCue.comment = comment;
        }
        if (color) {
            memoryCue.color = color;
        }
        return;
    }
    pMemoryCues->append({startPosition, comment, color});
}

bool createLibraryTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox library table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    rb_id INTEGER,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year INTEGER,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT UNIQUE,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    rating INTEGER,"
            "    analyze_path TEXT UNIQUE,"
            "    device TEXT,"
            "    color INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistsTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlists table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    name TEXT UNIQUE,"
            "    device TEXT NOT NULL"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistTracksTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlist tracks table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
            "    track_id INTEGER REFERENCES rekordbox_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool dropTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Dropping Rekordbox table: " << tableName;

    QSqlQuery query(database);
    query.prepare("DROP TABLE IF EXISTS " + tableName);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

// This function is executed outside the main thread. The snapshot generation
// fences its result so a late scan can never resurrect a removed device.
RekordboxDeviceScanResult findRekordboxDevices(
        QList<mixxx::removablemedia::RemovableMediaDevice> mediaDevices,
        quint64 generation,
        QSet<QString> loadBlockedDeviceIds) {
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    RekordboxDeviceScanResult result;
    result.generation = generation;

    for (auto& mediaDevice : mediaDevices) {
        mediaDevice.generation = generation;
        if (loadBlockedDeviceIds.contains(mediaDevice.deviceId)) {
            // Once STOP is accepted, discovery must not reopen export.pdb or
            // any other path in the physical removal group.
            mediaDevice.state =
                    mixxx::removablemedia::MediaState::Quiescing;
            result.mediaDevices.append(std::move(mediaDevice));
            continue;
        }
        mediaDevice.state = mixxx::removablemedia::MediaState::Scanning;
        bool databaseFileSeen = false;
        bool validDatabaseFound = false;

        for (const QString& mountPoint : std::as_const(mediaDevice.mountPoints)) {
            const QString devicePath = QDir::cleanPath(mountPoint);
            const QFileInfo databaseFileInfo(
                    QDir(devicePath).filePath(kPdbPath));
            if (!databaseFileInfo.exists() || !databaseFileInfo.isFile()) {
                continue;
            }
            databaseFileSeen = true;
            if (!hasValidRekordboxDatabaseHeader(databaseFileInfo)) {
                continue;
            }

            validDatabaseFound = true;
            QString displayName = rekordboxDeviceNameFromDatabase(
                    databaseFileInfo)
                                          .trimmed();
            if (displayName.isEmpty()) {
                displayName = mediaDevice.displayName;
            }
            mediaDevice.displayName = displayName;
            mediaDevice.state = mixxx::removablemedia::MediaState::Ready;
            mediaDevice.errorCode = mixxx::removablemedia::ErrorCode::None;
            mediaDevice.errorMessage.clear();

            const QVariantList data{
                    devicePath,
                    QStringLiteral(IS_RECORDBOX_DEVICE),
                    mediaDevice.deviceId,
                    QVariant::fromValue<qulonglong>(generation)};
            result.treeItems.append(new TreeItem(displayName, data));
            break;
        }

        if (!validDatabaseFound) {
            mediaDevice.state = mixxx::removablemedia::MediaState::Failed;
            mediaDevice.errorCode =
                    mixxx::removablemedia::ErrorCode::InvalidDatabase;
            mediaDevice.errorMessage = databaseFileSeen
                    ? QObject::tr("The Rekordbox database is invalid or damaged.")
                    : QObject::tr("No Rekordbox export database was found.");
        }
        result.mediaDevices.append(std::move(mediaDevice));
    }

    return result;
}

template<typename Base, typename T>
inline bool instanceof (const T* ptr) {
    return dynamic_cast<const Base*>(ptr) != nullptr;
}

QString fromUtf16LeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    return QTextCodec::codecForName("UTF-16LE")
            ->toUnicode(toConvert.data(), static_cast<int>(toConvert.length()));
}

QString fromUtf16BeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    int length = static_cast<int>(toConvert.length()) - 2; // strip off trailing nullbyte
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(toConvert.data(), length);
}

// Functions getText and parseDeviceDB are roughly based on the following Java file:
// https://github.com/Deep-Symmetry/crate-digger/commit/f09fa9fc097a2a428c43245ddd542ac1370c1adc
// getText is needed because the strings in the PDB file "have a variety of obscure representations".

QString getText(rekordbox_pdb_t::device_sql_string_t* deviceString) {
    QString text;

    if (instanceof <rekordbox_pdb_t::device_sql_short_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_short_ascii_t* shortAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_short_ascii_t*>(deviceString->body());
        text = QString::fromStdString(shortAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_ascii_t* longAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_long_ascii_t*>(deviceString->body());
        text = QString::fromStdString(longAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_utf16le_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_utf16le_t* longUtf16leString =
                static_cast<rekordbox_pdb_t::device_sql_long_utf16le_t*>(deviceString->body());
        text = fromUtf16LeString(longUtf16leString->text());
    }

    // Some strings read from Rekordbox *.PDB files contain random null characters
    // which if not removed cause Mixxx to crash when attempting to read file paths
    return text.remove(QChar('\x0'));
}

int createDevicePlaylist(
        QSqlDatabase& database,
        const QString& devicePath,
        const QString& deviceId) {
    int playlistID = kInvalidPlaylistId;

    QSqlQuery queryInsertIntoDevicePlaylist(database);
    queryInsertIntoDevicePlaylist.prepare(
            "INSERT INTO " + kRekordboxPlaylistsTable +
            " (name, device) "
            "VALUES (:name, :device)");

    queryInsertIntoDevicePlaylist.bindValue(":name", devicePath);
    queryInsertIntoDevicePlaylist.bindValue(":device", deviceId);

    if (!queryInsertIntoDevicePlaylist.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylist)
                << "devicePath: " << devicePath;
        return playlistID;
    }

    bool idIsValid = false;
    playlistID = queryInsertIntoDevicePlaylist.lastInsertId().toInt(&idIsValid);
    if (!idIsValid || playlistID <= 0) {
        qWarning() << "Failed to read inserted Rekordbox device playlist ID"
                   << devicePath;
        return kInvalidPlaylistId;
    }

    return playlistID;
}

mixxx::RgbColor colorFromID(int colorID) {
    switch (static_cast<IDForColor>(colorID)) {
    case IDForColor::Pink:
        return kColorForIDPink;
    case IDForColor::Red:
        return kColorForIDRed;
    case IDForColor::Orange:
        return kColorForIDOrange;
    case IDForColor::Yellow:
        return kColorForIDYellow;
    case IDForColor::Green:
        return kColorForIDGreen;
    case IDForColor::Aqua:
        return kColorForIDAqua;
    case IDForColor::Blue:
        return kColorForIDBlue;
    case IDForColor::Purple:
        return kColorForIDPurple;
    }
    return kColorForIDNoColor;
}

mixxx::RgbColor::optional_t cueColorFromID(int colorID) {
    if (colorID <= 0 || colorID > static_cast<int>(IDForColor::Purple)) {
        return mixxx::RgbColor::nullopt();
    }
    return colorFromID(colorID);
}

void insertTrack(
        rekordbox_pdb_t::track_row_t* track,
        QSqlQuery& query,
        QSqlQuery& queryInsertIntoDevicePlaylistTracks,
        QMap<uint32_t, QString>& artistsMap,
        QMap<uint32_t, QString>& albumsMap,
        QMap<uint32_t, QString>& genresMap,
        QMap<uint32_t, QString>& keysMap,
        const QString& devicePath,
        const QString& deviceId,
        QHash<uint32_t, int>& trackIdsByRekordboxId,
        int audioFilesCount) {
    int rbID = static_cast<int>(track->id());
    QString title = getText(track->title());
    QString artist = artistsMap[track->artist_id()];
    QString album = albumsMap[track->album_id()];
    QString year = QString::number(track->year());
    QString genre = genresMap[track->genre_id()];
    QString location = devicePath + getText(track->file_path());
    float bpm = static_cast<float>(track->tempo() / 100.0);
    int bitrate = static_cast<int>(track->bitrate());
    QString key = keysMap[track->key_id()];
    int playtime = static_cast<int>(track->duration());
    int rating = static_cast<int>(track->rating());
    QString comment = getText(track->comment());
    QString tracknumber = QString::number(track->track_number());
    QString anlzPath = devicePath + getText(track->analyze_path());

    query.bindValue(":rb_id", rbID);
    query.bindValue(":artist", artist);
    query.bindValue(":title", title);
    query.bindValue(":album", album);
    query.bindValue(":genre", genre);
    query.bindValue(":year", year);
    query.bindValue(":duration", playtime);
    query.bindValue(":location", location);
    query.bindValue(":rating", rating);
    query.bindValue(":comment", comment);
    query.bindValue(":tracknumber", tracknumber);
    query.bindValue(":key", key);
    query.bindValue(":bpm", bpm);
    query.bindValue(":bitrate", bitrate);
    query.bindValue(":analyze_path", anlzPath);
    query.bindValue(":device", deviceId);
    query.bindValue(":color",
            mixxx::RgbColor::toQVariant(
                    colorFromID(static_cast<int>(track->color_id()))));

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }

    bool idIsValid = false;
    const int trackID = query.lastInsertId().toInt(&idIsValid);
    if (!idIsValid || trackID <= 0) {
        qWarning() << "Failed to read inserted Rekordbox track ID" << rbID;
        return;
    }
    trackIdsByRekordboxId.insert(static_cast<uint32_t>(rbID), trackID);

    // Insert into device all tracks playlist
    queryInsertIntoDevicePlaylistTracks.bindValue(":track_id", trackID);
    queryInsertIntoDevicePlaylistTracks.bindValue(":position", audioFilesCount);

    if (!queryInsertIntoDevicePlaylistTracks.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylistTracks)
                << "trackID:" << trackID
                << "position:" << audioFilesCount;
    }
}

bool buildPlaylistTree(
        QSqlDatabase& database,
        QList<RekordboxPlaylistTreeEntry>& playlistTreeEntries,
        int parentEntryIndex,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& deviceId,
        const QString& playlistPath,
        const QHash<uint32_t, int>& trackIdsByRekordboxId,
        QSet<uint32_t>& visitedPlaylistIds,
        int depth);

RekordboxImportResult parseDeviceDB(
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        QString device,
        QString devicePath,
        QString deviceId,
        quint64 generation) {
    RekordboxImportResult importResult;
    importResult.devicePath = devicePath;
    importResult.deviceLabel = device;
    importResult.deviceId = deviceId;
    importResult.generation = generation;

    qDebug() << "parseDeviceDB device: " << device << " devicePath: " << devicePath;
    QElapsedTimer parseTimer;
    parseTimer.start();

    QString dbPath = devicePath + QStringLiteral("/") + kPdbPath;

    if (!QFile(dbPath).exists()) {
        return importResult;
    }

    // The pooler limits the lifetime all thread-local connections,
    // that should be closed immediately before exiting this function.
    const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);

    //Open the database connection in this thread.
    VERIFY_OR_DEBUG_ASSERT(database.isOpen()) {
        qDebug() << "Failed to open database for Rekordbox parser."
                 << database.lastError();
        return importResult;
    }

    //Give thread a low priority
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    ScopedTransaction transaction(database);

    QSqlQuery query(database);
    query.prepare("INSERT INTO " + kRekordboxLibraryTable +
            " (rb_id, artist, title, album, year,"
            "genre,comment,tracknumber,bpm, bitrate,duration, location,"
            "rating,key,analyze_path,device,color) VALUES (:rb_id, :artist, "
            ":title, :album, :year,:genre,"
            ":comment, :tracknumber,:bpm, :bitrate,:duration, :location,"
            ":rating,:key,:analyze_path,:device,:color)");

    int audioFilesCount = 0;

    // Create a playlist for all the tracks on a device
    int playlistID = createDevicePlaylist(
            database, devicePath, deviceId);
    if (playlistID == kInvalidPlaylistId) {
        return importResult;
    }

    QSqlQuery queryInsertIntoDevicePlaylistTracks(database);
    queryInsertIntoDevicePlaylistTracks.prepare(
            "INSERT INTO " + kRekordboxPlaylistTracksTable +
            " (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)");

    queryInsertIntoDevicePlaylistTracks.bindValue(":playlist_id", playlistID);

    mixxx::FileInfo fileInfo(dbPath);
    if (!Sandbox::askForAccess(&fileInfo)) {
        return importResult;
    }
    std::ifstream ifs(dbPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_pdb_t rekordboxDB = rekordbox_pdb_t(&ks);

    // There are other types of tables (eg. COLOR), these are the only ones we are
    // interested at the moment. Perhaps when/if
    // https://github.com/mixxxdj/mixxx/issues/6852
    // is completed, this can be revisited.
    // Attempt was made to also recover HISTORY
    // playlists (which are found on removable Rekordbox devices), however
    // they didn't appear to contain valid row_ref_t structures.
    constexpr int totalTables = 8;

    rekordbox_pdb_t::page_type_t tableOrder[totalTables] = {
            rekordbox_pdb_t::PAGE_TYPE_KEYS,
            rekordbox_pdb_t::PAGE_TYPE_GENRES,
            rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
            rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
            rekordbox_pdb_t::PAGE_TYPE_TRACKS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE,
            rekordbox_pdb_t::PAGE_TYPE_HISTORY};

    QMap<uint32_t, QString> keysMap;
    QMap<uint32_t, QString> genresMap;
    QMap<uint32_t, QString> artistsMap;
    QMap<uint32_t, QString> albumsMap;
    QMap<uint32_t, QString> playlistNameMap;
    QMap<uint32_t, bool> playlistIsFolderMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTreeMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTrackMap;
    QHash<uint32_t, int> trackIdsByRekordboxId;

    bool folderOrPlaylistFound = false;
    const quint64 pdbPageSize = rekordboxDB.len_page();
    const quint64 maximumPageCount = pdbPageSize == 0
            ? 0
            : static_cast<quint64>(QFileInfo(dbPath).size()) /
                    pdbPageSize;
    if (maximumPageCount == 0) {
        return importResult;
    }

    for (int tableOrderIndex = 0; tableOrderIndex < totalTables; tableOrderIndex++) {
        for (const auto& table : *rekordboxDB.tables()) {
            if (table->type() == tableOrder[tableOrderIndex]) {
                uint16_t lastIndex = table->last_page()->index();
                rekordbox_pdb_t::page_ref_t* currentRef = table->first_page();
                QSet<quint32> visitedPageIndexes;

                while (currentRef) {
                    const quint32 currentPageIndex = currentRef->index();
                    if (currentPageIndex >= maximumPageCount ||
                            visitedPageIndexes.contains(currentPageIndex)) {
                        qWarning() << "Rejecting cyclic or out-of-range Rekordbox page chain"
                                   << currentPageIndex << "of"
                                   << maximumPageCount;
                        return importResult;
                    }
                    visitedPageIndexes.insert(currentPageIndex);
                    rekordbox_pdb_t::page_t* page = currentRef->body();

                    if (page->is_data_page()) {
                        for (const auto& rowgroup : *page->row_groups()) {
                            for (const auto& rowRef : *rowgroup->rows()) {
                                if (rowRef->present()) {
                                    switch (tableOrder[tableOrderIndex]) {
                                    case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
                                        auto* key =
                                                static_cast<rekordbox_pdb_t::key_row_t*>(
                                                        rowRef->body());
                                        keysMap[key->id()] = getText(key->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
                                        auto* genre =
                                                static_cast<rekordbox_pdb_t::genre_row_t*>(
                                                        rowRef->body());
                                        genresMap[genre->id()] = getText(genre->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
                                        auto* artist =
                                                static_cast<rekordbox_pdb_t::artist_row_t*>(
                                                        rowRef->body());
                                        artistsMap[artist->id()] = getText(artist->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
                                        auto* album =
                                                static_cast<rekordbox_pdb_t::album_row_t*>(
                                                        rowRef->body());
                                        albumsMap[album->id()] = getText(album->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
                                        auto* playlistEntry =
                                                static_cast<rekordbox_pdb_t::playlist_entry_row_t*>(
                                                        rowRef->body());
                                        playlistTrackMap
                                                [playlistEntry->playlist_id()]
                                                [playlistEntry->entry_index()] =
                                                        playlistEntry
                                                                ->track_id();
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
                                        insertTrack(
                                                static_cast<rekordbox_pdb_t::track_row_t*>(
                                                        rowRef->body()),
                                                query,
                                                queryInsertIntoDevicePlaylistTracks,
                                                artistsMap,
                                                albumsMap,
                                                genresMap,
                                                keysMap,
                                                devicePath,
                                                deviceId,
                                                trackIdsByRekordboxId,
                                                audioFilesCount);

                                        audioFilesCount++;
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
                                        auto* playlistTree =
                                                static_cast<rekordbox_pdb_t::playlist_tree_row_t*>(
                                                        rowRef->body());

                                        playlistNameMap[playlistTree->id()] =
                                                getText(playlistTree->name());
                                        playlistIsFolderMap[playlistTree
                                                                    ->id()] =
                                                playlistTree->is_folder();
                                        playlistTreeMap
                                                [playlistTree->parent_id()]
                                                [playlistTree->sort_order()] =
                                                        playlistTree->id();

                                        folderOrPlaylistFound = true;
                                    } break;
                                    default:
                                        // we currently don't handle any other
                                        // data, even though there is more.
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (currentRef->index() == lastIndex) {
                        break;
                    } else {
                        currentRef = page->next_page();
                    }
                }
                if (!currentRef) {
                    qWarning() << "Rejecting unterminated Rekordbox page chain";
                    return importResult;
                }
            }
        }
    }

    if (audioFilesCount > 0 || folderOrPlaylistFound) {
        // If we have found anything, recursively build playlist/folder TreeItem children
        // for the original device TreeItem
        QSet<uint32_t> visitedPlaylistIds;
        if (!buildPlaylistTree(database,
                    importResult.playlistTree,
                    -1,
                    0,
                    playlistNameMap,
                    playlistIsFolderMap,
                    playlistTreeMap,
                    playlistTrackMap,
                    deviceId,
                    devicePath,
                    trackIdsByRekordboxId,
                    visitedPlaylistIds,
                    0)) {
            return importResult;
        }
    }

    qDebug() << "Found:" << audioFilesCount
             << "audio files in Rekordbox device" << device
             << "elapsed_ms:" << parseTimer.elapsed();

    transaction.commit();
    importResult.succeeded = true;

    return importResult;
}

bool buildPlaylistTree(
        QSqlDatabase& database,
        QList<RekordboxPlaylistTreeEntry>& playlistTreeEntries,
        int parentEntryIndex,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& deviceId,
        const QString& playlistPath,
        const QHash<uint32_t, int>& trackIdsByRekordboxId,
        QSet<uint32_t>& visitedPlaylistIds,
        int depth) {
    if (depth > kMaximumPlaylistDepth) {
        qWarning() << "Rejecting over-deep Rekordbox playlist tree";
        return false;
    }
    for (uint32_t childIndex = 0;
            childIndex < (uint32_t)playlistTreeMap[parentID].size();
            childIndex++) {
        uint32_t childID = playlistTreeMap[parentID][childIndex];
        if (childID == 0) {
            continue;
        }
        if (visitedPlaylistIds.contains(childID)) {
            qWarning() << "Rejecting cyclic or duplicate Rekordbox playlist node"
                       << childID;
            return false;
        }
        visitedPlaylistIds.insert(childID);
        QString playlistItemName = playlistNameMap[childID];

        QString currentPath = playlistPath + kPLaylistPathDelimiter + playlistItemName;

        const int childEntryIndex = playlistTreeEntries.size();
        playlistTreeEntries.append(
                {parentEntryIndex, playlistItemName, currentPath});

        // Create a playlist for this child
        QSqlQuery queryInsertIntoPlaylist(database);
        queryInsertIntoPlaylist.prepare(
                "INSERT INTO " + kRekordboxPlaylistsTable +
                " (name, device) "
                "VALUES (:name, :device)");

        queryInsertIntoPlaylist.bindValue(":name", currentPath);
        queryInsertIntoPlaylist.bindValue(":device", deviceId);

        if (!queryInsertIntoPlaylist.exec()) {
            LOG_FAILED_QUERY(queryInsertIntoPlaylist)
                    << "currentPath" << currentPath;
            return false;
        }

        bool idIsValid = false;
        const int playlistID = queryInsertIntoPlaylist.lastInsertId().toInt(&idIsValid);
        if (!idIsValid || playlistID <= 0) {
            qWarning() << "Failed to read inserted Rekordbox playlist ID"
                       << currentPath;
            return false;
        }

        QSqlQuery queryInsertIntoPlaylistTracks(database);
        queryInsertIntoPlaylistTracks.prepare(
                "INSERT INTO " + kRekordboxPlaylistTracksTable +
                " (playlist_id, track_id, position) "
                "VALUES (:playlist_id, :track_id, :position)");

        if (playlistTrackMap.contains(childID)) {
            // Add playlist tracks for children
            for (uint32_t trackIndex = 1; trackIndex <=
                    static_cast<uint32_t>(playlistTrackMap[childID].size());
                    trackIndex++) {
                uint32_t rbTrackID = playlistTrackMap[childID][trackIndex];

                const auto trackIdIt = trackIdsByRekordboxId.constFind(rbTrackID);
                if (trackIdIt == trackIdsByRekordboxId.cend()) {
                    qWarning() << "Rekordbox playlist references an unknown track"
                               << rbTrackID;
                    continue;
                }
                const int trackID = trackIdIt.value();

                queryInsertIntoPlaylistTracks.bindValue(":playlist_id", playlistID);
                queryInsertIntoPlaylistTracks.bindValue(":track_id", trackID);
                queryInsertIntoPlaylistTracks.bindValue(":position", static_cast<int>(trackIndex));

                if (!queryInsertIntoPlaylistTracks.exec()) {
                    LOG_FAILED_QUERY(queryInsertIntoPlaylistTracks)
                            << "playlistID:" << playlistID
                            << "trackID:" << trackID
                            << "trackIndex:" << trackIndex;

                    return false;
                }
            }
        }

        if (playlistIsFolderMap[childID]) {
            // If this child is a folder (playlists are only leaf nodes), build playlist tree for it
            if (!buildPlaylistTree(database,
                        playlistTreeEntries,
                        childEntryIndex,
                        childID,
                        playlistNameMap,
                        playlistIsFolderMap,
                        playlistTreeMap,
                        playlistTrackMap,
                        deviceId,
                        currentPath,
                        trackIdsByRekordboxId,
                        visitedPlaylistIds,
                        depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

void clearDeviceTables(QSqlDatabase& database, TreeItem* child) {
    ScopedTransaction transaction(database);

    const QString deviceId = treeItemDeviceId(child);
    if (deviceId.isEmpty()) {
        qWarning() << "Refusing to clear Rekordbox tables without a device identity";
        return;
    }

    QSqlQuery deletePlaylistTracksQuery(database);
    deletePlaylistTracksQuery.prepare(
            "delete from " + kRekordboxPlaylistTracksTable +
            " where playlist_id in (select id from " +
            kRekordboxPlaylistsTable + " where device=:device)");
    deletePlaylistTracksQuery.bindValue(":device", deviceId);
    if (!deletePlaylistTracksQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistTracksQuery)
                << "deviceId:" << deviceId;
        return;
    }

    QSqlQuery deletePlaylistsQuery(database);
    deletePlaylistsQuery.prepare(
            "delete from " + kRekordboxPlaylistsTable +
            " where device=:device");
    deletePlaylistsQuery.bindValue(":device", deviceId);
    if (!deletePlaylistsQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistsQuery)
                << "deviceId:" << deviceId;
        return;
    }

    QSqlQuery deleteTracksQuery(database);
    deleteTracksQuery.prepare("delete from " + kRekordboxLibraryTable + " where device=:device");
    deleteTracksQuery.bindValue(":device", deviceId);

    if (!deleteTracksQuery.exec()) {
        LOG_FAILED_QUERY(deleteTracksQuery)
                << "deviceId:" << deviceId;
        return;
    }

    transaction.commit();
}

void setHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    const QList<CuePointer> cuePoints = track->getCuePointsForPlayback();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }

    if (endPosition.isValid() && endPosition <= startPosition) {
        qWarning() << "Ignoring invalid Rekordbox hot loop end at"
                   << endPosition << "for start" << startPosition;
        endPosition = mixxx::audio::kInvalidFramePos;
    }

    mixxx::CueType type = mixxx::CueType::HotCue;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    if (pCue) {
        pCue->setType(type);
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                id,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

void addMemoryCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    if (color) {
        pCue = track->createAndAddCue(
                mixxx::CueType::MemoryCue,
                Cue::kNoHotCue,
                startPosition,
                mixxx::audio::kInvalidFramePos,
                *color);
    } else {
        pCue = track->createAndAddCue(
                mixxx::CueType::MemoryCue,
                Cue::kNoHotCue,
                startPosition,
                mixxx::audio::kInvalidFramePos);
    }
    if (pCue) {
        pCue->setLabel(label);
    }
}

void readAnalyze(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        bool readBeatGrid,
        const QString& anlzPath) {
    if (!QFile(anlzPath).exists()) {
        return;
    }

    qDebug() << "Rekordbox ANLZ path:" << anlzPath << " for: " << track->getTitle();

    std::ifstream ifs(anlzPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

    const double sampleRateKhz = sampleRate / 1000.0;

    QList<memory_cue_t> memoryCues;

    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            if (!readBeatGrid) {
                break;
            }

            auto* beatGridTag =
                    static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(
                            section->body());

            QVector<mixxx::rekordbox::GridBeat> grid;
            int firstBeatNumber = 1;

            for (const auto& beat : *beatGridTag->beats()) {
                if (grid.isEmpty()) {
                    firstBeatNumber = static_cast<int>(beat->beat_number());
                }
                grid.append(mixxx::rekordbox::gridBeatFromExport(sampleRate,
                        static_cast<int>(beat->time()),
                        timingOffset,
                        static_cast<int>(beat->tempo())));
            }

            const auto pBeats = mixxx::rekordbox::beatsFromGrid(sampleRate,
                    grid,
                    firstBeatNumber,
                    mixxx::rekordboxconstants::beatsSubversion);
            if (pBeats) {
                track->trySetBeats(pBeats);
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES: {
            if (ignoreCues) {
                break;
            }

            auto* cuesTag =
                    static_cast<rekordbox_anlz_t::cue_tag_t*>(
                            section->body());

            for (const auto& cueEntry : *cuesTag->cues()) {
                int time = static_cast<int>(cueEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        addOrUpdateMemoryCue(&memoryCues,
                                position,
                                QString(),
                                mixxx::RgbColor::nullopt());
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP:
                        // Memory loops are deliberately excluded.
                        break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    if (cueEntry->hot_cue() == 0) {
                        break;
                    }
                    const int hotCueIndex =
                            static_cast<int>(cueEntry->hot_cue() - 1);
                    auto endPosition = mixxx::audio::kInvalidFramePos;
                    if (cueEntry->type() == rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP) {
                        const int endTime =
                                static_cast<int>(cueEntry->loop_time()) -
                                timingOffset;
                        if (endTime > time) {
                            endPosition = mixxx::audio::FramePos(
                                    sampleRateKhz *
                                    static_cast<double>(endTime));
                        }
                    }
                    setHotCue(
                            track,
                            position,
                            endPosition,
                            hotCueIndex,
                            QString(),
                            mixxx::RgbColor::nullopt());
                } break;
                }
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES_2: {
            if (ignoreCues) {
                break;
            }

            auto* cuesExtendedTag =
                    static_cast<rekordbox_anlz_t::cue_extended_tag_t*>(
                            section->body());

            for (const auto& cueExtendedEntry : *cuesExtendedTag->cues()) {
                int time = static_cast<int>(cueExtendedEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesExtendedTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueExtendedEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        addOrUpdateMemoryCue(&memoryCues,
                                position,
                                fromUtf16BeString(cueExtendedEntry->comment()),
                                cueColorFromID(static_cast<int>(
                                        cueExtendedEntry->color_id())));
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP:
                        // Memory loops are deliberately excluded.
                        break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    if (cueExtendedEntry->hot_cue() == 0) {
                        break;
                    }
                    const int hotCueIndex =
                            static_cast<int>(cueExtendedEntry->hot_cue() - 1);
                    auto endPosition = mixxx::audio::kInvalidFramePos;
                    if (cueExtendedEntry->type() ==
                            rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP) {
                        const int endTime =
                                static_cast<int>(
                                        cueExtendedEntry->loop_time()) -
                                timingOffset;
                        if (endTime > time) {
                            endPosition = mixxx::audio::FramePos(
                                    sampleRateKhz *
                                    static_cast<double>(endTime));
                        }
                    }
                    setHotCue(track,
                            position,
                            endPosition,
                            hotCueIndex,
                            fromUtf16BeString(cueExtendedEntry->comment()),
                            mixxx::RgbColor(qRgb(
                                    static_cast<int>(
                                            cueExtendedEntry->color_red()),
                                    static_cast<int>(
                                            cueExtendedEntry->color_green()),
                                    static_cast<int>(cueExtendedEntry
                                                    ->color_blue()))));
                } break;
                }
            }
        } break;
        default:
            break;
        }
    }

    if (ignoreCues) {
        return;
    }

    track->removeCuesOfType(mixxx::CueType::MemoryCue);

    if (!memoryCues.isEmpty()) {
        std::sort(memoryCues.begin(),
                memoryCues.end(),
                [](const memory_cue_t& a, const memory_cue_t& b) {
                    return a.startPosition < b.startPosition;
                });

        for (const auto& memoryCue : std::as_const(memoryCues)) {
            addMemoryCue(track,
                    memoryCue.startPosition,
                    memoryCue.comment,
                    memoryCue.color);
        }
    }
    qDebug() << "Imported" << memoryCues.size()
             << "Rekordbox memory cues for:" << track->getTitle();
}

} // anonymous namespace

RekordboxPlaylistModel::RekordboxPlaylistModel(QObject* parent,
        TrackCollectionManager* trackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(parent,
                  trackCollectionManager,
                  "mixxx.db.model.rekordbox.playlistmodel",
                  kRekordboxPlaylistsTable,
                  kRekordboxPlaylistTracksTable,
                  trackSource) {
}

void RekordboxPlaylistModel::initSortColumnMapping() {
    // Add a bijective mapping between the SortColumnIds and column indices
    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }

    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::DateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TimesPlayed)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::LastPlayedAt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Rating)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Color)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::CoverArt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::PlaylistDateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

TrackPointer RekordboxPlaylistModel::getTrack(const QModelIndex& index) const {
    qDebug() << "RekordboxTrackModel::getTrack";

    TrackPointer track = BaseExternalPlaylistModel::getTrack(index);
    QString location = getFieldVariant(
            index, ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION)
                               .toString();

    if (!QFile(location).exists()) {
        return track;
    }

    // Rekordbox cues are a transient playback session. Keep embedded Serato
    // and native Mixxx cues in the Track's persistent cue list, but exclude
    // them from playback controls and UI while this Rekordbox track is used.
    track->beginRekordboxCueSession();

    // The following code accounts for timing offsets required to
    // correctly align timing information (cue points, loops, beatgrids)
    // exported from Rekordbox. This is caused by different MP3
    // decoders treating MP3s encoded in a variety of different cases
    // differently. The mp3guessenc library is used to determine which
    // case the MP3 is classified in. See the following PR for more
    // detailed information:
    // https://github.com/mixxxdj/mixxx/pull/2119

    int timingOffset = 0;

    if (location.endsWith(".mp3", Qt::CaseInsensitive)) {
        int timingShiftCase = mp3guessenc_timing_shift_case(location.toStdString().c_str());

        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(".m4a")) {
        timingOffset = 48;
    }
#endif

    mixxx::audio::SampleRate sampleRate = track->getSampleRate();

    QString anlzPath =
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH)
                    .toString();
    QString anlzPathExt = anlzPath.left(anlzPath.length() - 3) + "EXT";

    const bool hasExtendedAnalyze = QFile(anlzPathExt).exists();
    // Always take the beatgrid from DAT. Without EXT, DAT also supplies cues.
    readAnalyze(track, sampleRate, timingOffset, hasExtendedAnalyze, true, anlzPath);
    if (hasExtendedAnalyze) {
        readAnalyze(track, sampleRate, timingOffset, false, false, anlzPathExt);
    }

    // Assume that the key of the file the has been analyzed in Recordbox is correct
    // and prevent the AnalyzerKey from re-analyzing.
    // Form 5.4.3 Key format depends on the preferences option:
    // Classic: Abm,B,Ebm,F#,Bbm,Db,Fm,Ab,…,F#m,A,Dbm,E
    // Alphanumeric (Camelot): 1A,1B,2A,2B,3A,3B,4A,4B,…,11A,11B,12A,12B
    // Not reckognized: 1m, 01A
    // Earlier versions allow any format
    // Decision: We normalize the KeyText here to not write garbage to the
    // file metadata and it is unlikely to loose extra info.
    track->setKeys(KeyFactory::makeBasicKeysNormalized(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString(),
            mixxx::track::io::key::USER));

    track->setColor(mixxx::RgbColor::fromQVariant(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_COLOR)));

    return track;
}

bool RekordboxPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool RekordboxPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}

RekordboxFeature::RekordboxFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig,
        std::unique_ptr<mixxx::removablemedia::IRemovableMediaService>
                pRemovableMediaService)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    if (!pRemovableMediaService) {
        pRemovableMediaService =
                mixxx::removablemedia::createRemovableMediaService();
    }
    m_pRemovableMediaService = std::move(pRemovableMediaService);
    QString tableName = kRekordboxLibraryTable;
    QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR,
            REKORDBOX_ANALYZE_PATH};

    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pRekordboxPlaylistModel = make_parented<RekordboxPlaylistModel>(
            this, pLibrary->trackCollectionManager(), m_trackSource);

    m_title = tr("Rekordbox");

    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    // Drop any leftover temporary Rekordbox database tables if they exist
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);

    // Create new temporary Rekordbox database tables
    createLibraryTable(database, kRekordboxLibraryTable);
    createPlaylistsTable(database, kRekordboxPlaylistsTable);
    createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);
    transaction.commit();

    connect(&m_devicesFutureWatcher,
            &QFutureWatcher<RekordboxDeviceScanResult>::finished,
            this,
            &RekordboxFeature::onRekordboxDevicesFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<RekordboxImportResult>::finished,
            this,
            &RekordboxFeature::onTracksFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<RekordboxImportResult>::finished,
            this,
            &RekordboxFeature::databaseImportStateChanged);
    connect(m_pRemovableMediaService.get(),
            &mixxx::removablemedia::IRemovableMediaService::devicesChanged,
            this,
            &RekordboxFeature::onRemovableMediaDevicesChanged);
    connect(m_pRemovableMediaService.get(),
            &mixxx::removablemedia::IRemovableMediaService::safeStopFinished,
            this,
            &RekordboxFeature::onSafeStopFinished);
    m_safeStopReferenceReleaseTimer.setInterval(
            kSafeStopReferenceReleaseCheckIntervalMillis);
    connect(&m_safeStopReferenceReleaseTimer,
            &QTimer::timeout,
            this,
            [this] {
                if (m_safeStopReferencesReleased &&
                        m_safeStopReferencesReleased()) {
                    m_safeStopReferenceReleaseTimer.stop();
                    m_safeStopReferencesReleased = {};
                    auto requestPlatformSafeStop =
                            std::move(m_pendingSafeStopPlatformRequest);
                    m_pendingSafeStopPlatformRequest = {};
                    if (requestPlatformSafeStop) {
                        requestPlatformSafeStop();
                    }
                    return;
                }
                if (!m_safeStopReferenceReleaseElapsed.hasExpired(
                            kSafeStopReferenceReleaseTimeoutMillis)) {
                    return;
                }

                m_safeStopReferenceReleaseTimer.stop();
                m_safeStopReferencesReleased = {};
                m_pendingSafeStopPlatformRequest = {};
                mixxx::removablemedia::SafeStopResult result;
                result.deviceId = m_safeStopOperationDeviceId;
                result.state = mixxx::removablemedia::MediaState::Failed;
                result.errorCode =
                        mixxx::removablemedia::ErrorCode::TimedOut;
                result.errorMessage = tr(
                        "Mixxx could not release all loaded tracks before "
                        "the safe-stop deadline.");
                onSafeStopFinished(result);
            });
    // initialize the model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    m_pRemovableMediaService->requestRescan();
}

RekordboxFeature::~RekordboxFeature() {
    m_devicesFuture.waitForFinished();
    m_tracksFuture.waitForFinished();

    // Drop temporary Rekordbox database tables on shutdown
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);
    transaction.commit();
}

void RekordboxFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    parented_ptr<WLibraryTextBrowser> pEdit = make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setHtml(formatRootViewHtml());
    pEdit->setOpenLinks(false);
    connect(pEdit, &WLibraryTextBrowser::anchorClicked, this, &RekordboxFeature::htmlLinkClicked);
    pLibraryWidget->registerView("REKORDBOXHOME", pEdit);
}

void RekordboxFeature::htmlLinkClicked(const QUrl& link) {
    if (QString(link.path()) == "refresh") {
        activate();
    } else {
        qDebug() << "Unknown link clicked" << link;
    }
}

std::unique_ptr<BaseSqlTableModel>
RekordboxFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    VERIFY_OR_DEBUG_ASSERT(data.canConvert<QVariantList>()) {
        return {};
    }
    QVariantList playlists = data.toList();
    VERIFY_OR_DEBUG_ASSERT(playlists.size() > 0) {
        return {};
    }
    auto pModel = std::make_unique<RekordboxPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setPlaylist(playlists.at(0).toString());
    return pModel;
}

QVariant RekordboxFeature::title() {
    return m_title;
}

bool RekordboxFeature::isSupported() {
    return true;
}

TreeItemModel* RekordboxFeature::sidebarModel() const {
    return m_pSidebarModel;
}

int RekordboxFeature::deviceCount() const {
    const TreeItem* pRoot = m_pSidebarModel->getRootItem();
    return pRoot ? pRoot->childRows() : 0;
}

bool RekordboxFeature::isDeviceScanRunning() const {
    return m_devicesFutureWatcher.isRunning() ||
            m_deviceScanApplyPending;
}

bool RekordboxFeature::isDatabaseImportRunning() const {
    return m_tracksFutureWatcher.isRunning() ||
            m_databaseImportApplyPending;
}

QString RekordboxFeature::deviceScanState() const {
    if (!isDeviceScanRunning()) {
        return QStringLiteral("idle");
    }
    return m_deviceScanPending
            ? QStringLiteral("pending")
            : QStringLiteral("scanning");
}

QList<mixxx::removablemedia::RemovableMediaDevice>
RekordboxFeature::mediaDevices() const {
    return m_mediaDevices;
}

void RekordboxFeature::scanDevices() {
    m_pRemovableMediaService->requestRescan();
    startDeviceScan();
}

void RekordboxFeature::startDeviceScan(bool replaceCompletedScan) {
    if (replaceCompletedScan) {
        VERIFY_OR_DEBUG_ASSERT(m_deviceScanApplyPending &&
                !m_devicesFutureWatcher.isRunning()) {
            return;
        }
    } else if (isDeviceScanRunning()) {
        m_deviceScanPending = true;
        emit deviceScanStateChanged();
        return;
    }
    // Own the result before publishing a scanning snapshot. Synchronous
    // observers of devicesChanged() must never see an idle admission fence
    // while this scan is already being prepared.
    m_deviceScanApplyPending = true;
    m_deviceScanPending = false;
    m_activeScanGeneration = m_pRemovableMediaService->generation();
    QList<mixxx::removablemedia::RemovableMediaDevice> snapshot =
            m_pRemovableMediaService->devices();
    for (auto& device : snapshot) {
        if (!m_safeStopRemovalGroupId.isEmpty() &&
                device.removalGroupId == m_safeStopRemovalGroupId) {
            m_loadBlockedDeviceIds.insert(device.deviceId);
            if (!m_safeStopBlockedDeviceIds.contains(device.deviceId)) {
                m_safeStopBlockedDeviceIds.append(device.deviceId);
            }
            for (const QString& mountPoint : device.mountPoints) {
                const QString root = QDir::cleanPath(mountPoint);
                if (!m_safeStopBlockedMountPoints.contains(root)) {
                    m_safeStopBlockedMountPoints.append(root);
                }
            }
        }
        if (m_loadBlockedDeviceIds.contains(device.deviceId)) {
            device.state = mixxx::removablemedia::MediaState::Quiescing;
        } else {
            device.state = mixxx::removablemedia::MediaState::Scanning;
        }
    }
    m_mediaDevices = snapshot;
    emit devicesChanged();
    m_devicesFuture = QtConcurrent::run(
            findRekordboxDevices,
            snapshot,
            m_activeScanGeneration,
            m_loadBlockedDeviceIds);
    m_devicesFutureWatcher.setFuture(m_devicesFuture);
    emit deviceScanStateChanged();
}

void RekordboxFeature::onRemovableMediaDevicesChanged() {
    if (!isDeviceScanRunning()) {
        // startDeviceScan() acquires the apply fence before it publishes the
        // service's new snapshot, closing the idle notification window.
        startDeviceScan();
        return;
    }
    m_mediaDevices = m_pRemovableMediaService->devices();
    for (auto& device : m_mediaDevices) {
        if (!m_safeStopRemovalGroupId.isEmpty() &&
                device.removalGroupId == m_safeStopRemovalGroupId) {
            m_loadBlockedDeviceIds.insert(device.deviceId);
            if (!m_safeStopBlockedDeviceIds.contains(device.deviceId)) {
                m_safeStopBlockedDeviceIds.append(device.deviceId);
            }
            for (const QString& mountPoint : device.mountPoints) {
                const QString root = QDir::cleanPath(mountPoint);
                if (!m_safeStopBlockedMountPoints.contains(root)) {
                    m_safeStopBlockedMountPoints.append(root);
                }
            }
        }
        device.state = m_loadBlockedDeviceIds.contains(device.deviceId)
                ? mixxx::removablemedia::MediaState::Quiescing
                : mixxx::removablemedia::MediaState::Scanning;
    }
    emit devicesChanged();
    startDeviceScan();
}

QString RekordboxFeature::deviceIdForLocation(const QString& location) const {
    for (const auto& device : m_mediaDevices) {
        for (const QString& mountPoint : device.mountPoints) {
            if (pathIsWithinMount(location, mountPoint)) {
                return device.deviceId;
            }
        }
    }
    return {};
}

bool RekordboxFeature::isLocationLoadAllowed(const QString& location) const {
    for (const QString& mountPoint : m_safeStopBlockedMountPoints) {
        if (pathIsWithinMount(location, mountPoint)) {
            return false;
        }
    }
    const QString deviceId = deviceIdForLocation(location);
    return deviceId.isEmpty() || !m_loadBlockedDeviceIds.contains(deviceId);
}

bool RekordboxFeature::isSafeStopActiveFor(
        const QString& deviceId) const {
    return !deviceId.isEmpty() &&
            deviceId == m_safeStopOperationDeviceId;
}

bool RekordboxFeature::requestSafeStop(
        const QString& deviceId,
        quint64 expectedGeneration,
        const std::function<void()>& releaseMediaReferences,
        const std::function<bool()>& mediaReferencesReleased) {
    if (!m_safeStopOperationDeviceId.isEmpty()) {
        if (deviceId == m_safeStopOperationDeviceId) {
            // The public API has no operation token. Treat a repeated request
            // for the accepted target as an idempotent join so a synthetic
            // same-ID Busy result cannot masquerade as its terminal result.
            return true;
        }
        rejectSafeStop(deviceId,
                tr("Another removable-media stop operation is in progress."));
        return false;
    }

    const auto deviceIt = std::find_if(m_mediaDevices.cbegin(),
            m_mediaDevices.cend(),
            [&deviceId](const auto& device) {
                return device.deviceId == deviceId;
            });
    if (deviceIt == m_mediaDevices.cend()) {
        mixxx::removablemedia::SafeStopResult result;
        result.deviceId = deviceId;
        result.state = mixxx::removablemedia::MediaState::Failed;
        result.errorCode = mixxx::removablemedia::ErrorCode::NotFound;
        result.errorMessage = tr("The selected removable device is no longer present.");
        onSafeStopFinished(result);
        return false;
    }
    if (deviceIt->generation != expectedGeneration) {
        mixxx::removablemedia::SafeStopResult result;
        result.deviceId = deviceId;
        result.state = mixxx::removablemedia::MediaState::Failed;
        result.errorCode = mixxx::removablemedia::ErrorCode::StaleGeneration;
        result.errorMessage = tr("The removable device list changed. Select the device again.");
        onSafeStopFinished(result);
        return false;
    }
    if (isDeviceScanRunning()) {
        rejectSafeStop(deviceId, tr("A removable-media scan is still running."));
        return false;
    }
    if (isDatabaseImportRunning()) {
        rejectSafeStop(deviceId, tr("The Rekordbox database is still being imported."));
        return false;
    }
    if (!deviceIt->canStop) {
        mixxx::removablemedia::SafeStopResult result;
        result.deviceId = deviceId;
        result.state = mixxx::removablemedia::MediaState::Failed;
        result.errorCode = mixxx::removablemedia::ErrorCode::Unsupported;
        result.errorMessage = tr("Safe removal is unavailable on this platform adapter.");
        onSafeStopFinished(result);
        return false;
    }

    m_safeStopOperationDeviceId = deviceId;
    m_safeStopRemovalGroupId = deviceIt->removalGroupId;
    m_safeStopBlockedDeviceIds.clear();
    m_safeStopBlockedMountPoints.clear();
    for (const auto& device : std::as_const(m_mediaDevices)) {
        if (device.removalGroupId == deviceIt->removalGroupId) {
            m_loadBlockedDeviceIds.insert(device.deviceId);
            m_safeStopBlockedDeviceIds.append(device.deviceId);
            for (const QString& mountPoint : device.mountPoints) {
                const QString root = QDir::cleanPath(mountPoint);
                if (!m_safeStopBlockedMountPoints.contains(root)) {
                    m_safeStopBlockedMountPoints.append(root);
                }
            }
        }
    }
    // Freeze the entire physical removal group before publishing any state.
    // A synchronous devicesChanged observer must never see one partition as
    // quiescing while a sibling partition can still pass LOAD admission.
    for (auto& device : m_mediaDevices) {
        if (device.removalGroupId == m_safeStopRemovalGroupId) {
            device.state = mixxx::removablemedia::MediaState::Quiescing;
            device.errorCode = mixxx::removablemedia::ErrorCode::None;
            device.errorMessage.clear();
            device.busyReason.clear();
        }
    }
    // The LOAD gates above must be held before releasing players, while the
    // platform request must not start until all Mixxx file handles have been
    // closed. The Library supplies these callbacks to stop and eject only
    // players that reference the selected physical removal group and then
    // verify their asynchronous readers have acknowledged TRACK_UNLOADED.
    if (releaseMediaReferences) {
        releaseMediaReferences();
    }
    emit devicesChanged();
    auto requestPlatformSafeStop = [this, deviceId, expectedGeneration] {
        return m_pRemovableMediaService->requestSafeStop(
                deviceId, expectedGeneration);
    };
    if (!mediaReferencesReleased || mediaReferencesReleased()) {
        return requestPlatformSafeStop();
    }

    m_safeStopReferencesReleased = mediaReferencesReleased;
    m_pendingSafeStopPlatformRequest =
            std::move(requestPlatformSafeStop);
    m_safeStopReferenceReleaseElapsed.start();
    m_safeStopReferenceReleaseTimer.start();
    return true;
}

void RekordboxFeature::rejectSafeStop(
        const QString& deviceId,
        const QString& busyReason) {
    if (isSafeStopActiveFor(deviceId)) {
        return;
    }
    mixxx::removablemedia::SafeStopResult result;
    result.deviceId = deviceId;
    result.state = mixxx::removablemedia::MediaState::Busy;
    result.errorCode = mixxx::removablemedia::ErrorCode::Busy;
    result.errorMessage = busyReason;
    if (!m_safeStopOperationDeviceId.isEmpty()) {
        // A concurrent rejection is a terminal result only for the new
        // request. It must never release or overwrite the accepted
        // operation's removal-group LOAD gates.
        emit safeStopFinished(result);
        return;
    }
    onSafeStopFinished(result);
}

void RekordboxFeature::setMediaDeviceState(
        const QString& deviceId,
        mixxx::removablemedia::MediaState state,
        mixxx::removablemedia::ErrorCode errorCode,
        const QString& errorMessage,
        const QString& busyReason) {
    for (auto& device : m_mediaDevices) {
        if (device.deviceId != deviceId) {
            continue;
        }
        device.state = state;
        device.errorCode = errorCode;
        device.errorMessage = errorMessage;
        device.busyReason = busyReason;
        emit devicesChanged();
        return;
    }
}

void RekordboxFeature::onSafeStopFinished(
        const mixxx::removablemedia::SafeStopResult& result) {
    QStringList terminalDeviceIds;
    const bool completedAcceptedOperation =
            result.deviceId == m_safeStopOperationDeviceId;
    if (completedAcceptedOperation) {
        terminalDeviceIds = m_safeStopBlockedDeviceIds;
    }
    if (terminalDeviceIds.isEmpty()) {
        terminalDeviceIds.append(result.deviceId);
    }
    for (const QString& deviceId : std::as_const(terminalDeviceIds)) {
        setMediaDeviceState(deviceId,
                result.state,
                result.errorCode,
                result.errorMessage,
                result.state == mixxx::removablemedia::MediaState::Busy
                        ? result.errorMessage
                        : QString());
    }
    emit safeStopFinished(result);
    if (completedAcceptedOperation) {
        m_safeStopReferenceReleaseTimer.stop();
        m_safeStopReferencesReleased = {};
        m_pendingSafeStopPlatformRequest = {};
        // Keep every LOAD/STOP gate and the operation correlation token live
        // through all terminal model and result notifications. Reentrant
        // observers must not be able to start a new load or physical stop
        // before the matching terminal result has actually been published.
        for (const QString& deviceId :
                std::as_const(m_safeStopBlockedDeviceIds)) {
            m_loadBlockedDeviceIds.remove(deviceId);
        }
        m_safeStopBlockedDeviceIds.clear();
        m_safeStopBlockedMountPoints.clear();
        m_safeStopRemovalGroupId.clear();
        m_safeStopOperationDeviceId.clear();
        // Rescan only after the platform operation and authoritative
        // reconciliation reached a terminal state. While the group was
        // quiescing, worker scans explicitly skipped every blocked device.
        startDeviceScan();
    }
}

QString RekordboxFeature::formatRootViewHtml() const {
    QString title = tr("Rekordbox");
    QString summary = tr(
            "Reads databases exported for Pioneer CDJ / XDJ players using "
            "the Rekordbox Export mode.<br/>"
            "Rekordbox can only export to USB or SD devices with a FAT or "
            "HFS file system.<br/>"
            "Mixxx can read a database from any device that contains the "
            "database folders (<tt>PIONEER</tt> and <tt>Contents</tt>).<br/>"
            "Not supported are Rekordbox databases that have been moved to "
            "an external device via<br/>"
            "<i>Preferences > Advanced > Database management</i>.<br/>"
            "<br/>"
            "The following data is read:");

    QStringList items;

    items
            << tr("Folders")
            << tr("Playlists")
            << tr("Beatgrids")
            << tr("Hot cues")
            << tr("Memory cues")
            << tr("Loops (only the first loop is currently usable in Mixxx)");

    QString html;
    QString refreshLink = tr("Check for attached Rekordbox USB / SD devices (refresh)");
    html.append(QString("<h2>%1</h2>").arg(title));
    html.append(QString("<p>%1</p>").arg(summary));
    html.append(QString("<ul>"));
    for (const auto& item : std::as_const(items)) {
        html.append(QString("<li>%1</li>").arg(item));
    }
    html.append(QString("</ul>"));

    //Colorize links in lighter blue, instead of QT default dark blue.
    //Links are still different from regular text, but readable on dark/light backgrounds.
    //https://github.com/mixxxdj/mixxx/issues/9103
    html.append(QString("<a style=\"color:#0496FF;\" href=\"refresh\">%1</a>")
                        .arg(refreshLink));
    return html;
}

void RekordboxFeature::refreshLibraryModels() {
}

void RekordboxFeature::activate() {
    qDebug() << "RekordboxFeature::activate()";

    // Let a worker thread do the XML parsing
    scanDevices();
    m_title = tr("(loading) Rekordbox");
    //calls a slot in the sidebar model such that 'Rekordbox (isLoading)' is displayed.
    m_featureLoadingRequested = true;
    emit featureIsLoading(this, true);

    emit enableCoverArtDisplay(true);
    emit switchToView("REKORDBOXHOME");
    emit disableSearch();
}

void RekordboxFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    //access underlying TreeItem object
    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!(item && item->getData().isValid())) {
        return;
    }

    if (isDatabaseImportRunning()) {
        qDebug() << "Ignoring Rekordbox source activation while database import is running";
        return;
    }

    // TreeItem list data holds 2 values in a QList and have different meanings.
    // If the 2nd QList element IS_RECORDBOX_DEVICE, the 1st element is the
    // filesystem device path, and the parseDeviceDB concurrent thread to parse
    // the Rekcordbox database is initiated. If the 2nd element is
    // IS_NOT_RECORDBOX_DEVICE, the 1st element is the playlist path and it is
    // activated.
    QList<QVariant> data = item->getData().toList();
    if (data.size() < 2) {
        return;
    }
    QString playlist = data[0].toString();
    bool doParseDeviceDB = data[1].toString() == IS_RECORDBOX_DEVICE;

    TreeItem* deviceItem = item;
    while (deviceItem->parent() && !deviceItem->parent()->isRoot()) {
        deviceItem = deviceItem->parent();
    }
    const QString deviceId = treeItemDeviceId(deviceItem);
    const QVariantList deviceData = deviceItem->getData().toList();
    const quint64 generation = deviceData.size() > 3
            ? deviceData.at(3).toULongLong()
            : 0;
    if (!deviceId.isEmpty() && m_loadBlockedDeviceIds.contains(deviceId)) {
        qWarning() << "Ignoring load from quiescing removable media" << deviceId;
        return;
    }

    const QString deviceLabel = item->getLabel();
    qDebug() << "RekordboxFeature::activateChild " << deviceLabel
             << " playlist: " << playlist << " doParseDeviceDB: " << doParseDeviceDB;

    if (doParseDeviceDB) {
        qDebug() << "Parse Rekordbox Device DB: " << playlist;

        m_activeImportDeviceId = deviceId;
        m_activeImportGeneration = generation;
        // Keep import ownership through the queued result-application slot.
        m_databaseImportApplyPending = true;
        // Let a worker thread do the XML parsing
        m_tracksFuture = QtConcurrent::run(
                parseDeviceDB,
                static_cast<Library*>(parent())->dbConnectionPool(),
                deviceLabel,
                playlist,
                deviceId,
                generation);
        m_tracksFutureWatcher.setFuture(m_tracksFuture);
        emit databaseImportStateChanged();

        // This device is now a playlist element, future activations should treat is
        // as such
        data[1] = QVariant(IS_NOT_RECORDBOX_DEVICE);
        item->setData(QVariant(data));
    } else {
        qDebug() << "Activate Rekordbox Playlist: " << playlist;
        m_activeDeviceId = deviceId;
        m_pRekordboxPlaylistModel->setPlaylist(playlist);
        emit showTrackModel(m_pRekordboxPlaylistModel);
    }
}

void RekordboxFeature::onRekordboxDevicesFound() {
    const RekordboxDeviceScanResult result = m_devicesFuture.result();
    auto foundDevices = std::vector<std::unique_ptr<TreeItem>>(
            result.treeItems.cbegin(), result.treeItems.cend());

    const bool staleResult =
            result.generation != m_pRemovableMediaService->generation();
    const bool rerunScan = m_deviceScanPending || staleResult;
    m_deviceScanPending = false;
    if (staleResult) {
        qDebug() << "Discarding stale Rekordbox media scan generation"
                 << result.generation << "current"
                 << m_pRemovableMediaService->generation();
        if (rerunScan) {
            // Transfer ownership directly to the replacement generation. A
            // synchronous signal observer must never see an idle admission
            // window between a completed scan and its pending successor.
            startDeviceScan(true);
        } else {
            m_deviceScanApplyPending = false;
            emit deviceScanStateChanged();
        }
        return;
    }

    clearLastRightClickedIndex();

    TreeItem* root = m_pSidebarModel->getRootItem();
    QSqlDatabase database = m_pTrackCollection->database();

    QSet<QString> foundDeviceIds;
    for (const auto& pDeviceFound : foundDevices) {
        foundDeviceIds.insert(treeItemDeviceId(pDeviceFound.get()));
    }
    // Keep existing source nodes inert while STOP owns their LOAD gates.
    // A terminal scan removes them after success or refreshes them after
    // failure without reading the group during quiesce.
    foundDeviceIds.unite(m_loadBlockedDeviceIds);

    // Remove backwards so adjacent removals cannot skip an item.
    for (int deviceIndex = root->childRows() - 1; deviceIndex >= 0; --deviceIndex) {
        TreeItem* child = root->child(deviceIndex);
        const QString deviceId = treeItemDeviceId(child);
        if (foundDeviceIds.contains(deviceId)) {
            continue;
        }
        clearDeviceTables(database, child);
        const bool removedActiveDevice = deviceId == m_activeDeviceId;
        m_pSidebarModel->removeRows(deviceIndex, 1);
        m_loadBlockedDeviceIds.remove(deviceId);
        if (removedActiveDevice) {
            m_activeDeviceId.clear();
            m_pRekordboxPlaylistModel->setPlaylist(QString());
            emit showTrackModel(nullptr);
            emit activeDeviceRemoved(deviceId);
        }
    }

    std::vector<std::unique_ptr<TreeItem>> childrenToAdd;
    for (auto&& pDeviceFound : foundDevices) {
        const QString deviceId = treeItemDeviceId(pDeviceFound.get());
        TreeItem* existing = nullptr;
        int existingRow = -1;
        for (int deviceIndex = 0; deviceIndex < root->childRows(); ++deviceIndex) {
            TreeItem* child = root->child(deviceIndex);
            if (treeItemDeviceId(child) == deviceId) {
                existing = child;
                existingRow = deviceIndex;
                break;
            }
        }

        if (!existing) {
            childrenToAdd.push_back(std::move(pDeviceFound));
            continue;
        }

        if (treeItemDevicePath(existing) !=
                treeItemDevicePath(pDeviceFound.get())) {
            const bool migratedActiveDevice =
                    deviceId == m_activeDeviceId;
            clearDeviceTables(database, existing);
            m_pSidebarModel->removeRows(existingRow, 1);
            childrenToAdd.push_back(std::move(pDeviceFound));
            if (migratedActiveDevice) {
                m_activeDeviceId.clear();
                m_pRekordboxPlaylistModel->setPlaylist(QString());
                emit showTrackModel(nullptr);
                emit activeDeviceRemoved(deviceId);
            }
            continue;
        }

        QVariantList refreshedData = pDeviceFound->getData().toList();
        const QVariantList existingData = existing->getData().toList();
        if (refreshedData.size() > 1 && existingData.size() > 1) {
            // Preserve whether this still-unmoved device has already been
            // imported while updating its generation fence.
            refreshedData[1] = existingData.at(1);
        }
        existing->setData(refreshedData);
        if (!existing->hasChildren()) {
            existing->setLabel(pDeviceFound->getLabel());
        }
    }

    if (!childrenToAdd.empty()) {
        m_pSidebarModel->insertTreeItemRows(std::move(childrenToAdd), 0);
    }
    m_pSidebarModel->triggerRepaint();

    m_mediaDevices = result.mediaDevices;
    for (auto& device : m_mediaDevices) {
        if (m_loadBlockedDeviceIds.contains(device.deviceId)) {
            device.state = mixxx::removablemedia::MediaState::Quiescing;
        }
    }
    emit devicesChanged();

    // Calls a slot in the sidebar model such that isLoading is removed from
    // the feature title. A QML status-only scan does not set feature loading.
    if (rerunScan) {
        // Keep the apply fence continuously owned while this result is
        // replaced by the pending generation. startDeviceScan(true) publishes
        // the next scanning state without an observable idle transition.
        startDeviceScan(true);
    } else {
        m_deviceScanApplyPending = false;
        emit deviceScanStateChanged();
        m_title = tr("Rekordbox");
        if (m_featureLoadingRequested) {
            m_featureLoadingRequested = false;
            emit featureLoadingFinished(this);
        }
    }
}

void RekordboxFeature::makeDeviceImportRetryable(
        const QString& deviceId,
        const QString& devicePath,
        const QString& deviceLabel) {
    if (deviceId.isEmpty()) {
        return;
    }

    // The worker may have committed before its generation became stale.
    // Always remove rows by opaque device identity before allowing a retry.
    TreeItem staleDevice(deviceLabel,
            QVariantList{devicePath,
                    QStringLiteral(IS_RECORDBOX_DEVICE),
                    deviceId});
    QSqlDatabase database = m_pTrackCollection->database();
    clearDeviceTables(database, &staleDevice);

    TreeItem* root = m_pSidebarModel->getRootItem();
    if (!root) {
        return;
    }
    for (int deviceIndex = 0; deviceIndex < root->childRows(); ++deviceIndex) {
        TreeItem* candidate = root->child(deviceIndex);
        if (treeItemDeviceId(candidate) != deviceId) {
            continue;
        }
        QVariantList currentData = candidate->getData().toList();
        if (currentData.size() > 1) {
            // Preserve the current path and generation. A scan may have
            // refreshed them while this import was running.
            currentData[1] = QStringLiteral(IS_RECORDBOX_DEVICE);
            candidate->setData(currentData);
            m_pSidebarModel->triggerRepaint();
        }
        return;
    }
}

void RekordboxFeature::onTracksFound() {
    qDebug() << "onTracksFound";

    RekordboxImportResult importResult;
    try {
        importResult = m_tracksFuture.result();
    } catch (const std::exception& e) {
        qWarning() << "Failed to load Rekordbox database:" << e.what();
        makeDeviceImportRetryable(m_activeImportDeviceId);
        setMediaDeviceState(m_activeImportDeviceId,
                mixxx::removablemedia::MediaState::Failed,
                mixxx::removablemedia::ErrorCode::InvalidDatabase,
                tr("The Rekordbox database import failed."));
        m_activeImportDeviceId.clear();
        m_activeImportGeneration = 0;
        m_databaseImportApplyPending = false;
        return;
    }

    const bool deviceStillPresent = std::any_of(m_mediaDevices.cbegin(),
            m_mediaDevices.cend(),
            [&importResult](const auto& device) {
                return device.deviceId == importResult.deviceId &&
                        device.generation == importResult.generation;
            });
    if (importResult.deviceId != m_activeImportDeviceId ||
            importResult.generation != m_activeImportGeneration ||
            importResult.generation != m_pRemovableMediaService->generation() ||
            !deviceStillPresent) {
        qWarning() << "Discarding stale Rekordbox import for"
                   << importResult.deviceId << "generation"
                   << importResult.generation;
        makeDeviceImportRetryable(
                importResult.deviceId.isEmpty()
                        ? m_activeImportDeviceId
                        : importResult.deviceId,
                importResult.devicePath,
                importResult.deviceLabel);
        m_activeImportDeviceId.clear();
        m_activeImportGeneration = 0;
        m_databaseImportApplyPending = false;
        return;
    }

    if (!importResult.succeeded) {
        makeDeviceImportRetryable(importResult.deviceId,
                importResult.devicePath,
                importResult.deviceLabel);
        setMediaDeviceState(importResult.deviceId,
                mixxx::removablemedia::MediaState::Failed,
                mixxx::removablemedia::ErrorCode::InvalidDatabase,
                tr("The Rekordbox database import failed."));
        m_activeImportDeviceId.clear();
        m_activeImportGeneration = 0;
        m_databaseImportApplyPending = false;
        return;
    }

    const QString& devicePlaylist = importResult.devicePath;
    TreeItem* deviceItem = nullptr;
    TreeItem* root = m_pSidebarModel->getRootItem();
    for (int deviceIndex = 0; deviceIndex < root->childRows(); ++deviceIndex) {
        TreeItem* candidate = root->child(deviceIndex);
        if (treeItemDeviceId(candidate) == importResult.deviceId) {
            deviceItem = candidate;
            break;
        }
    }

    if (deviceItem) {
        QList<TreeItem*> insertedItems;
        insertedItems.reserve(importResult.playlistTree.size());
        for (const auto& entry : importResult.playlistTree) {
            TreeItem* parentItem = deviceItem;
            if (entry.parentIndex >= 0) {
                if (entry.parentIndex >= insertedItems.size()) {
                    qWarning() << "Skipping Rekordbox playlist with invalid parent index"
                               << entry.parentIndex << entry.path;
                    insertedItems.append(nullptr);
                    continue;
                }
                parentItem = insertedItems.at(entry.parentIndex);
            }

            TreeItem* insertedItem = parentItem
                    ? parentItem->appendChild(
                              entry.label,
                              QVariant(QList<QString>{
                                      entry.path,
                                      IS_NOT_RECORDBOX_DEVICE}))
                    : nullptr;
            insertedItems.append(insertedItem);
        }
        m_pSidebarModel->triggerRepaint();
    }

    qDebug() << "Show Rekordbox Device Playlist: " << devicePlaylist;

    m_activeDeviceId = importResult.deviceId;
    m_pRekordboxPlaylistModel->setPlaylist(devicePlaylist);
    emit showTrackModel(m_pRekordboxPlaylistModel);
    m_activeImportDeviceId.clear();
    m_activeImportGeneration = 0;
    m_databaseImportApplyPending = false;
}
