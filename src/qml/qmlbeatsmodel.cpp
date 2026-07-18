#include "qml/qmlbeatsmodel.h"

#include <QModelIndex>

namespace mixxx {
namespace qml {
namespace {
const QHash<int, QByteArray> kRoleNames = {
        {QmlBeatsModel::FramePositionRole, "framePosition"},
        {QmlBeatsModel::IsDownbeatRole, "isDownbeat"},
        {QmlBeatsModel::BeatNumberRole, "beatNumber"},
};
}

QmlBeatsModel::QmlBeatsModel(
        QObject* parent)
        : QAbstractListModel(parent),
          m_pBeats(nullptr),
          m_numBeats(0),
          m_firstBeatOffset(0) {
}

void QmlBeatsModel::setBeats(const BeatsPointer pBeats, audio::FramePos trackEndPosition) {
    beginResetModel();
    m_numBeats = 0;
    m_firstBeatOffset = 0;
    m_pBeats = pBeats;
    if (pBeats != nullptr) {
        // For constant grids cfirstmarker() is the persisted first-downbeat
        // anchor. Beat maps do not carry separate bar metadata, so their first
        // marker is the stable, beatgrid-adjustable fallback phase anchor.
        m_firstBeatOffset = pBeats->iteratorFrom(audio::kStartFramePos) -
                pBeats->cfirstmarker();
        m_numBeats = pBeats->numBeatsInRange(audio::kStartFramePos, trackEndPosition);
    }
    endResetModel();
}

QVariant QmlBeatsModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= m_numBeats) {
        return QVariant();
    }

    const BeatsPointer pBeats = m_pBeats;
    if (pBeats == nullptr) {
        return QVariant();
    }

    auto it = pBeats->iteratorFrom(audio::kStartFramePos) + index.row();
    VERIFY_OR_DEBUG_ASSERT(it != pBeats->cend()) {
        return QVariant();
    }

    switch (role) {
    case QmlBeatsModel::FramePositionRole:
        return (*it).value();
    case QmlBeatsModel::BeatNumberRole:
    case QmlBeatsModel::IsDownbeatRole: {
        constexpr int kBeatsPerBar = 4;
        const int beatOffset = m_firstBeatOffset + index.row();
        const int beatNumber =
                ((beatOffset % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar + 1;
        return role == QmlBeatsModel::BeatNumberRole
                ? QVariant(beatNumber)
                : QVariant(beatNumber == 1);
    }
    default:
        return QVariant();
    }
}

int QmlBeatsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }

    return m_numBeats;
}

QHash<int, QByteArray> QmlBeatsModel::roleNames() const {
    return kRoleNames;
}

QVariant QmlBeatsModel::get(int row) const {
    QModelIndex idx = index(row, 0);
    QVariantMap dataMap;
    for (auto it = kRoleNames.constBegin(); it != kRoleNames.constEnd(); it++) {
        dataMap.insert(it.value(), data(idx, it.key()));
    }
    return dataMap;
}

} // namespace qml
} // namespace mixxx
