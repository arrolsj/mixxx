#include "qml/qmllibrarysourcemodel.h"

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

} // namespace qml
} // namespace mixxx
