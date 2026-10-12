#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include "controllers/controller.h"

class ControllerManager;

namespace mixxx {
namespace qml {

class QmlControllerStatus : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(mixxx::qml::QmlControllerStatus* devices READ devices CONSTANT)
    Q_PROPERTY(bool hasOpenController READ hasOpenController NOTIFY hasOpenControllerChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)
    Q_PROPERTY(QStringList detectedControllerNames READ detectedControllerNames NOTIFY controllerNamesChanged)
    Q_PROPERTY(QStringList openControllerNames READ openControllerNames NOTIFY controllerNamesChanged)
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
        return static_cast<int>(m_devices.size());
    }
    QStringList detectedControllerNames() const;
    QStringList openControllerNames() const;

  signals:
    void hasOpenControllerChanged();
    void devicesChanged();
    void controllerNamesChanged();

  protected:
    // Allows the device-list behavior to be exercised without constructing a
    // ControllerManager, which owns a hardware-enumeration thread. Production
    // instances are still created through the public manager constructor.
    QmlControllerStatus();
    void replaceDevices(const QList<Controller*>& controllers);
    // Apply a read-only ALSA sequencer inventory, independently of PortMidi's
    // frozen table. Invalid snapshots retain the last successful inventory.
    bool applyMidiTopology(const QByteArray& topology);

  private slots:
    void rebuildDevices();
    void handleOpenChanged();
    void handleMappingApplied();
    void handleDeviceDisconnected(const QString& deviceName);
    void handleDeviceReconnected(const QString& deviceName);

  private:
    bool isControllerOpen(const Controller* pController) const;
    QStringList controllerNames(bool openOnly) const;
    void setDeviceForcedDisconnected(
            const QString& deviceName, bool forcedDisconnected);
    void updateHasOpenController();
    void rebuildDeviceRows();
    void refreshMidiTopology();

    struct DeviceRow {
        QPointer<Controller> controller;
        QString name;
        bool input;
        bool output;
    };
    struct MidiPort {
        QString name;
        bool input;
        bool output;
        bool operator==(const MidiPort& other) const {
            return name == other.name && input == other.input && output == other.output;
        }
    };

    static inline ControllerManager* s_pControllerManager = nullptr;

    ControllerManager* const m_pManager;
    QList<QPointer<Controller>> m_controllers;
    QList<DeviceRow> m_devices;
    QList<MidiPort> m_midiPorts;
    bool m_hasMidiTopology{false};
    QTimer m_midiTopologyTimer;
    QSet<QString> m_forcedDisconnectedDeviceNames;
    bool m_hasOpenController;
};

} // namespace qml
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::qml::QmlControllerStatus*)
