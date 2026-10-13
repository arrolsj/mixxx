#include <gtest/gtest.h>

#include <QList>
#include <QMetaObject>

#include <memory>
#include <utility>

#include "controllers/controller.h"
#include "controllers/midi/legacymidicontrollermapping.h"
#include "qml/qmlcontrollerstatus.h"
#include "test/mixxxtest.h"

namespace {

class FakeController final : public Controller {
  public:
    FakeController(const QString& name, bool input, bool output,
            QString extension = QStringLiteral(".test.xml"))
            : Controller(name), m_extension(std::move(extension)) {
        setInputDevice(input);
        setOutputDevice(output);
    }

    ~FakeController() override {
        if (isOpen()) {
            close();
        }
    }

    QString mappingExtension() override {
        return m_extension;
    }

    void setMapping(std::shared_ptr<LegacyControllerMapping> pMapping) override {
        m_pMutableMapping = std::move(pMapping);
    }

    QList<LegacyControllerMapping::ScriptFileInfo> getMappingScriptFiles() override {
        return m_pMutableMapping ? m_pMutableMapping->getScriptFiles()
                                 : QList<LegacyControllerMapping::ScriptFileInfo>();
    }

    QList<std::shared_ptr<AbstractLegacyControllerSetting>> getMappingSettings() override {
        return m_pMutableMapping
                ? m_pMutableMapping->getSettings()
                : QList<std::shared_ptr<AbstractLegacyControllerSetting>>();
    }

    bool isMappable() const override {
        return true;
    }

    bool matchMapping(const MappingInfo& mapping) override {
        Q_UNUSED(mapping);
        return false;
    }

    void setOpened(bool open) {
        setOpen(open);
    }

  protected:
    void sendBytes(const QByteArray& data) override {
        Q_UNUSED(data);
    }

  private:
    QString m_extension;
    int open() override {
        setOpen(true);
        return 0;
    }

    int close() override {
        setOpen(false);
        return 0;
    }
};

class TestableQmlControllerStatus final
        : public mixxx::qml::QmlControllerStatus {
  public:
    TestableQmlControllerStatus()
            : QmlControllerStatus() {
    }

    void setControllers(const QList<Controller*>& controllers) {
        replaceDevices(controllers);
    }
    bool setMidiTopology(const QByteArray& topology) {
        return applyMidiTopology(topology);
    }
};

class QmlControllerStatusTest : public MixxxTest {
};

TEST_F(QmlControllerStatusTest, RebuildsDeviceRowsAndAggregateOpenState) {
    FakeController first(QStringLiteral("SX3"), true, false);
    FakeController second(QStringLiteral("Loopback"), false, true);
    auto mapping = std::make_shared<LegacyMidiControllerMapping>();
    mapping->setName(QStringLiteral("LastCUE SX3"));
    first.setMapping(mapping);
    first.setOpened(true);

    TestableQmlControllerStatus status;
    int devicesChangedCount = 0;
    int openStateChangedCount = 0;
    QObject::connect(&status,
            &mixxx::qml::QmlControllerStatus::devicesChanged,
            &status,
            [&devicesChangedCount] { ++devicesChangedCount; });
    QObject::connect(&status,
            &mixxx::qml::QmlControllerStatus::hasOpenControllerChanged,
            &status,
            [&openStateChangedCount] { ++openStateChangedCount; });

    status.setControllers(QList<Controller*>{&first, &second});
    ASSERT_EQ(status.rowCount(), 2);
    EXPECT_EQ(status.deviceCount(), 2);
    EXPECT_TRUE(status.hasOpenController());
    EXPECT_EQ(status.detectedControllerNames(),
            QStringList({QStringLiteral("SX3"), QStringLiteral("Loopback")}));
    EXPECT_EQ(status.openControllerNames(),
            QStringList({QStringLiteral("SX3")}));
    EXPECT_EQ(devicesChangedCount, 1);
    EXPECT_EQ(openStateChangedCount, 1);

    const QModelIndex firstIndex = status.index(0, 0);
    EXPECT_QSTRING_EQ(QStringLiteral("SX3"),
            status.data(firstIndex,
                          mixxx::qml::QmlControllerStatus::NameRole)
                    .toString());
    EXPECT_TRUE(status.data(firstIndex,
                              mixxx::qml::QmlControllerStatus::OpenRole)
                        .toBool());
    EXPECT_TRUE(status.data(firstIndex,
                              mixxx::qml::QmlControllerStatus::InputRole)
                        .toBool());
    EXPECT_FALSE(status.data(firstIndex,
                               mixxx::qml::QmlControllerStatus::OutputRole)
                         .toBool());
    EXPECT_QSTRING_EQ(QStringLiteral("LastCUE SX3"),
            status.data(firstIndex,
                          mixxx::qml::QmlControllerStatus::MappingRole)
                    .toString());

    const QModelIndex secondIndex = status.index(1, 0);
    EXPECT_FALSE(status.data(secondIndex,
                               mixxx::qml::QmlControllerStatus::InputRole)
                         .toBool());
    EXPECT_TRUE(status.data(secondIndex,
                              mixxx::qml::QmlControllerStatus::OutputRole)
                        .toBool());

    status.setControllers(QList<Controller*>{&second});
    ASSERT_EQ(status.rowCount(), 1);
    EXPECT_EQ(status.deviceCount(), 1);
    EXPECT_FALSE(status.hasOpenController());
    EXPECT_EQ(status.detectedControllerNames(),
            QStringList({QStringLiteral("Loopback")}));
    EXPECT_TRUE(status.openControllerNames().isEmpty());
    EXPECT_EQ(devicesChangedCount, 2);
    EXPECT_EQ(openStateChangedCount, 2);
    EXPECT_QSTRING_EQ(QStringLiteral("Loopback"),
            status.data(status.index(0, 0),
                          mixxx::qml::QmlControllerStatus::NameRole)
                    .toString());
}

TEST_F(QmlControllerStatusTest, NotifiesOpenAndMappingRoles) {
    FakeController controller(QStringLiteral("SX3"), true, true);
    TestableQmlControllerStatus status;
    status.setControllers(QList<Controller*>{&controller});

    QList<QList<int>> roleNotifications;
    int openStateChangedCount = 0;
    QObject::connect(&status,
            &QAbstractItemModel::dataChanged,
            &status,
            [&roleNotifications](const QModelIndex&,
                    const QModelIndex&,
                    const QList<int>& roles) {
                roleNotifications.append(roles);
            });
    QObject::connect(&status,
            &mixxx::qml::QmlControllerStatus::hasOpenControllerChanged,
            &status,
            [&openStateChangedCount] { ++openStateChangedCount; });

    auto startupMapping = std::make_shared<LegacyMidiControllerMapping>();
    startupMapping->setName(QStringLiteral("Startup mapping"));
    controller.setMapping(startupMapping);
    controller.setOpened(true);

    ASSERT_EQ(roleNotifications.size(), 1);
    const QList<int>& openRoles = roleNotifications.constFirst();
    EXPECT_EQ(openRoles.size(), 2);
    EXPECT_TRUE(openRoles.contains(mixxx::qml::QmlControllerStatus::OpenRole));
    EXPECT_TRUE(openRoles.contains(mixxx::qml::QmlControllerStatus::MappingRole));
    EXPECT_EQ(openStateChangedCount, 1);
    EXPECT_TRUE(status.hasOpenController());
    EXPECT_QSTRING_EQ(QStringLiteral("Startup mapping"),
            status.data(status.index(0, 0),
                          mixxx::qml::QmlControllerStatus::MappingRole)
                    .toString());

    roleNotifications.clear();
    auto replacementMapping = std::make_shared<LegacyMidiControllerMapping>();
    replacementMapping->setName(QStringLiteral("Replacement mapping"));
    controller.setMapping(replacementMapping);
    EXPECT_TRUE(QMetaObject::invokeMethod(
            &status, "handleMappingApplied", Qt::DirectConnection));

    ASSERT_EQ(roleNotifications.size(), 1);
    const QList<int>& mappingRoles = roleNotifications.constFirst();
    ASSERT_EQ(mappingRoles.size(), 1);
    EXPECT_EQ(mappingRoles.constFirst(),
            mixxx::qml::QmlControllerStatus::MappingRole);
    EXPECT_QSTRING_EQ(QStringLiteral("Replacement mapping"),
            status.data(status.index(0, 0),
                          mixxx::qml::QmlControllerStatus::MappingRole)
                    .toString());
}

TEST_F(QmlControllerStatusTest,
        ForcesMatchingDeviceRowsClosedUntilReconnected) {
    FakeController input(QStringLiteral("PIONEER DJ DDJ-SX3"), true, false);
    FakeController output(QStringLiteral("PIONEER DJ DDJ-SX3"), false, true);
    FakeController other(QStringLiteral("Loopback"), true, false);
    input.setOpened(true);
    output.setOpened(true);
    other.setOpened(true);

    TestableQmlControllerStatus status;
    status.setControllers(QList<Controller*>{&input, &output, &other});

    const auto isOpen = [&status](int row) {
        return status
                .data(status.index(row, 0),
                        mixxx::qml::QmlControllerStatus::OpenRole)
                .toBool();
    };
    const QString deviceName = QStringLiteral("PIONEER_DJ_DDJ-SX3");

    ASSERT_TRUE(status.hasOpenController());
    EXPECT_EQ(status.openControllerNames(),
            QStringList({QStringLiteral("PIONEER DJ DDJ-SX3"),
                    QStringLiteral("Loopback")}));
    ASSERT_TRUE(QMetaObject::invokeMethod(&status,
            "handleDeviceDisconnected",
            Qt::DirectConnection,
            Q_ARG(QString, deviceName)));
    EXPECT_FALSE(isOpen(0));
    EXPECT_FALSE(isOpen(1));
    EXPECT_TRUE(isOpen(2));
    EXPECT_TRUE(status.hasOpenController());
    EXPECT_TRUE(input.isOpen());
    EXPECT_TRUE(output.isOpen());
    EXPECT_EQ(status.openControllerNames(),
            QStringList({QStringLiteral("Loopback")}));

    // Enumeration must not temporarily clear the logical disconnect state
    // while stale native controller objects still report themselves open.
    status.setControllers(QList<Controller*>{&input, &output, &other});
    EXPECT_FALSE(isOpen(0));
    EXPECT_FALSE(isOpen(1));
    EXPECT_TRUE(isOpen(2));

    other.setOpened(false);
    EXPECT_FALSE(status.hasOpenController());

    ASSERT_TRUE(QMetaObject::invokeMethod(&status,
            "handleDeviceReconnected",
            Qt::DirectConnection,
            Q_ARG(QString, deviceName)));
    EXPECT_TRUE(isOpen(0));
    EXPECT_TRUE(isOpen(1));
    EXPECT_FALSE(isOpen(2));
    EXPECT_TRUE(status.hasOpenController());
    EXPECT_EQ(status.openControllerNames(),
            QStringList({QStringLiteral("PIONEER DJ DDJ-SX3")}));
}

TEST_F(QmlControllerStatusTest, DiscoversUnknownMidiWithoutOpeningOrMappingIt) {
    TestableQmlControllerStatus status;
    const QByteArray empty("Client info\nClient 0 : \"System\" [Kernel]\n"
                           "  Port 1 : \"Announce\" (R-e-)\n");
    ASSERT_TRUE(status.setMidiTopology(empty));
    EXPECT_EQ(status.deviceCount(), 0);
    const QByteArray plugged = empty +
            "Client 32 : \"New DJ Device\" [Kernel]\n"
            "  Port 0 : \"New DJ Device MIDI 1\" (RWeX)\n";
    ASSERT_TRUE(status.setMidiTopology(plugged));
    ASSERT_EQ(status.deviceCount(), 1);
    EXPECT_EQ(status.detectedControllerNames(),
            QStringList({QStringLiteral("New DJ Device MIDI 1")}));
    EXPECT_FALSE(status.hasOpenController());
    EXPECT_TRUE(status.openControllerNames().isEmpty());
    EXPECT_TRUE(status.data(status.index(0, 0), status.MappingRole).toString().isEmpty());
    EXPECT_TRUE(status.data(status.index(0, 0), status.InputRole).toBool());
    EXPECT_TRUE(status.data(status.index(0, 0), status.OutputRole).toBool());

    int resets = 0;
    QObject::connect(&status, &QAbstractItemModel::modelReset,
            &status, [&resets] { ++resets; });
    ASSERT_TRUE(status.setMidiTopology(plugged));
    EXPECT_EQ(resets, 0);
    ASSERT_TRUE(status.setMidiTopology(empty));
    EXPECT_EQ(status.deviceCount(), 0);
    EXPECT_EQ(resets, 1);
}

TEST_F(QmlControllerStatusTest, RequestsOnlyUnmanagedSupportedMidiInputs) {
    TestableQmlControllerStatus status;
    QStringList requested;
    int requests = 0;
    QObject::connect(&status,
            &mixxx::qml::QmlControllerStatus::pioneerInputsDiscovered,
            &status, [&](const QStringList& names) { requested = names; ++requests; });
    const QByteArray topology("Client info\nClient 24 : \"Pioneer\" [Kernel]\n"
                             "  Port 0 : \"DDJ-FLX4 MIDI 1\" (RWeX)\n"
                             "  Port 1 : \"DDJ-400\" (-WeX)\n"
                             "  Port 2 : \"Unknown DDJ-400\" (RWeX)\n");
    ASSERT_TRUE(status.setMidiTopology(topology));
    EXPECT_EQ(requested, QStringList({QStringLiteral("DDJ-FLX4 MIDI 1")}));
    EXPECT_FALSE(status.hasOpenController());
    ASSERT_TRUE(status.setMidiTopology(topology));
    EXPECT_EQ(requests, 2); // manager can retry deferred discovery, no native I/O here
    FakeController managed(QStringLiteral("DDJ-FLX4 MIDI 1"), true, true,
            QStringLiteral(".midi.xml"));
    status.setControllers({&managed});
    ASSERT_TRUE(status.setMidiTopology(topology));
    EXPECT_TRUE(requested.isEmpty());
    ASSERT_TRUE(status.setMidiTopology("Client info\nClient 0 : \"System\" [Kernel]\n"));
    EXPECT_TRUE(requested.isEmpty());
    EXPECT_FALSE(status.setMidiTopology("broken"));
    EXPECT_EQ(requests, 4);
}

TEST_F(QmlControllerStatusTest, InventoryPreservesOpenStreamsAndRemovesStaleMidiRows) {
    FakeController midi(QStringLiteral("SX3 MIDI 1"), true, true,
            QStringLiteral(".midi.xml"));
    FakeController hid(QStringLiteral("HID Controller"), true, false);
    midi.setOpened(true);
    TestableQmlControllerStatus status;
    status.setControllers({&midi, &hid});
    const QByteArray present("Client info\nClient 24 : \"SX3\" [Kernel]\n"
                             "  Port 0 : \"SX3 MIDI 1\" (RWeX)\n");
    ASSERT_TRUE(status.setMidiTopology(present));
    EXPECT_EQ(status.deviceCount(), 2); // no duplicate for the managed SX3
    EXPECT_TRUE(status.hasOpenController());
    ASSERT_TRUE(status.setMidiTopology(present +
            "Client 40 : \"Unknown\" [Kernel]\n"
            "  Port 0 : \"Unknown MIDI\" (RWeX)\n"));
    EXPECT_EQ(status.deviceCount(), 3);
    EXPECT_TRUE(midi.isOpen());
    EXPECT_EQ(status.openControllerNames(), QStringList({QStringLiteral("SX3 MIDI 1")}));
    ASSERT_TRUE(status.setMidiTopology("Client info\nClient 0 : \"System\" [Kernel]\n"));
    EXPECT_EQ(status.detectedControllerNames(), QStringList({QStringLiteral("HID Controller")}));
    EXPECT_FALSE(status.hasOpenController());
    EXPECT_TRUE(midi.isOpen()); // inventory never closes native streams
    ASSERT_TRUE(status.setMidiTopology(present));
    EXPECT_EQ(status.deviceCount(), 2);
}

TEST_F(QmlControllerStatusTest, FiltersInternalPortsAndKeepsDirectionsAndUnicode) {
    TestableQmlControllerStatus status;
    ASSERT_TRUE(status.setMidiTopology(QStringLiteral(
            "Client info\nClient 0 : \"System\" [Kernel]\n"
            "  Port 0 : \"Timer\" (Rwe-)\n"
            "Client 14 : \"Midi Through\" [Kernel]\n"
            "  Port 0 : \"MIDI Through Port-0\" (RWe-)\n"
            "Client 128 : \"Mixxx\" [User]\n"
            "  Port 0 : \"port-0\" (rwe-)\n"
            "Client 30 : \"Hardware\" [Kernel]\n"
            "  Port 0 : \"控制器 Input\" (R-eX)\n"
            "  Port 1 : \"LED Output\" (-WeX)\n").toLocal8Bit()));
    ASSERT_EQ(status.deviceCount(), 2);
    EXPECT_EQ(status.data(status.index(0, 0), status.NameRole).toString(),
            QStringLiteral("控制器 Input"));
    EXPECT_TRUE(status.data(status.index(0, 0), status.InputRole).toBool());
    EXPECT_FALSE(status.data(status.index(0, 0), status.OutputRole).toBool());
    EXPECT_FALSE(status.data(status.index(1, 0), status.InputRole).toBool());
    EXPECT_TRUE(status.data(status.index(1, 0), status.OutputRole).toBool());
}

TEST_F(QmlControllerStatusTest, InvalidInventoryDoesNotErasePreviousDevices) {
    TestableQmlControllerStatus status;
    ASSERT_TRUE(status.setMidiTopology(
            "Client info\nClient 24 : \"SX3\" [Kernel]\n"
            "  Port 0 : \"SX3 MIDI 1\" (RWeX)\n"));
    EXPECT_FALSE(status.setMidiTopology({}));
    EXPECT_FALSE(status.setMidiTopology("Client info\n"));
    EXPECT_FALSE(status.setMidiTopology(
            "Client info\nClient 24 : \"SX3\" [Kernel]\n  Port 0 : \"truncated"));
    EXPECT_EQ(status.deviceCount(), 1);
}

} // namespace
