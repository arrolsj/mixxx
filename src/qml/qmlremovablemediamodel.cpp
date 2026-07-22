#include "qml/qmlremovablemediamodel.h"

#include "library/library.h"
#include "moc_qmlremovablemediamodel.cpp"

namespace mixxx {
namespace qml {
namespace {

const QHash<int, QByteArray> kRoleNames = {
        {QmlRemovableMediaModel::DeviceIdRole, "deviceId"},
        {QmlRemovableMediaModel::RemovalGroupIdRole, "removalGroupId"},
        {QmlRemovableMediaModel::DisplayNameRole, "displayName"},
        {QmlRemovableMediaModel::GenerationRole, "generation"},
        {QmlRemovableMediaModel::StateRole, "state"},
        {QmlRemovableMediaModel::CanStopRole, "canStop"},
        {QmlRemovableMediaModel::BusyReasonRole, "busyReason"},
        {QmlRemovableMediaModel::ErrorCodeRole, "errorCode"},
        {QmlRemovableMediaModel::ErrorMessageRole, "errorMessage"},
};

} // namespace

QmlRemovableMediaModel::QmlRemovableMediaModel(
        Library* pLibrary,
        QObject* parent)
        : QAbstractListModel(parent),
          m_pLibrary(pLibrary) {
    connect(m_pLibrary,
            &Library::removableMediaDevicesChanged,
            this,
            &QmlRemovableMediaModel::rebuild);
    rebuild();
}

int QmlRemovableMediaModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_devices.size();
}

QVariant QmlRemovableMediaModel::data(
        const QModelIndex& index,
        int role) const {
    if (!checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid) ||
            index.row() >= m_devices.size()) {
        return {};
    }
    return valueForRole(m_devices.at(index.row()), role);
}

QHash<int, QByteArray> QmlRemovableMediaModel::roleNames() const {
    return kRoleNames;
}

QVariantMap QmlRemovableMediaModel::get(int row) const {
    QVariantMap values;
    if (row < 0 || row >= m_devices.size()) {
        return values;
    }
    const auto& device = m_devices.at(row);
    for (auto role = kRoleNames.cbegin(); role != kRoleNames.cend(); ++role) {
        values.insert(QString::fromLatin1(role.value()),
                valueForRole(device, role.key()));
    }
    return values;
}

int QmlRemovableMediaModel::indexOfDeviceId(
        const QString& deviceId) const {
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).deviceId == deviceId) {
            return row;
        }
    }
    return -1;
}

void QmlRemovableMediaModel::rebuild() {
    const int oldCount = m_devices.size();
    beginResetModel();
    m_devices = m_pLibrary->removableMediaDevices();
    endResetModel();
    if (oldCount != m_devices.size()) {
        emit countChanged();
    }
}

QVariant QmlRemovableMediaModel::valueForRole(
        const removablemedia::RemovableMediaDevice& device,
        int role) const {
    switch (role) {
    case DeviceIdRole:
        return device.deviceId;
    case RemovalGroupIdRole:
        return device.removalGroupId;
    case DisplayNameRole:
        return device.displayName;
    case GenerationRole:
        return QVariant::fromValue<qulonglong>(device.generation);
    case StateRole:
        return removablemedia::mediaStateToString(device.state);
    case CanStopRole:
        return device.canStop;
    case BusyReasonRole:
        return device.busyReason;
    case ErrorCodeRole:
        return removablemedia::errorCodeToString(device.errorCode);
    case ErrorMessageRole:
        return device.errorMessage;
    default:
        return {};
    }
}

} // namespace qml
} // namespace mixxx
