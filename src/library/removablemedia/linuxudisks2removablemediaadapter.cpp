#include "library/removablemedia/linuxudisks2removablemediaadapter.h"

#if defined(__LINUX__)
#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QHash>
#include <QMetaType>
#include <QVariantMap>
#include <algorithm>
#include <utility>
#endif

namespace mixxx {
namespace removablemedia {

#if defined(__LINUX__)
namespace {

const QString kUDisksService = QStringLiteral("org.freedesktop.UDisks2");
const QString kManagerPath = QStringLiteral("/org/freedesktop/UDisks2/Manager");
const QString kManagerInterface = QStringLiteral("org.freedesktop.UDisks2.Manager");
const QString kPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kBlockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kFilesystemInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString kPartitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");
const QString kDriveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
constexpr int kDbusTimeoutMillis = 5000;

struct UDisksDeviceRecord {
    RemovableMediaDevice device;
    QString blockObjectPath;
    QString driveObjectPath;
    QString siblingId;
    QByteArray deviceIdentitySuffix;
    bool canPowerOff{false};
    bool unsafeSibling{false};
    bool exposeDevice{true};
};

struct UDisksInventory {
    bool valid{false};
    QList<UDisksDeviceRecord> records;
};

QVariantMap getAllProperties(
        const QString& objectPath,
        const QString& interfaceName,
        bool* pOk = nullptr) {
    QDBusInterface properties(kUDisksService,
            objectPath,
            kPropertiesInterface,
            QDBusConnection::systemBus());
    properties.setTimeout(kDbusTimeoutMillis);
    const QDBusReply<QVariantMap> reply = properties.call(
            QStringLiteral("GetAll"), interfaceName);
    if (pOk) {
        *pOk = reply.isValid();
    }
    return reply.isValid() ? reply.value() : QVariantMap{};
}

QStringList decodeByteArrayList(
        const QVariant& value,
        bool* pOk = nullptr) {
    QStringList mountPoints;
    if (pOk) {
        *pOk = false;
    }
    if (!value.isValid()) {
        return mountPoints;
    }

    if (value.canConvert<QList<QByteArray>>()) {
        const auto byteArrays = value.value<QList<QByteArray>>();
        for (QByteArray bytes : byteArrays) {
            if (bytes.endsWith('\0')) {
                bytes.chop(1);
            }
            const QString path = QDir::cleanPath(QString::fromLocal8Bit(bytes));
            if (!path.isEmpty()) {
                mountPoints.append(path);
            }
        }
        if (pOk) {
            *pOk = true;
        }
        return mountPoints;
    }

    if (value.canConvert<QDBusArgument>()) {
        const QDBusArgument argument = value.value<QDBusArgument>();
        if (argument.currentType() != QDBusArgument::ArrayType) {
            return mountPoints;
        }
        argument.beginArray();
        while (!argument.atEnd()) {
            QByteArray bytes;
            argument >> bytes;
            if (bytes.endsWith('\0')) {
                bytes.chop(1);
            }
            const QString path = QDir::cleanPath(QString::fromLocal8Bit(bytes));
            if (!path.isEmpty()) {
                mountPoints.append(path);
            }
        }
        argument.endArray();
        if (pOk) {
            *pOk = true;
        }
    }
    return mountPoints;
}

QString objectPathFromVariant(const QVariant& value) {
    if (value.canConvert<QDBusObjectPath>()) {
        return value.value<QDBusObjectPath>().path();
    }
    return value.toString();
}

QString opaqueHash(const QString& prefix, const QByteArray& identity) {
    const QByteArray digest = QCryptographicHash::hash(
            identity, QCryptographicHash::Sha256)
                                      .toHex();
    return prefix + QString::fromLatin1(digest);
}

bool readRequiredBoolean(
        const QVariantMap& properties,
        const QString& name,
        bool* pValue) {
    const auto property = properties.constFind(name);
    if (property == properties.cend() ||
            property->userType() != QMetaType::Bool) {
        return false;
    }
    *pValue = property->toBool();
    return true;
}

UDisksInventory enumerateRecords() {
    UDisksInventory inventory;
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return inventory;
    }

    QDBusInterface manager(kUDisksService,
            kManagerPath,
            kManagerInterface,
            bus);
    manager.setTimeout(kDbusTimeoutMillis);
    const QDBusReply<QList<QDBusObjectPath>> blockReply = manager.call(
            QStringLiteral("GetBlockDevices"), QVariantMap{});
    if (!blockReply.isValid()) {
        return inventory;
    }

    for (const QDBusObjectPath& blockPath : blockReply.value()) {
        bool blockPropertiesValid = false;
        const QVariantMap blockProperties = getAllProperties(
                blockPath.path(), kBlockInterface, &blockPropertiesValid);
        if (!blockPropertiesValid) {
            return {};
        }
        const QString drivePath = objectPathFromVariant(
                blockProperties.value(QStringLiteral("Drive")));
        if (drivePath.isEmpty() || drivePath == QStringLiteral("/")) {
            continue;
        }
        bool hintSystem = false;
        bool hintIgnore = false;
        if (!readRequiredBoolean(blockProperties,
                    QStringLiteral("HintSystem"),
                    &hintSystem) ||
                !readRequiredBoolean(blockProperties,
                        QStringLiteral("HintIgnore"),
                        &hintIgnore)) {
            // Missing or ill-typed safety metadata makes the authoritative
            // physical-drive inventory unusable. Never guess false.
            return {};
        }

        bool drivePropertiesValid = false;
        const QVariantMap driveProperties = getAllProperties(
                drivePath, kDriveInterface, &drivePropertiesValid);
        if (!drivePropertiesValid) {
            return {};
        }
        const auto siblingProperty =
                driveProperties.constFind(QStringLiteral("SiblingId"));
        if (siblingProperty == driveProperties.cend() ||
                siblingProperty->userType() != QMetaType::QString) {
            // PowerOff operates on a physical power domain. Without this
            // property the adapter cannot prove that another Drive object
            // will not be affected.
            return {};
        }
        const QString siblingId = siblingProperty->toString().trimmed();
        const bool removable =
                driveProperties.value(QStringLiteral("Removable")).toBool() ||
                driveProperties.value(QStringLiteral("MediaRemovable")).toBool() ||
                driveProperties.value(QStringLiteral("ConnectionBus")).toString() ==
                        QStringLiteral("usb");
        if (!removable) {
            continue;
        }

        bool filesystemPropertiesValid = false;
        const QVariantMap filesystemProperties = getAllProperties(
                blockPath.path(),
                kFilesystemInterface,
                &filesystemPropertiesValid);
        const bool declaresFilesystem =
                blockProperties.value(
                                       QStringLiteral("IdUsage"))
                        .toString() ==
                QStringLiteral("filesystem");
        if (declaresFilesystem && !filesystemPropertiesValid) {
            // A transient/permission DBus failure is not equivalent to an
            // unmounted filesystem. Fail the whole authoritative inventory.
            return {};
        }
        QStringList mountPoints;
        if (filesystemPropertiesValid) {
            const auto mountPointsProperty =
                    filesystemProperties.constFind(
                            QStringLiteral("MountPoints"));
            bool mountPointsDecoded = false;
            if (mountPointsProperty != filesystemProperties.cend()) {
                mountPoints = decodeByteArrayList(
                        *mountPointsProperty, &mountPointsDecoded);
            }
            if (declaresFilesystem && !mountPointsDecoded) {
                // An undecodable aay is not evidence that the filesystem is
                // unmounted. Refuse the entire authoritative inventory.
                return {};
            }
        }

        const QString uuid = blockProperties
                                     .value(QStringLiteral("IdUUID"))
                                     .toString()
                                     .trimmed()
                                     .toLower();
        const QByteArray rawDevice = blockProperties
                                             .value(QStringLiteral("Device"))
                                             .toByteArray();

        QString stableDriveIdentity = driveProperties
                                              .value(QStringLiteral("WWN"))
                                              .toString()
                                              .trimmed();
        bool stableDriveIdentityReliable = !stableDriveIdentity.isEmpty();
        if (stableDriveIdentity.isEmpty()) {
            stableDriveIdentity = driveProperties
                                          .value(QStringLiteral("Serial"))
                                          .toString()
                                          .trimmed();
            stableDriveIdentityReliable = !stableDriveIdentity.isEmpty();
        }
        if (stableDriveIdentity.isEmpty()) {
            stableDriveIdentity = driveProperties
                                          .value(QStringLiteral("Id"))
                                          .toString()
                                          .trimmed();
            stableDriveIdentityReliable = !stableDriveIdentity.isEmpty();
        }
        if (stableDriveIdentity.isEmpty()) {
            // Last-resort identity for hardware that exposes no serial/WWN.
            // This remains opaque and never leaks an object path to QML.
            stableDriveIdentity = drivePath;
        }
        const QString removalGroupId = opaqueHash(
                QStringLiteral("udisks2-drive:"),
                stableDriveIdentity.toUtf8());

        bool partitionPropertiesValid = false;
        const QVariantMap partitionProperties = getAllProperties(
                blockPath.path(),
                kPartitionInterface,
                &partitionPropertiesValid);
        QString partitionIdentity;
        bool stableBlockIdentityReliable = !uuid.isEmpty();
        if (partitionPropertiesValid &&
                partitionProperties.contains(QStringLiteral("Number"))) {
            partitionIdentity = QStringLiteral("partition:%1")
                                        .arg(partitionProperties
                                                        .value(QStringLiteral("Number"))
                                                        .toUInt());
            stableBlockIdentityReliable = true;
        }
        bool symlinksDecoded = !partitionIdentity.isEmpty();
        if (partitionIdentity.isEmpty() && !uuid.isEmpty()) {
            // A filesystem directly on the whole block device may have no
            // Partition interface or by-id link. Its filesystem UUID is still
            // stable across /dev/sdX re-enumeration, so never mix the transient
            // kernel node into an otherwise durable deviceId.
            partitionIdentity = QStringLiteral("uuid:%1").arg(uuid);
            symlinksDecoded = true;
        }
        if (partitionIdentity.isEmpty()) {
            symlinksDecoded = false;
            QStringList symlinks = decodeByteArrayList(
                    blockProperties.value(QStringLiteral("Symlinks")),
                    &symlinksDecoded);
            std::sort(symlinks.begin(), symlinks.end());
            const auto stableSymlink = std::find_if(symlinks.cbegin(),
                    symlinks.cend(),
                    [](const QString& path) {
                        return path.startsWith(
                                QStringLiteral("/dev/disk/by-id/"));
                    });
            if (stableSymlink != symlinks.cend()) {
                partitionIdentity = *stableSymlink;
                stableBlockIdentityReliable = true;
            }
        }
        if (partitionIdentity.isEmpty()) {
            partitionIdentity = QString::fromLocal8Bit(rawDevice);
        }

        QByteArray deviceIdentitySuffix = uuid.toUtf8();
        deviceIdentitySuffix.append('\0');
        deviceIdentitySuffix.append(partitionIdentity.toUtf8());
        QByteArray blockIdentity = removalGroupId.toUtf8();
        blockIdentity.append('\0');
        blockIdentity.append(deviceIdentitySuffix);

        UDisksDeviceRecord record;
        record.blockObjectPath = blockPath.path();
        record.driveObjectPath = drivePath;
        record.siblingId = siblingId;
        record.deviceIdentitySuffix = deviceIdentitySuffix;
        record.unsafeSibling =
                hintSystem ||
                hintIgnore ||
                !stableDriveIdentityReliable ||
                !stableBlockIdentityReliable ||
                !symlinksDecoded;
        record.exposeDevice = !hintSystem && !hintIgnore;
        record.device.deviceId = opaqueHash(
                QStringLiteral("udisks2-device:"), blockIdentity);
        record.device.removalGroupId = removalGroupId;
        record.device.displayName = blockProperties
                                            .value(QStringLiteral("IdLabel"))
                                            .toString()
                                            .trimmed();
        if (record.device.displayName.isEmpty()) {
            record.device.displayName = QStringLiteral("%1 %2")
                                                .arg(driveProperties
                                                                .value(QStringLiteral("Vendor"))
                                                                .toString()
                                                                .trimmed(),
                                                        driveProperties
                                                                .value(QStringLiteral("Model"))
                                                                .toString()
                                                                .trimmed())
                                                .trimmed();
        }
        if (record.device.displayName.isEmpty()) {
            record.device.displayName = QStringLiteral("Removable media");
        }
        record.device.mountPoints = mountPoints;
        record.canPowerOff = driveProperties
                                     .value(QStringLiteral("CanPowerOff"))
                                     .toBool();
        record.device.canStop = false;
        inventory.records.append(std::move(record));
    }

    // Some low-cost or cloned devices report the same serial/WWN. Never let
    // that merge two physical drives into one removal group.
    QHash<QString, QSet<QString>> drivePathsByBaseGroup;
    for (const auto& record : std::as_const(inventory.records)) {
        drivePathsByBaseGroup[record.device.removalGroupId].insert(
                record.driveObjectPath);
    }
    for (auto& record : inventory.records) {
        const bool ambiguousPhysicalIdentity =
                drivePathsByBaseGroup.value(
                                             record.device.removalGroupId)
                        .size() > 1;
        if (ambiguousPhysicalIdentity) {
            QByteArray disambiguatedGroup =
                    record.device.removalGroupId.toUtf8();
            disambiguatedGroup.append('\0');
            disambiguatedGroup.append(record.driveObjectPath.toUtf8());
            record.device.removalGroupId = opaqueHash(
                    QStringLiteral("udisks2-drive:"),
                    disambiguatedGroup);
            // The rows remain distinct for discovery, but a stable physical
            // target cannot be proven across re-enumeration. Fail closed.
            record.unsafeSibling = true;
        }
        QByteArray finalDeviceIdentity =
                record.device.removalGroupId.toUtf8();
        finalDeviceIdentity.append('\0');
        finalDeviceIdentity.append(record.deviceIdentitySuffix);
        record.device.deviceId = opaqueHash(
                QStringLiteral("udisks2-device:"),
                finalDeviceIdentity);
    }

    // A malformed/cloned inventory must never collapse two rows into one.
    // Use the authoritative block object only as a deterministic tie-breaker;
    // normal identities remain stable across object-path changes.
    QHash<QString, int> identityCounts;
    for (const auto& record : std::as_const(inventory.records)) {
        ++identityCounts[record.device.deviceId];
    }
    for (auto& record : inventory.records) {
        if (identityCounts.value(record.device.deviceId) < 2) {
            continue;
        }
        record.unsafeSibling = true;
        QByteArray disambiguatedIdentity = record.device.deviceId.toUtf8();
        disambiguatedIdentity.append('\0');
        disambiguatedIdentity.append(record.blockObjectPath.toUtf8());
        record.device.deviceId = opaqueHash(
                QStringLiteral("udisks2-device:"),
                disambiguatedIdentity);
    }

    // UDisks Drive.PowerOff may affect every Drive object sharing a non-empty
    // SiblingId (for example, multi-slot USB readers). This adapter does not
    // yet coordinate cross-Drive unmounts, so such power domains are
    // deliberately unsupported.
    QHash<QString, QSet<QString>> drivePathsBySiblingId;
    for (const auto& record : std::as_const(inventory.records)) {
        if (!record.siblingId.isEmpty()) {
            drivePathsBySiblingId[record.siblingId].insert(
                    record.driveObjectPath);
        }
    }
    for (auto& record : inventory.records) {
        if (!record.siblingId.isEmpty() &&
                drivePathsBySiblingId.value(record.siblingId).size() > 1) {
            record.unsafeSibling = true;
        }
    }

    // STOP is a physical-drive operation. A system, ignored, or
    // identity-indeterminate sibling vetoes the entire final removal group,
    // even when that sibling is not exposed as a Rekordbox source row.
    QHash<QString, bool> unsafeRemovalGroups;
    for (const auto& record : std::as_const(inventory.records)) {
        unsafeRemovalGroups[record.device.removalGroupId] =
                unsafeRemovalGroups.value(record.device.removalGroupId) ||
                record.unsafeSibling;
    }
    for (auto& record : inventory.records) {
        record.device.canStop =
                record.canPowerOff &&
                !unsafeRemovalGroups.value(record.device.removalGroupId);
    }
    inventory.valid = true;
    return inventory;
}

SafeStopResult dbusFailure(
        const QString& deviceId,
        const QDBusMessage& reply) {
    const QString name = reply.errorName();
    ErrorCode code = ErrorCode::OsRejected;
    MediaState state = MediaState::Failed;
    if (name.contains(QStringLiteral("DeviceBusy"), Qt::CaseInsensitive)) {
        code = ErrorCode::Busy;
        state = MediaState::Busy;
    } else if (name.contains(QStringLiteral("NotAuthorized"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("AccessDenied"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("Auth"), Qt::CaseInsensitive)) {
        code = ErrorCode::PermissionDenied;
    } else if (name.contains(QStringLiteral("NotSupported"), Qt::CaseInsensitive)) {
        code = ErrorCode::Unsupported;
    } else if (name.contains(QStringLiteral("NoReply"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("Timeout"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("Timedout"), Qt::CaseInsensitive)) {
        code = ErrorCode::TimedOut;
    }

    SafeStopResult result;
    result.deviceId = deviceId;
    result.state = state;
    result.errorCode = code;
    result.errorMessage = reply.errorMessage().isEmpty()
            ? name
            : reply.errorMessage();
    return result;
}

SafeStopResult unexpectedDbusReply(
        const QString& deviceId,
        const QString& operation) {
    SafeStopResult result;
    result.deviceId = deviceId;
    result.state = MediaState::Failed;
    result.errorCode = ErrorCode::IoError;
    result.errorMessage = QStringLiteral(
            "udisks2 returned a non-reply message for %1.")
                                  .arg(operation);
    return result;
}

QDBusMessage callUDisksMethod(
        const QString& objectPath,
        const QString& interfaceName,
        const QString& methodName) {
    QDBusMessage message = QDBusMessage::createMethodCall(
            kUDisksService, objectPath, interfaceName, methodName);
    QVariantMap options;
    // Never trigger an interactive privilege prompt from Mixxx/QML. A narrow
    // broker or polkit rule must grant exactly the required LastCUE action.
    options.insert(QStringLiteral("auth.no_user_interaction"), true);
    message << options;
    return QDBusConnection::systemBus().call(
            message, QDBus::Block, kDbusTimeoutMillis);
}

} // namespace
#endif

UDisks2MediaSnapshot enumerateUDisks2RemovableMedia() {
    UDisks2MediaSnapshot snapshot;
#if defined(__LINUX__)
    const UDisksInventory inventory = enumerateRecords();
    snapshot.valid = inventory.valid;
    if (!inventory.valid) {
        return snapshot;
    }
    snapshot.devices.reserve(inventory.records.size());
    for (const auto& record : inventory.records) {
        snapshot.presentDeviceIds.insert(record.device.deviceId);
        snapshot.presentRemovalGroupIds.insert(
                record.device.removalGroupId);
        if (record.exposeDevice) {
            snapshot.retainableDeviceIds.insert(record.device.deviceId);
            snapshot.retainableDevices.insert(
                    record.device.deviceId, record.device);
        }
        if (record.exposeDevice &&
                !record.device.mountPoints.isEmpty()) {
            snapshot.devices.append(record.device);
        }
    }
#endif
    return snapshot;
}

SafeStopResult requestUDisks2SafeStop(
        const RemovableMediaDevice& device) {
#if defined(__LINUX__)
    const UDisksInventory inventory = enumerateRecords();
    if (!inventory.valid) {
        SafeStopResult result;
        result.deviceId = device.deviceId;
        result.state = MediaState::Failed;
        result.errorCode = ErrorCode::IoError;
        result.errorMessage = QStringLiteral(
                "The authoritative udisks2 inventory is unavailable.");
        return result;
    }
    QList<UDisksDeviceRecord> relatedRecords;
    QSet<QString> drivePaths;
    bool canPowerOff = true;
    bool groupHasUnsafeBlock = false;
    bool selectedDevicePresent = false;
    for (const auto& record : inventory.records) {
        if (record.device.removalGroupId != device.removalGroupId) {
            continue;
        }
        selectedDevicePresent =
                selectedDevicePresent ||
                record.device.deviceId == device.deviceId;
        relatedRecords.append(record);
        drivePaths.insert(record.driveObjectPath);
        canPowerOff = canPowerOff && record.canPowerOff;
        groupHasUnsafeBlock =
                groupHasUnsafeBlock ||
                record.unsafeSibling ||
                !record.device.canStop;
    }

    if (!selectedDevicePresent || relatedRecords.isEmpty()) {
        SafeStopResult result;
        result.deviceId = device.deviceId;
        result.state = MediaState::Failed;
        result.errorCode = ErrorCode::NotFound;
        result.errorMessage = QStringLiteral(
                "The selected udisks2 device is no longer present.");
        return result;
    }
    if (groupHasUnsafeBlock) {
        SafeStopResult result;
        result.deviceId = device.deviceId;
        result.state = MediaState::Failed;
        result.errorCode = ErrorCode::Unsupported;
        result.errorMessage = QStringLiteral(
                "The physical drive contains a system, ignored, or identity-indeterminate block.");
        return result;
    }
    if (!canPowerOff || drivePaths.size() != 1) {
        SafeStopResult result;
        result.deviceId = device.deviceId;
        result.state = MediaState::Failed;
        result.errorCode = ErrorCode::Unsupported;
        result.errorMessage = QStringLiteral(
                "The selected drive is not a unique CanPowerOff target.");
        return result;
    }
    const QString drivePath = *drivePaths.cbegin();

    // All mounted filesystems in the physical removal group must be released
    // before powering off the drive. A failure is reported verbatim; success
    // is never synthesized after a partial or rejected operation.
    for (const auto& record : relatedRecords) {
        if (record.device.mountPoints.isEmpty()) {
            continue;
        }
        const QDBusMessage reply = callUDisksMethod(record.blockObjectPath,
                kFilesystemInterface,
                QStringLiteral("Unmount"));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            return dbusFailure(device.deviceId, reply);
        }
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return unexpectedDbusReply(
                    device.deviceId, QStringLiteral("Unmount"));
        }
    }

    const QDBusMessage powerOffReply = callUDisksMethod(
            drivePath, kDriveInterface, QStringLiteral("PowerOff"));
    if (powerOffReply.type() == QDBusMessage::ErrorMessage) {
        return dbusFailure(device.deviceId, powerOffReply);
    }
    if (powerOffReply.type() != QDBusMessage::ReplyMessage) {
        return unexpectedDbusReply(
                device.deviceId, QStringLiteral("PowerOff"));
    }

    SafeStopResult result;
    result.deviceId = device.deviceId;
    // The service promotes this to SafeToRemove only after a fresh,
    // authoritative inventory confirms that every block in the physical
    // removal group disappeared.
    result.state = MediaState::Quiescing;
    result.errorCode = ErrorCode::None;
    return result;
#else
    SafeStopResult result;
    result.deviceId = device.deviceId;
    result.state = MediaState::Failed;
    result.errorCode = ErrorCode::Unsupported;
    result.errorMessage = QStringLiteral(
            "The udisks2 adapter is available only on LastCUE/Linux.");
    return result;
#endif
}

} // namespace removablemedia
} // namespace mixxx
