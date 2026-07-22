#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace mixxx {
namespace removablemedia {

enum class MediaState {
    Ready,
    Scanning,
    Quiescing,
    Busy,
    Failed,
    SafeToRemove,
};

enum class ErrorCode {
    None,
    NotFound,
    StaleGeneration,
    Busy,
    PermissionDenied,
    Unsupported,
    TimedOut,
    OsRejected,
    IoError,
    InvalidDatabase,
};

struct RemovableMediaDevice {
    // Opaque identities. Neither value is a drive letter or mount point.
    QString deviceId;
    QString removalGroupId;
    QString displayName;

    // Platform locators are deliberately kept below the QML boundary.
    QStringList mountPoints;

    quint64 generation{0};
    MediaState state{MediaState::Ready};
    bool canStop{false};
    QString busyReason;
    ErrorCode errorCode{ErrorCode::None};
    QString errorMessage;

    bool operator==(const RemovableMediaDevice& other) const;
};

struct SafeStopResult {
    QString deviceId;
    MediaState state{MediaState::Failed};
    ErrorCode errorCode{ErrorCode::None};
    QString errorMessage;
};

QString mediaStateToString(MediaState state);
QString errorCodeToString(ErrorCode errorCode);

} // namespace removablemedia
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::removablemedia::RemovableMediaDevice)
Q_DECLARE_METATYPE(QList<mixxx::removablemedia::RemovableMediaDevice>)
Q_DECLARE_METATYPE(mixxx::removablemedia::SafeStopResult)
