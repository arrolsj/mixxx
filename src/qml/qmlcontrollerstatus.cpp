#include "qml/qmlcontrollerstatus.h"

#include <QFile>
#include <QRegularExpression>

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
#if defined(__LINUX__)
    // Inspect system inventory without closing streams or reinitializing
    // PortMidi. Discovery must never interrupt an already open controller.
    connect(&m_midiTopologyTimer, &QTimer::timeout,
            this, &QmlControllerStatus::refreshMidiTopology);
    m_midiTopologyTimer.start(1000);
    refreshMidiTopology();
#endif
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
    return parent.isValid() ? 0 : deviceCount();
}

QVariant QmlControllerStatus::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid) ||
            index.row() >= m_devices.size()) {
        return {};
    }
    const auto& device = m_devices.at(index.row());
    Controller* pController = device.controller;

    switch (role) {
    case NameRole:
        return device.name;
    case OpenRole:
        return isControllerOpen(pController);
    case InputRole:
        return device.input;
    case OutputRole:
        return device.output;
    case MappingRole: {
        const auto pMapping = pController ? pController->getMapping() : nullptr;
        return pMapping ? pMapping->name() : QString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> QmlControllerStatus::roleNames() const {
    return kRoleNames;
}

QStringList QmlControllerStatus::detectedControllerNames() const {
    return controllerNames(false);
}

QStringList QmlControllerStatus::openControllerNames() const {
    return controllerNames(true);
}

QStringList QmlControllerStatus::controllerNames(bool openOnly) const {
    QStringList names;
    for (const auto& device : m_devices) {
        if (openOnly && !isControllerOpen(device.controller)) {
            continue;
        }
        const QString name = device.name.trimmed();
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

void QmlControllerStatus::rebuildDevices() {
    replaceDevices(m_pManager->getControllers());
}

void QmlControllerStatus::replaceDevices(
        const QList<Controller*>& controllers) {
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
    rebuildDeviceRows();
}

void QmlControllerStatus::rebuildDeviceRows() {
    beginResetModel();
    m_devices.clear();
    QSet<QString> managedNames;
    for (const auto& pController : std::as_const(m_controllers)) {
        if (!pController) {
            continue;
        }
        const QString name = pController->getName();
        if (m_hasMidiTopology &&
                pController->mappingExtension() == QStringLiteral(".midi.xml")) {
            bool present = false;
            for (const auto& port : std::as_const(m_midiPorts)) {
                present |= port.name == name;
            }
            if (!present) {
                continue;
            }
        }
        managedNames.insert(name);
        m_devices.append({pController, name,
                pController->isInputDevice(), pController->isOutputDevice()});
    }
    for (const auto& port : std::as_const(m_midiPorts)) {
        if (!managedNames.contains(port.name)) {
            m_devices.append({nullptr, port.name, port.input, port.output});
        }
    }
    endResetModel();
    emit devicesChanged();
    emit controllerNamesChanged();
    updateHasOpenController();
}

void QmlControllerStatus::refreshMidiTopology() {
    QFile file(QStringLiteral("/proc/asound/seq/clients"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        applyMidiTopology(file.readAll());
    }
}

bool QmlControllerStatus::applyMidiTopology(const QByteArray& topology) {
    if (!topology.startsWith("Client info\n")) {
        return false;
    }
    static const QRegularExpression clientPattern(
            QStringLiteral("^Client\\s+(\\d+)\\s*:"));
    static const QRegularExpression portPattern(
            QStringLiteral("^Port\\s+\\d+\\s*:\\s*\"(.*)\"\\s*\\(([Rr-])([Ww-])[e-][X-]\\)"));
    QList<MidiPort> ports;
    int client = -1;
    bool sawClient = false;
    for (const auto& rawLine : topology.split('\n')) {
        const QString line = QString::fromLocal8Bit(rawLine).trimmed();
        if (line == QStringLiteral("Client info")) {
            continue;
        }
        const auto clientMatch = clientPattern.match(line);
        if (clientMatch.hasMatch()) {
            sawClient = true;
            client = clientMatch.captured(1).toInt();
            continue;
        }
        const auto portMatch = portPattern.match(line);
        if (line.startsWith(QStringLiteral("Client ")) ||
                (line.startsWith(QStringLiteral("Port ")) && !portMatch.hasMatch())) {
            return false;
        }
        if (client <= 0 || !portMatch.hasMatch()) {
            continue;
        }
        const QString name = portMatch.captured(1).trimmed();
        const bool input = portMatch.captured(2) == QStringLiteral("R");
        const bool output = portMatch.captured(3) == QStringLiteral("W");
        // Uppercase R/W denotes subscribable ports. Lowercase application
        // endpoints, System events and the built-in MIDI Through are not devices.
        if (name.isEmpty() || (!input && !output) ||
                name.startsWith(QStringLiteral("MIDI Through Port"), Qt::CaseInsensitive)) {
            continue;
        }
        ports.append({name, input, output});
    }
    if (!sawClient) {
        return false;
    }
    if (!m_hasMidiTopology || ports != m_midiPorts) {
        m_hasMidiTopology = true;
        m_midiPorts = std::move(ports);
        rebuildDeviceRows();
    }
    return true;
}

void QmlControllerStatus::handleOpenChanged() {
    auto* pController = qobject_cast<Controller*>(sender());
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).controller == pController) {
            // ControllerManager applies the startup mapping immediately before
            // opening the device, without emitting mappingApplied.
            emit dataChanged(
                    index(row, 0), index(row, 0), {OpenRole, MappingRole});
            break;
        }
    }
    emit controllerNamesChanged();
    updateHasOpenController();
}

void QmlControllerStatus::handleMappingApplied() {
    if (!m_devices.isEmpty()) {
        emit dataChanged(
                index(0, 0), index(m_devices.size() - 1, 0), {MappingRole});
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

    for (int row = 0; row < m_devices.size(); ++row) {
        const Controller* pController = m_devices.at(row).controller;
        if (pController &&
                sanitizeDeviceName(pController->getName()) ==
                        sanitizedDeviceName) {
            emit dataChanged(index(row, 0), index(row, 0), {OpenRole});
        }
    }
    emit controllerNamesChanged();
    updateHasOpenController();
}

void QmlControllerStatus::updateHasOpenController() {
    bool hasOpenController = false;
    for (const auto& device : std::as_const(m_devices)) {
        if (isControllerOpen(device.controller)) {
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
