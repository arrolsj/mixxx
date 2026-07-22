#include "library/removablemedia/windowsremovablemediaadapter.h"

#if defined(__WINDOWS__)
// clang-format off
#include <windows.h>
#include <winioctl.h>
// clang-format on
#endif

#include <QDir>
#include <QStorageInfo>
#include <utility>

namespace mixxx {
namespace removablemedia {

#if defined(__WINDOWS__)
namespace {

struct WindowsStorageIdentity {
    bool isUsb{false};
    bool removableMedia{false};
    DWORD deviceNumber{MAXDWORD};
};

QString nativeVolumeRoot(const QString& rootPath) {
    QString root = QDir::toNativeSeparators(rootPath);
    if (!root.endsWith(QLatin1Char('\\'))) {
        root.append(QLatin1Char('\\'));
    }
    return root;
}

QString volumeHandlePath(const QString& nativeRoot) {
    if (nativeRoot.size() < 2 || nativeRoot.at(1) != QLatin1Char(':')) {
        return {};
    }
    return QStringLiteral("\\\\.\\") + nativeRoot.left(2);
}

WindowsStorageIdentity queryStorageIdentity(const QString& nativeRoot) {
    WindowsStorageIdentity identity;
    const QString handlePath = volumeHandlePath(nativeRoot);
    if (handlePath.isEmpty()) {
        return identity;
    }

    const HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(handlePath.utf16()),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return identity;
    }

    STORAGE_DEVICE_NUMBER number{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(handle,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &number,
                sizeof(number),
                &bytesReturned,
                nullptr)) {
        identity.deviceNumber = number.DeviceNumber;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE descriptorBuffer[1024]{};
    if (DeviceIoControl(handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                descriptorBuffer,
                sizeof(descriptorBuffer),
                &bytesReturned,
                nullptr) &&
            bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(
                descriptorBuffer);
        identity.isUsb = descriptor->BusType == BusTypeUsb;
        identity.removableMedia = descriptor->RemovableMedia != FALSE;
    }

    CloseHandle(handle);
    return identity;
}

QString volumeGuid(const QString& nativeRoot) {
    WCHAR buffer[128]{};
    if (!GetVolumeNameForVolumeMountPointW(
                reinterpret_cast<LPCWSTR>(nativeRoot.utf16()),
                buffer,
                static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])))) {
        return {};
    }
    QString value = QString::fromWCharArray(buffer).toLower();
    while (value.endsWith(QLatin1Char('\\'))) {
        value.chop(1);
    }
    return value;
}

} // namespace
#endif

QList<RemovableMediaDevice> enumerateWindowsRemovableMedia() {
    QList<RemovableMediaDevice> devices;
#if defined(__WINDOWS__)
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& volume : volumes) {
        if (!volume.isValid() || !volume.isReady()) {
            continue;
        }

        const QString nativeRoot = nativeVolumeRoot(volume.rootPath());
        const UINT driveType = GetDriveTypeW(
                reinterpret_cast<LPCWSTR>(nativeRoot.utf16()));
        if (driveType != DRIVE_REMOVABLE && driveType != DRIVE_FIXED) {
            continue;
        }

        const WindowsStorageIdentity storage = queryStorageIdentity(nativeRoot);
        if (driveType != DRIVE_REMOVABLE &&
                !storage.isUsb &&
                !storage.removableMedia) {
            continue;
        }

        const QString guid = volumeGuid(nativeRoot);
        if (guid.isEmpty()) {
            // DeviceNumber is only a snapshot-scoped physical grouping hint;
            // it may change after re-enumeration. Without the persistent
            // volume GUID there is no stable selection identity, so fail
            // closed instead of exposing a misleading deviceId.
            continue;
        }

        RemovableMediaDevice device;
        device.deviceId = QStringLiteral("win-volume:%1").arg(guid);
        device.removalGroupId = storage.deviceNumber == MAXDWORD
                ? device.deviceId
                : QStringLiteral("win-storage-device:%1")
                          .arg(storage.deviceNumber);
        device.displayName = volume.displayName().trimmed();
        if (device.displayName.isEmpty()) {
            device.displayName = QStringLiteral("Removable media");
        }
        device.mountPoints = {QDir::cleanPath(volume.rootPath())};
        // Windows is discovery-only by design. No code path in this adapter
        // performs an eject, dismount or privilege elevation.
        device.canStop = false;
        devices.append(std::move(device));
    }
#endif
    return devices;
}

} // namespace removablemedia
} // namespace mixxx
