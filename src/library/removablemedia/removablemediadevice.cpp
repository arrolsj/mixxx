#include "library/removablemedia/removablemediadevice.h"

namespace mixxx {
namespace removablemedia {

bool RemovableMediaDevice::operator==(const RemovableMediaDevice& other) const {
    return deviceId == other.deviceId &&
            removalGroupId == other.removalGroupId &&
            displayName == other.displayName &&
            mountPoints == other.mountPoints &&
            generation == other.generation &&
            state == other.state &&
            canStop == other.canStop &&
            busyReason == other.busyReason &&
            errorCode == other.errorCode &&
            errorMessage == other.errorMessage;
}

QString mediaStateToString(MediaState state) {
    switch (state) {
    case MediaState::Ready:
        return QStringLiteral("ready");
    case MediaState::Scanning:
        return QStringLiteral("scanning");
    case MediaState::Quiescing:
        return QStringLiteral("quiescing");
    case MediaState::Busy:
        return QStringLiteral("busy");
    case MediaState::Failed:
        return QStringLiteral("failed");
    case MediaState::SafeToRemove:
        return QStringLiteral("safeToRemove");
    }
    return QStringLiteral("failed");
}

QString errorCodeToString(ErrorCode errorCode) {
    switch (errorCode) {
    case ErrorCode::None:
        return QString();
    case ErrorCode::NotFound:
        return QStringLiteral("not-found");
    case ErrorCode::StaleGeneration:
        return QStringLiteral("stale-generation");
    case ErrorCode::Busy:
        return QStringLiteral("busy");
    case ErrorCode::PermissionDenied:
        return QStringLiteral("not-authorized");
    case ErrorCode::Unsupported:
        return QStringLiteral("not-supported");
    case ErrorCode::TimedOut:
        return QStringLiteral("timeout");
    case ErrorCode::OsRejected:
        return QStringLiteral("system-veto");
    case ErrorCode::IoError:
        return QStringLiteral("io-error");
    case ErrorCode::InvalidDatabase:
        return QStringLiteral("invalid-database");
    }
    return QStringLiteral("io-error");
}

} // namespace removablemedia
} // namespace mixxx
