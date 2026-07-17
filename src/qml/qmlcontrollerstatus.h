#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QQmlEngine>

#include "controllers/controller.h"

class ControllerManager;

namespace mixxx {
namespace qml {

class QmlControllerStatus : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(mixxx::qml::QmlControllerStatus* devices READ devices CONSTANT)
    Q_PROPERTY(bool hasOpenController READ hasOpenController NOTIFY hasOpenControllerChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)
    QML_NAMED_ELEMENT(ControllerStatus)
    QML_SINGLETON

  public:
    enum Roles {
        NameRole = Qt::UserRole,
        OpenRole,
        InputRole,
        OutputRole,
        MappingRole,
    };
    Q_ENUM(Roles)

    explicit QmlControllerStatus(ControllerManager* pManager, QObject* pParent = nullptr);

    static QmlControllerStatus* create(QQmlEngine* pQmlEngine, QJSEngine* pJsEngine);
    static void registerControllerManager(ControllerManager* pManager) {
        s_pControllerManager = pManager;
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QmlControllerStatus* devices() {
        return this;
    }
    bool hasOpenController() const {
        return m_hasOpenController;
    }
    int deviceCount() const {
        return static_cast<int>(m_controllers.size());
    }

  signals:
    void hasOpenControllerChanged();
    void devicesChanged();

  protected:
    // Allows the device-list behavior to be exercised without constructing a
    // ControllerManager, which owns a hardware-enumeration thread. Production
    // instances are still created through the public manager constructor.
    QmlControllerStatus();
    void replaceDevices(const QList<Controller*>& controllers);

  private slots:
    void rebuildDevices();
    void handleOpenChanged();
    void handleMappingApplied();

  private:
    void updateHasOpenController();

    static inline ControllerManager* s_pControllerManager = nullptr;

    ControllerManager* const m_pManager;
    QList<QPointer<Controller>> m_controllers;
    bool m_hasOpenController;
};

} // namespace qml
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::qml::QmlControllerStatus*)
