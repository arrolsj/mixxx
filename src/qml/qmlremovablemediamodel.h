#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>

#include "library/removablemedia/removablemediadevice.h"

class Library;

namespace mixxx {
namespace qml {

class QmlRemovableMediaModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    QML_NAMED_ELEMENT(RemovableMediaModel)
    QML_UNCREATABLE("Only accessible through Mixxx.Library.mediaDevices")

  public:
    enum Roles {
        DeviceIdRole = Qt::UserRole,
        RemovalGroupIdRole,
        DisplayNameRole,
        GenerationRole,
        StateRole,
        CanStopRole,
        BusyReasonRole,
        ErrorCodeRole,
        ErrorMessageRole,
    };
    Q_ENUM(Roles)

    explicit QmlRemovableMediaModel(
            Library* pLibrary,
            QObject* parent = nullptr);

    int count() const {
        return rowCount();
    }
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOfDeviceId(const QString& deviceId) const;

  signals:
    void countChanged();

  private slots:
    void rebuild();

  private:
    QVariant valueForRole(
            const mixxx::removablemedia::RemovableMediaDevice& device,
            int role) const;

    Library* const m_pLibrary;
    QList<mixxx::removablemedia::RemovableMediaDevice> m_devices;
};

} // namespace qml
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::qml::QmlRemovableMediaModel*)
