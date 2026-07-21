#include "qml/qmlcontrollerstatus.h"

#include <utility>

#include "controllers/controllermanager.h"
#include "moc_qmlcontrollerstatus.cpp"
#include "util/assert.h"

namespace mixxx {
namespace qml {
namespace {
const QHash<int, QByteArray> kRoleNames = {
        {QmlControllerStatus::NameRole, "name"},
        {QmlControllerStatus::OpenRole, "open"},
        {QmlControllerStatus::InputRole, "input"},
        {QmlControllerStatus::OutputRole, "output"},
        {QmlControllerStatus::MappingRole, "mapping"},
};

QString sanitizeDeviceName(QString name) {
    return name.replace(" ", "_").replace("/", "_").replace("\\", "_");
}
} // namespace

QmlControllerStatus::QmlControllerStatus(
        ControllerManager* pManager,
        QObject* pParent)
        : QAbstractListModel(pParent),
          m_pManager(pManager),
          m_hasOpenController(false) {
    VERIFY_OR_DEBUG_ASSERT(m_pManager) {
        return;
    }
    connect(m_pManager,
            &ControllerManager::devicesChanged,
            this,
            &QmlControllerStatus::rebuildDevices);
    connect(m_pManager,
            &ControllerManager::mappingApplied,
            this,
            &QmlControllerStatus::handleMappingApplied);
    connect(m_pManager,
            &ControllerManager::controllerDeviceDisconnected,
            this,
            &QmlControllerStatus::handleDeviceDisconnected);
    connect(m_pManager,
            &ControllerManager::controllerDeviceReconnected,
            this,
            &QmlControllerStatus::handleDeviceReconnected);
    rebuildDevices();
}

QmlControllerStatus::QmlControllerStatus()
        : QAbstractListModel(nullptr),
          m_pManager(nullptr),
          m_hasOpenController(false) {
}

// static
QmlControllerStatus* QmlControllerStatus::create(
        QQmlEngine* pQmlEngine,
        QJSEngine* pJsEngine) {
    Q_UNUSED(pJsEngine);
    VERIFY_OR_DEBUG_ASSERT(s_pControllerManager) {
        qWarning() << "ControllerManager has not been registered yet";
        return nullptr;
    }
    return new QmlControllerStatus(s_pControllerManager, pQmlEngine);
}

int QmlControllerStatus::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_controllers.size());
}

QVariant QmlControllerStatus::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid) ||
            index.row() >= m_controllers.size()) {
        return {};
    }
    Controller* pController = m_controllers.at(index.row());
    if (!pController) {
        return {};
    }

    switch (role) {
    case NameRole:
        return pController->getName();
    case OpenRole:
        return isControllerOpen(pController);
    case InputRole:
        return pController->isInputDevice();
    case OutputRole:
        return pController->isOutputDevice();
    case MappingRole: {
        const auto pMapping = pController->getMapping();
        return pMapping ? pMapping->name() : QString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> QmlControllerStatus::roleNames() const {
    return kRoleNames;
}

void QmlControllerStatus::rebuildDevices() {
    replaceDevices(m_pManager->getControllers());
}

void QmlControllerStatus::replaceDevices(
        const QList<Controller*>& controllers) {
    beginResetModel();
    for (const auto& pController : std::as_const(m_controllers)) {
        if (pController) {
            disconnect(pController, nullptr, this, nullptr);
        }
    }
    m_controllers.clear();
    m_controllers.reserve(controllers.size());
    for (Controller* pController : controllers) {
        m_controllers.append(pController);
        connect(pController,
                &Controller::openChanged,
                this,
                &QmlControllerStatus::handleOpenChanged);
    }
    endResetModel();
    emit devicesChanged();
    updateHasOpenController();
}

void QmlControllerStatus::handleOpenChanged() {
    auto* pController = qobject_cast<Controller*>(sender());
    for (int row = 0; row < m_controllers.size(); ++row) {
        if (m_controllers.at(row) == pController) {
            // ControllerManager applies the startup mapping immediately before
            // opening the device, without emitting mappingApplied.
            emit dataChanged(
                    index(row, 0), index(row, 0), {OpenRole, MappingRole});
            break;
        }
    }
    updateHasOpenController();
}

void QmlControllerStatus::handleMappingApplied() {
    if (!m_controllers.isEmpty()) {
        emit dataChanged(
                index(0, 0), index(m_controllers.size() - 1, 0), {MappingRole});
    }
}

void QmlControllerStatus::handleDeviceDisconnected(const QString& deviceName) {
    setDeviceForcedDisconnected(deviceName, true);
}

void QmlControllerStatus::handleDeviceReconnected(const QString& deviceName) {
    setDeviceForcedDisconnected(deviceName, false);
}

bool QmlControllerStatus::isControllerOpen(
        const Controller* pController) const {
    return pController && pController->isOpen() &&
            !m_forcedDisconnectedDeviceNames.contains(
                    sanitizeDeviceName(pController->getName()));
}

void QmlControllerStatus::setDeviceForcedDisconnected(
        const QString& deviceName,
        bool forcedDisconnected) {
    const QString sanitizedDeviceName = sanitizeDeviceName(deviceName);
    if (sanitizedDeviceName.isEmpty()) {
        return;
    }

    bool changed = false;
    if (forcedDisconnected) {
        if (!m_forcedDisconnectedDeviceNames.contains(sanitizedDeviceName)) {
            m_forcedDisconnectedDeviceNames.insert(sanitizedDeviceName);
            changed = true;
        }
    } else {
        changed = m_forcedDisconnectedDeviceNames.remove(sanitizedDeviceName);
    }
    if (!changed) {
        return;
    }

    for (int row = 0; row < m_controllers.size(); ++row) {
        const Controller* pController = m_controllers.at(row);
        if (pController &&
                sanitizeDeviceName(pController->getName()) ==
                        sanitizedDeviceName) {
            emit dataChanged(index(row, 0), index(row, 0), {OpenRole});
        }
    }
    updateHasOpenController();
}

void QmlControllerStatus::updateHasOpenController() {
    bool hasOpenController = false;
    for (const auto& pController : std::as_const(m_controllers)) {
        if (isControllerOpen(pController)) {
            hasOpenController = true;
            break;
        }
    }
    if (m_hasOpenController == hasOpenController) {
        return;
    }
    m_hasOpenController = hasOpenController;
    emit hasOpenControllerChanged();
}

} // namespace qml
} // namespace mixxx
