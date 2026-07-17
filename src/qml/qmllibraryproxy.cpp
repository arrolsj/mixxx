#include "qml/qmllibraryproxy.h"

#include <QAbstractItemModel>

#include "library/library.h"
#include "library/librarytablemodel.h"
#include "library/sidebarmodel.h"
#include "moc_qmllibraryproxy.cpp"

namespace mixxx {
namespace qml {

QmlLibraryProxy::QmlLibraryProxy(std::shared_ptr<Library> pLibrary, QObject* parent)
        : QObject(parent),
          m_pLibrary(pLibrary),
          m_pSourcesProperty(new QmlLibrarySourceModel(m_pLibrary->sidebarModel(), this)),
          m_pModelProperty(new QmlLibraryTrackListModel(m_pLibrary->trackTableModel(), this)) {
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
    m_pLibrary->scanRekordboxDevices();
}

bool QmlLibraryProxy::mediaReady() const {
    return m_pLibrary->rekordboxDeviceCount() > 0;
}

bool QmlLibraryProxy::mediaScanInProgress() const {
    return m_pLibrary->isRekordboxDeviceScanRunning();
}

int QmlLibraryProxy::mediaDeviceCount() const {
    return m_pLibrary->rekordboxDeviceCount();
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
