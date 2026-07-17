#include "qml/qmllibrarytracklistmodel.h"

#include "library/trackmodel.h"
#include "moc_qmllibrarytracklistmodel.cpp"
#include "util/assert.h"
#include "util/duration.h"

namespace mixxx {
namespace qml {
namespace {
const QHash<int, QByteArray> kRoleNames = {
        {QmlLibraryTrackListModel::TitleRole, "title"},
        {QmlLibraryTrackListModel::ArtistRole, "artist"},
        {QmlLibraryTrackListModel::AlbumRole, "album"},
        {QmlLibraryTrackListModel::AlbumArtistRole, "albumArtist"},
        {QmlLibraryTrackListModel::BpmRole, "bpm"},
        {QmlLibraryTrackListModel::KeyRole, "key"},
        {QmlLibraryTrackListModel::DurationRole, "duration"},
        {QmlLibraryTrackListModel::FileUrlRole, "fileUrl"},
};

QVariant numericModelData(
        const QAbstractItemModel* pModel,
        const QModelIndex& index,
        bool convertDuration) {
    // SQL-backed models expose raw values through EditRole, while the file
    // browser stores them in UserRole. DisplayRole is intentionally last
    // because duration and BPM may already be localized/formatted there.
    for (const int role : {Qt::EditRole, Qt::UserRole, Qt::DisplayRole}) {
        const QVariant value = pModel->data(index, role);
        if (convertDuration && value.canConvert<mixxx::Duration>()) {
            return value.value<mixxx::Duration>().toDoubleSeconds();
        }
        bool ok = false;
        const double numericValue = value.toDouble(&ok);
        if (ok) {
            return numericValue;
        }
    }
    return {};
}
} // namespace

QmlLibraryTrackListModel::QmlLibraryTrackListModel(
        QAbstractItemModel* pModel,
        QObject* pParent)
        : QIdentityProxyModel(pParent),
          m_pTrackModel(nullptr) {
    setTrackModel(pModel);
}

bool QmlLibraryTrackListModel::setTrackModel(QAbstractItemModel* pModel) {
    auto* pTrackModel = dynamic_cast<TrackModel*>(pModel);
    VERIFY_OR_DEBUG_ASSERT(!pModel || pTrackModel) {
        return false;
    }

    m_pTrackModel = pTrackModel;
    if (pTrackModel) {
        pTrackModel->select();
    }
    if (sourceModel() != pModel) {
        setSourceModel(pModel);
    }
    return true;
}

QVariant QmlLibraryTrackListModel::data(const QModelIndex& proxyIndex, int role) const {
    if (!proxyIndex.isValid()) {
        return {};
    }

    VERIFY_OR_DEBUG_ASSERT(checkIndex(proxyIndex)) {
        return {};
    }

    const auto* pSourceModel = sourceModel();
    VERIFY_OR_DEBUG_ASSERT(pSourceModel && m_pTrackModel) {
        return {};
    }

    if (proxyIndex.column() > 0) {
        return {};
    }

    int column = -1;
    switch (role) {
    case TitleRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Title);
        break;
    case ArtistRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Artist);
        break;
    case AlbumRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Album);
        break;
    case AlbumArtistRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(
                TrackModel::SortColumnId::AlbumArtist);
        break;
    case BpmRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Bpm);
        break;
    case KeyRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key);
        break;
    case DurationRole:
        column = m_pTrackModel->columnIndexFromSortColumnId(TrackModel::SortColumnId::Duration);
        break;
    case FileUrlRole:
        return m_pTrackModel->getTrackUrl(mapToSource(proxyIndex));
    default:
        break;
    }

    if (column < 0) {
        return {};
    }

    const QModelIndex sourceIndex = mapToSource(proxyIndex);
    const QModelIndex valueIndex = pSourceModel->index(
            sourceIndex.row(), column, sourceIndex.parent());
    if (role == BpmRole) {
        return numericModelData(pSourceModel, valueIndex, false);
    }
    if (role == DurationRole) {
        return numericModelData(pSourceModel, valueIndex, true);
    }
    return pSourceModel->data(valueIndex, Qt::DisplayRole);
}

int QmlLibraryTrackListModel::columnCount(const QModelIndex& parent) const {
    // This is a list model, i.e. no entries have a parent.
    VERIFY_OR_DEBUG_ASSERT(!parent.isValid()) {
        return 0;
    }

    // There is exactly one column. All data is exposed as roles.
    return 1;
}

QHash<int, QByteArray> QmlLibraryTrackListModel::roleNames() const {
    return kRoleNames;
}

QVariant QmlLibraryTrackListModel::get(int row) const {
    QModelIndex idx = index(row, 0);
    QVariantMap dataMap;
    for (auto it = kRoleNames.constBegin(); it != kRoleNames.constEnd(); it++) {
        dataMap.insert(it.value(), data(idx, it.key()));
    }
    return dataMap;
}

} // namespace qml
} // namespace mixxx
