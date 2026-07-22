#include "qml/qmllibrarysourcemodel.h"

#include <functional>

#include "library/sidebarmodel.h"
#include "moc_qmllibrarysourcemodel.cpp"
#include "util/assert.h"

namespace mixxx {
namespace qml {
namespace {
const QHash<int, QByteArray> kRoleNames = {
        {QmlLibrarySourceModel::DisplayRole, "display"},
        {QmlLibrarySourceModel::LabelRole, "label"},
        {QmlLibrarySourceModel::IconNameRole, "iconName"},
        {QmlLibrarySourceModel::DataRole, "data"},
        {QmlLibrarySourceModel::HasChildrenRole, "hasChildren"},
        {QmlLibrarySourceModel::DeviceIdRole, "deviceId"},
};
} // namespace

QmlLibrarySourceModel::QmlLibrarySourceModel(SidebarModel* pModel, QObject* pParent)
        : QIdentityProxyModel(pParent) {
    setSourceModel(pModel);
}

QVariant QmlLibrarySourceModel::data(const QModelIndex& proxyIndex, int role) const {
    if (!proxyIndex.isValid()) {
        return {};
    }

    VERIFY_OR_DEBUG_ASSERT(checkIndex(proxyIndex)) {
        return {};
    }

    const QModelIndex sourceIndex = mapToSource(proxyIndex);
    const auto* pSidebarModel = qobject_cast<const SidebarModel*>(sourceModel());
    VERIFY_OR_DEBUG_ASSERT(pSidebarModel && sourceIndex.isValid()) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
    case DisplayRole:
    case LabelRole:
        return pSidebarModel->data(sourceIndex, Qt::DisplayRole);
    case IconNameRole:
        return pSidebarModel->data(sourceIndex, SidebarModel::IconNameRole);
    case DataRole:
        return pSidebarModel->data(sourceIndex, SidebarModel::DataRole);
    case HasChildrenRole:
        return pSidebarModel->hasChildren(sourceIndex);
    case DeviceIdRole:
        return deviceIdForSidebarIndex(sourceIndex);
    default:
        return QIdentityProxyModel::data(proxyIndex, role);
    }
}

QHash<int, QByteArray> QmlLibrarySourceModel::roleNames() const {
    return kRoleNames;
}

QModelIndex QmlLibrarySourceModel::mapToSidebar(const QModelIndex& proxyIndex) const {
    if (!proxyIndex.isValid() || proxyIndex.model() != this) {
        return {};
    }
    return mapToSource(proxyIndex);
}

QString QmlLibrarySourceModel::deviceIdForSidebarIndex(
        const QModelIndex& sidebarIndex) const {
    const auto* pSidebarModel = qobject_cast<const SidebarModel*>(sourceModel());
    if (!pSidebarModel) {
        return {};
    }
    QModelIndex current = sidebarIndex;
    while (current.isValid()) {
        const QVariantList data = pSidebarModel
                                          ->data(current, SidebarModel::DataRole)
                                          .toList();
        if (data.size() > 2) {
            return data.at(2).toString();
        }
        current = current.parent();
    }
    return {};
}

QModelIndex QmlLibrarySourceModel::sourceIndexForDevice(
        const QString& deviceId) const {
    if (deviceId.isEmpty()) {
        return {};
    }
    const auto* pSidebarModel = qobject_cast<const SidebarModel*>(sourceModel());
    if (!pSidebarModel) {
        return {};
    }

    std::function<QModelIndex(const QModelIndex&)> findDevice =
            [&](const QModelIndex& parent) -> QModelIndex {
        const int rows = pSidebarModel->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex candidate = pSidebarModel->index(row, 0, parent);
            if (deviceIdForSidebarIndex(candidate) == deviceId) {
                return candidate;
            }
            const QModelIndex nested = findDevice(candidate);
            if (nested.isValid()) {
                return nested;
            }
        }
        return {};
    };

    const QModelIndex sidebarIndex = findDevice(QModelIndex());
    return sidebarIndex.isValid() ? mapFromSource(sidebarIndex) : QModelIndex();
}

} // namespace qml
} // namespace mixxx
