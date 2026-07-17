#pragma once

#include <QIdentityProxyModel>
#include <QQmlEngine>

class SidebarModel;

namespace mixxx {
namespace qml {

class QmlLibrarySourceModel : public QIdentityProxyModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(LibrarySourceModel)
    QML_UNCREATABLE("Only accessible via Mixxx.Library.sources")

  public:
    enum Roles {
        DisplayRole = Qt::UserRole,
        LabelRole,
        IconNameRole,
        DataRole,
        HasChildrenRole,
    };
    Q_ENUM(Roles)

    explicit QmlLibrarySourceModel(SidebarModel* pModel, QObject* pParent = nullptr);

    QVariant data(const QModelIndex& proxyIndex, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QModelIndex mapToSidebar(const QModelIndex& proxyIndex) const;
};

} // namespace qml
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::qml::QmlLibrarySourceModel*)
