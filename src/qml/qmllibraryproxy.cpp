#include "qml/qmllibraryproxy.h"

#include <QAbstractItemModel>

#include "library/library.h"
#include "library/librarytablemodel.h"
#include "library/sidebarmodel.h"
#include "library/trackmodel.h"
#include "moc_qmllibraryproxy.cpp"

namespace mixxx {
namespace qml {

QmlLibraryProxy::QmlLibraryProxy(std::shared_ptr<Library> pLibrary, QObject* parent)
        : QObject(parent),
          m_pLibrary(pLibrary),
          m_pSourcesProperty(new QmlLibrarySourceModel(m_pLibrary->sidebarModel(), this)),
          m_pModelProperty(new QmlLibraryTrackListModel(m_pLibrary->trackTableModel(), this)),
          m_pMediaDevicesProperty(new QmlRemovableMediaModel(m_pLibrary.get(), this)) {
    connect(m_pLibrary.get(),
            &Library::showTrackModel,
            this,
            [this](QAbstractItemModel* pModel) {
                if (m_pModelProperty->setTrackModel(pModel)) {
                    m_sourceActivationProvidedModel = true;
                    emit modelChanged();
                }
            });
    connect(m_pLibrary.get(),
            &Library::rekordboxMediaStatusChanged,
            this,
            &QmlLibraryProxy::mediaStatusChanged);
    connect(m_pLibrary.get(),
            &Library::removableMediaDevicesChanged,
            this,
            &QmlLibraryProxy::mediaStatusChanged);
    connect(m_pLibrary.get(),
            &Library::removableMediaSafeStopFinished,
            this,
            [this](const mixxx::removablemedia::SafeStopResult& result) {
                emit mediaSafeStopFinished(result.deviceId,
                        mixxx::removablemedia::mediaStateToString(result.state),
                        mixxx::removablemedia::errorCodeToString(result.errorCode),
                        result.errorMessage);
            });
    connect(m_pLibrary.get(),
            &Library::removableMediaActiveDeviceRemoved,
            this,
            &QmlLibraryProxy::mediaActiveDeviceRemoved);
    m_pLibrary->scanRekordboxDevices();
}

bool QmlLibraryProxy::mediaReady() const {
    return m_pLibrary->rekordboxDeviceCount() > 0;
}

bool QmlLibraryProxy::mediaScanInProgress() const {
    return m_pLibrary->isRekordboxDeviceScanRunning();
}

bool QmlLibraryProxy::mediaImportInProgress() const {
    return m_pLibrary->isRekordboxDatabaseImportRunning();
}

int QmlLibraryProxy::mediaDeviceCount() const {
    return m_pMediaDevicesProperty->rowCount();
}

QString QmlLibraryProxy::mediaScanState() const {
    return m_pLibrary->rekordboxDeviceScanState();
}

void QmlLibraryProxy::requestMediaRescan() {
    m_pLibrary->scanRekordboxDevices();
}

bool QmlLibraryProxy::requestSafeStop(
        const QString& deviceId,
        qulonglong expectedGeneration) {
    if (deviceId.isEmpty()) {
        return false;
    }
    return m_pLibrary->requestRemovableMediaSafeStop(
            deviceId, expectedGeneration);
}

QModelIndex QmlLibraryProxy::sourceIndexForDevice(
        const QString& deviceId) const {
    return m_pSourcesProperty->sourceIndexForDevice(deviceId);
}

bool QmlLibraryProxy::activateSource(const QModelIndex& sourceIndex) {
    QModelIndex sidebarIndex;
    if (sourceIndex.model() == m_pSourcesProperty) {
        sidebarIndex = m_pSourcesProperty->mapToSidebar(sourceIndex);
    } else if (sourceIndex.model() == m_pLibrary->sidebarModel()) {
        sidebarIndex = sourceIndex;
    }
    if (!sidebarIndex.isValid()) {
        return false;
    }
    m_pLibrary->sidebarModel()->doubleClicked(sidebarIndex);
    const bool hasChildren = m_pLibrary->sidebarModel()->hasChildren(sidebarIndex);
    m_sourceActivationProvidedModel = false;
    m_pLibrary->sidebarModel()->clicked(sidebarIndex);
    if (!m_sourceActivationProvidedModel &&
            m_pModelProperty->setTrackModel(nullptr)) {
        emit modelChanged();
    }
    return !hasChildren;
}

QModelIndex QmlLibraryProxy::parentSource(const QModelIndex& sourceIndex) const {
    if (sourceIndex.model() != m_pSourcesProperty) {
        return {};
    }
    return sourceIndex.parent();
}

bool QmlLibraryProxy::loadTrackToPlayer(
        int row, const QString& group, bool play) {
    if (row < 0 || group.isEmpty()) {
        return false;
    }

    const QModelIndex proxyIndex = m_pModelProperty->index(row, 0);
    if (!proxyIndex.isValid()) {
        return false;
    }

    auto* pTrackModel = dynamic_cast<TrackModel*>(m_pModelProperty->sourceModel());
    VERIFY_OR_DEBUG_ASSERT(pTrackModel) {
        return false;
    }

    const QModelIndex sourceIndex = m_pModelProperty->mapToSource(proxyIndex);
    VERIFY_OR_DEBUG_ASSERT(sourceIndex.isValid()) {
        return false;
    }

    const QString location = pTrackModel->getTrackLocation(sourceIndex);
    if (!m_pLibrary->isRemovableMediaLocationLoadAllowed(location)) {
        qWarning() << "Blocked QML LOAD before reading quiescing media"
                   << location << group;
        return false;
    }

    // Loading through TrackModel::getTrack() is essential for external
    // libraries. RekordboxPlaylistModel overrides it to parse the track's
    // ANLZ beat grid, Hot Cues and Memory Cues before the deck receives the
    // TrackPointer. Loading only by file URL bypasses that import step.
    const TrackPointer pTrack = pTrackModel->getTrack(sourceIndex);
    if (!pTrack) {
        return false;
    }
    m_pLibrary->slotLoadTrackToPlayer(pTrack, group, play);
    return true;
}

// static
QmlLibraryProxy* QmlLibraryProxy::create(QQmlEngine* pQmlEngine, QJSEngine* pJsEngine) {
    // The implementation of this method is mostly taken from the code example
    // that shows the replacement for `qmlRegisterSingletonInstance()` when
    // using `QML_SINGLETON`.
    // https://doc.qt.io/qt-6/qqmlengine.html#QML_SINGLETON

    // The instance has to exist before it is used. We cannot replace it.
    VERIFY_OR_DEBUG_ASSERT(s_pLibrary) {
        qWarning() << "Library hasn't been registered yet";
        return nullptr;
    }
    return new QmlLibraryProxy(s_pLibrary, pQmlEngine);
}

} // namespace qml
} // namespace mixxx
