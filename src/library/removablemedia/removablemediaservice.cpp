#include "library/removablemedia/removablemediaservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "library/removablemedia/linuxudisks2removablemediaadapter.h"
#include "library/removablemedia/windowsremovablemediaadapter.h"
#include "moc_removablemediaservice.cpp"

namespace mixxx {
namespace removablemedia {
namespace {

constexpr int kTopologyDebounceMillis = 400;
constexpr int kTopologyPollMillis = 4000;
constexpr int kReadinessRetryInitialMillis = 150;
constexpr int kReadinessRetryMaximumMillis = 1000;
constexpr int kReadinessRetryDeadlineMillis = 5000;
constexpr int kSafeStopVerificationPollMillis = 250;
constexpr int kSafeStopVerificationTimeoutMillis = 5000;

struct PlatformMediaSnapshot {
    bool valid{false};
    QList<RemovableMediaDevice> devices;
    QSet<QString> presentDeviceIds;
    QSet<QString> retainableDeviceIds;
    QHash<QString, RemovableMediaDevice> retainableDevices;
    QSet<QString> presentRemovalGroupIds;
};

bool sameTopology(
        const QList<RemovableMediaDevice>& left,
        const QList<RemovableMediaDevice>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (int i = 0; i < left.size(); ++i) {
        const auto& a = left.at(i);
        const auto& b = right.at(i);
        if (a.deviceId != b.deviceId ||
                a.removalGroupId != b.removalGroupId ||
                a.displayName != b.displayName ||
                a.mountPoints != b.mountPoints ||
                a.canStop != b.canStop) {
            return false;
        }
    }
    return true;
}

void sortDevices(QList<RemovableMediaDevice>* pDevices) {
    std::sort(pDevices->begin(),
            pDevices->end(),
            [](const RemovableMediaDevice& left,
                    const RemovableMediaDevice& right) {
                return left.deviceId < right.deviceId;
            });
}

SafeStopResult makeFailure(
        const QString& deviceId,
        ErrorCode errorCode,
        const QString& message) {
    SafeStopResult result;
    result.deviceId = deviceId;
    result.state = errorCode == ErrorCode::Busy
            ? MediaState::Busy
            : MediaState::Failed;
    result.errorCode = errorCode;
    result.errorMessage = message;
    return result;
}

#if !defined(__WINDOWS__) && !defined(__LINUX__)
QList<RemovableMediaDevice> enumeratePortableMediaFallback() {
    QList<RemovableMediaDevice> devices;
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& volume : volumes) {
        if (!volume.isValid() || !volume.isReady()) {
            continue;
        }
        const QString mountPoint = QDir::cleanPath(volume.rootPath());
        if (!(mountPoint.startsWith(QStringLiteral("/media/")) ||
                    mountPoint.startsWith(QStringLiteral("/run/media/")) ||
                    mountPoint.startsWith(QStringLiteral("/Volumes/")))) {
            continue;
        }
        RemovableMediaDevice device;
        const QByteArray identity = QCryptographicHash::hash(
                volume.device() + '\0' + mountPoint.toUtf8(),
                QCryptographicHash::Sha256)
                                            .toHex();
        device.deviceId = QStringLiteral("posix-volume:%1")
                                  .arg(QString::fromLatin1(identity));
        device.removalGroupId = device.deviceId;
        device.displayName = volume.displayName().isEmpty()
                ? QDir(mountPoint).dirName()
                : volume.displayName();
        device.mountPoints = {mountPoint};
        // The future LastCUE adapter delegates to a narrow udisks2 broker.
        // This fallback is intentionally observe-only.
        device.canStop = false;
        devices.append(std::move(device));
    }
    return devices;
}
#endif

class PlatformRemovableMediaService final : public IRemovableMediaService {
  public:
    PlatformRemovableMediaService(
            bool safeStopEnabled,
            QObject* parent)
            : IRemovableMediaService(parent),
              m_safeStopEnabled(safeStopEnabled) {
        s_instance = this;

        m_debounceTimer.setSingleShot(true);
        m_debounceTimer.setInterval(kTopologyDebounceMillis);
        connect(&m_debounceTimer,
                &QTimer::timeout,
                this,
                [this] { startSnapshot(); });

        m_pollTimer.setInterval(kTopologyPollMillis);
        connect(&m_pollTimer,
                &QTimer::timeout,
                this,
                [this] { requestRescan(); });
        m_pollTimer.start();

        connect(&m_snapshotWatcher,
                &QFutureWatcher<PlatformMediaSnapshot>::finished,
                this,
                [this] { applySnapshot(); });
        m_readinessRetryTimer.setSingleShot(true);
        connect(&m_readinessRetryTimer,
                &QTimer::timeout,
                this,
                [this] { startSnapshot(); });
        connect(&m_safeStopWatcher,
                &QFutureWatcher<SafeStopResult>::finished,
                this,
                [this] { handlePlatformSafeStopFinished(); });

        m_verificationTimer.setInterval(kSafeStopVerificationPollMillis);
        connect(&m_verificationTimer,
                &QTimer::timeout,
                this,
                [this] {
                    verifySafeStop(false);
                    if (!m_verifyingDeviceId.isEmpty()) {
                        startSnapshot(true);
                    }
                });

        QTimer::singleShot(0, this, [this] { requestRescan(); });
    }

    ~PlatformRemovableMediaService() override {
        if (s_instance == this) {
            s_instance.clear();
        }
        m_snapshotFuture.waitForFinished();
        m_safeStopFuture.waitForFinished();
    }

    QList<RemovableMediaDevice> devices() const override {
        return m_devices;
    }

    quint64 generation() const override {
        return m_generation;
    }

    void requestRescan() override {
        // Restarting the timer coalesces event storms. Polling and native hints
        // share the same authoritative snapshot path.
        m_debounceTimer.start();
    }

    bool requestSafeStop(
            const QString& deviceId,
            quint64 expectedGeneration) override {
        const auto deviceIt = std::find_if(m_devices.cbegin(),
                m_devices.cend(),
                [&deviceId](const RemovableMediaDevice& device) {
                    return device.deviceId == deviceId;
                });
        if (deviceIt == m_devices.cend()) {
            emitFailure(makeFailure(deviceId,
                    ErrorCode::NotFound,
                    tr("The selected removable device is no longer present.")));
            return false;
        }
        if (expectedGeneration != m_generation ||
                expectedGeneration != deviceIt->generation) {
            emitFailure(makeFailure(deviceId,
                    ErrorCode::StaleGeneration,
                    tr("The removable device list changed. Select the device again.")));
            return false;
        }
        if (!deviceIt->canStop) {
            emitFailure(makeFailure(deviceId,
                    ErrorCode::Unsupported,
                    tr("Safe removal is disabled in the current adapter mode.")));
            return false;
        }
        if (m_snapshotWatcher.isRunning() ||
                m_snapshotApplyPending ||
                m_debounceTimer.isActive() ||
                m_refreshPending ||
                m_readinessRetryTimer.isActive()) {
            emitFailure(makeFailure(deviceId,
                    ErrorCode::Busy,
                    tr("The removable-media inventory is still being reconciled.")));
            return false;
        }
        if (m_safeStopOperationPending ||
                m_safeStopWatcher.isRunning() ||
                !m_verifyingDeviceId.isEmpty()) {
            emitFailure(makeFailure(deviceId,
                    ErrorCode::Busy,
                    tr("Another removable-media operation is in progress.")));
            return false;
        }

#if defined(__LINUX__)
        m_safeStopOperationPending = true;
        m_requestedSafeStopRemovalGroupId = deviceIt->removalGroupId;
        m_safeStopFuture = QtConcurrent::run(requestUDisks2SafeStop, *deviceIt);
        m_safeStopWatcher.setFuture(m_safeStopFuture);
        return true;
#else
        emitFailure(makeFailure(deviceId,
                ErrorCode::Unsupported,
                tr("Safe removal is available only through the LastCUE OS adapter.")));
        return false;
#endif
    }

    void handleNativeTopologyHint() {
        m_readinessBurstElapsed.restart();
        m_readinessRetryDelayMillis = kReadinessRetryInitialMillis;
        m_readinessRetryTimer.stop();
        requestRescan();
    }

    static QPointer<PlatformRemovableMediaService> s_instance;

  private:
    static PlatformMediaSnapshot enumerateSnapshot() {
        PlatformMediaSnapshot snapshot;
#if defined(__WINDOWS__)
        snapshot.devices = enumerateWindowsRemovableMedia();
        snapshot.valid = true;
#elif defined(__LINUX__)
        const UDisks2MediaSnapshot udisksSnapshot =
                enumerateUDisks2RemovableMedia();
        snapshot.valid = udisksSnapshot.valid;
        snapshot.devices = udisksSnapshot.devices;
        snapshot.presentRemovalGroupIds =
                udisksSnapshot.presentRemovalGroupIds;
        snapshot.presentDeviceIds = udisksSnapshot.presentDeviceIds;
        snapshot.retainableDeviceIds =
                udisksSnapshot.retainableDeviceIds;
        snapshot.retainableDevices =
                udisksSnapshot.retainableDevices;
#else
        snapshot.devices = enumeratePortableMediaFallback();
        snapshot.valid = true;
#endif
        if (snapshot.valid && snapshot.presentRemovalGroupIds.isEmpty()) {
            for (const auto& device : std::as_const(snapshot.devices)) {
                snapshot.presentDeviceIds.insert(device.deviceId);
                snapshot.retainableDeviceIds.insert(device.deviceId);
                snapshot.retainableDevices.insert(
                        device.deviceId, device);
                snapshot.presentRemovalGroupIds.insert(
                        device.removalGroupId);
            }
        }
        return snapshot;
    }

    void startSnapshot(bool operationVerification = false) {
        if (!operationVerification &&
                m_safeStopOperationPending) {
            m_refreshPending = true;
            return;
        }
        if (m_snapshotWatcher.isRunning() || m_snapshotApplyPending) {
            m_refreshPending = true;
            return;
        }
        // QFutureWatcher::isRunning() may become false before its queued
        // finished slot applies the result. This explicit ownership flag
        // closes that generation/admission window.
        m_snapshotApplyPending = true;
        m_snapshotFuture = QtConcurrent::run(enumerateSnapshot);
        m_snapshotWatcher.setFuture(m_snapshotFuture);
    }

    void applySnapshot() {
        PlatformMediaSnapshot snapshot = m_snapshotFuture.result();
        if (!snapshot.valid) {
            // An unavailable system bus or failed UDisks inventory cannot
            // prove either arrival or removal. Preserve the last known model.
            scheduleReadinessRetry();
            verifySafeStop(false);
            m_snapshotApplyPending = false;
            flushPendingRefreshAfterOperation();
            return;
        }

        QList<RemovableMediaDevice> devices = std::move(snapshot.devices);
        for (const auto& oldDevice : std::as_const(m_devices)) {
            const auto authoritativeDevice =
                    snapshot.retainableDevices.constFind(
                            oldDevice.deviceId);
            if (!snapshot.presentDeviceIds.contains(oldDevice.deviceId) ||
                    !snapshot.retainableDeviceIds.contains(
                            oldDevice.deviceId) ||
                    authoritativeDevice ==
                            snapshot.retainableDevices.cend() ||
                    std::any_of(devices.cbegin(),
                            devices.cend(),
                            [&oldDevice](const RemovableMediaDevice& device) {
                                return device.deviceId == oldDevice.deviceId;
                            })) {
                continue;
            }
            // Preserve a retryable row after a partial STOP unmounted the
            // filesystem but the authoritative block inventory still sees it.
            RemovableMediaDevice unmountedDevice =
                    authoritativeDevice.value();
            unmountedDevice.mountPoints.clear();
            devices.append(std::move(unmountedDevice));
        }
        sortDevices(&devices);

        for (auto& device : devices) {
            // Discovery and authority are separate. Only the explicit
            // linux-udisks2 mode may expose STOP capability.
            device.canStop = m_safeStopEnabled && device.canStop;
        }

        if (!sameTopology(m_devices, devices)) {
            ++m_generation;
            for (auto& device : devices) {
                device.generation = m_generation;
            }
            m_devices = std::move(devices);
            emit devicesChanged();
        }
        m_presentRemovalGroupIds =
                std::move(snapshot.presentRemovalGroupIds);

        scheduleReadinessRetry();
        verifySafeStop(true);
        m_snapshotApplyPending = false;
        flushPendingRefreshAfterOperation();
    }

    void scheduleReadinessRetry() {
        if (!m_readinessBurstElapsed.isValid() ||
                m_readinessBurstElapsed.elapsed() >=
                        kReadinessRetryDeadlineMillis) {
            m_readinessRetryTimer.stop();
            return;
        }
        if (!m_readinessRetryTimer.isActive()) {
            m_readinessRetryTimer.start(m_readinessRetryDelayMillis);
            m_readinessRetryDelayMillis = std::min(
                    kReadinessRetryMaximumMillis,
                    m_readinessRetryDelayMillis * 2);
        }
    }

    void handlePlatformSafeStopFinished() {
        const SafeStopResult result = m_safeStopFuture.result();
        m_verifyingDeviceId = result.deviceId;
        m_verifyingRemovalGroupId = m_requestedSafeStopRemovalGroupId;
        m_requestedSafeStopRemovalGroupId.clear();
        m_verificationElapsed.start();
        m_verificationSawAuthoritativeSnapshot = false;
        m_verificationTimer.start();
        if (result.errorCode != ErrorCode::None) {
            // An Unmount sequence may have partially succeeded before a
            // later partition or PowerOff failed. Hold LOAD gates until one
            // authoritative inventory has reconciled that partial state.
            m_pendingPlatformFailure = result;
            m_failureReconciliationPending = true;
        } else {
            m_failureReconciliationPending = false;
        }
        startSnapshot(true);
    }

    void verifySafeStop(bool authoritativeSnapshot) {
        if (m_verifyingDeviceId.isEmpty()) {
            return;
        }
        if (authoritativeSnapshot) {
            m_verificationSawAuthoritativeSnapshot = true;
        }
        if (authoritativeSnapshot && m_failureReconciliationPending) {
            const SafeStopResult result = m_pendingPlatformFailure;
            m_verifyingDeviceId.clear();
            m_verifyingRemovalGroupId.clear();
            m_failureReconciliationPending = false;
            m_verificationSawAuthoritativeSnapshot = false;
            m_safeStopOperationPending = false;
            m_verificationTimer.stop();
            emit safeStopFinished(result);
            flushPendingRefreshAfterOperation();
            return;
        }
        const bool stillPresent = m_presentRemovalGroupIds.contains(
                m_verifyingRemovalGroupId);
        if (authoritativeSnapshot && !stillPresent) {
            SafeStopResult result;
            result.deviceId = m_verifyingDeviceId;
            result.state = MediaState::SafeToRemove;
            result.errorCode = ErrorCode::None;
            m_verifyingDeviceId.clear();
            m_verifyingRemovalGroupId.clear();
            m_verificationSawAuthoritativeSnapshot = false;
            m_safeStopOperationPending = false;
            m_verificationTimer.stop();
            emit safeStopFinished(result);
            flushPendingRefreshAfterOperation();
            return;
        }
        if (m_verificationElapsed.isValid() &&
                m_verificationElapsed.elapsed() >=
                        kSafeStopVerificationTimeoutMillis) {
            if (!m_verificationSawAuthoritativeSnapshot) {
                // A platform failure may have followed one or more successful
                // Unmount calls, and even a successful PowerOff reply is not
                // authoritative topology evidence. Keep the operation and all
                // LOAD gates owned until at least one valid post-operation
                // inventory can reconcile the physical removal group.
                return;
            }
            const QString deviceId = m_verifyingDeviceId;
            SafeStopResult result = makeFailure(deviceId,
                    ErrorCode::TimedOut,
                    tr("The operating system accepted the stop request, but an authoritative inventory did not confirm removal."));
            m_verifyingDeviceId.clear();
            m_verifyingRemovalGroupId.clear();
            m_failureReconciliationPending = false;
            m_verificationSawAuthoritativeSnapshot = false;
            m_safeStopOperationPending = false;
            m_verificationTimer.stop();
            emit safeStopFinished(result);
            flushPendingRefreshAfterOperation();
            return;
        }
    }

    void flushPendingRefreshAfterOperation() {
        if (m_refreshPending &&
                !m_snapshotApplyPending &&
                !m_safeStopOperationPending) {
            m_refreshPending = false;
            requestRescan();
        }
    }

    void emitFailure(SafeStopResult result) {
        QTimer::singleShot(0,
                this,
                [this, result = std::move(result)] {
                    emit safeStopFinished(result);
                });
    }

    QList<RemovableMediaDevice> m_devices;
    quint64 m_generation{0};
    QTimer m_debounceTimer;
    QTimer m_pollTimer;
    QTimer m_readinessRetryTimer;
    QTimer m_verificationTimer;
    QFuture<PlatformMediaSnapshot> m_snapshotFuture;
    QFutureWatcher<PlatformMediaSnapshot> m_snapshotWatcher;
    QFuture<SafeStopResult> m_safeStopFuture;
    QFutureWatcher<SafeStopResult> m_safeStopWatcher;
    QElapsedTimer m_verificationElapsed;
    QElapsedTimer m_readinessBurstElapsed;
    int m_readinessRetryDelayMillis{kReadinessRetryInitialMillis};
    QString m_verifyingDeviceId;
    QString m_verifyingRemovalGroupId;
    QString m_requestedSafeStopRemovalGroupId;
    QSet<QString> m_presentRemovalGroupIds;
    SafeStopResult m_pendingPlatformFailure;
    bool m_failureReconciliationPending{false};
    bool m_verificationSawAuthoritativeSnapshot{false};
    bool m_refreshPending{false};
    bool m_snapshotApplyPending{false};
    bool m_safeStopOperationPending{false};
    const bool m_safeStopEnabled;
};

QPointer<PlatformRemovableMediaService> PlatformRemovableMediaService::s_instance;

QList<RemovableMediaDevice> fakeDevicesFromEnvironment() {
    QList<RemovableMediaDevice> devices;
    const QStringList roots = QString::fromLocal8Bit(
            qgetenv("LASTCUE_FAKE_MEDIA_ROOTS"))
                                      .split(QDir::listSeparator(),
                                              Qt::SkipEmptyParts);
    for (const QString& root : roots) {
        const QString normalizedRoot = QDir::cleanPath(root.trimmed());
        if (normalizedRoot.isEmpty()) {
            continue;
        }
        const QByteArray digest = QCryptographicHash::hash(
                normalizedRoot.toUtf8(), QCryptographicHash::Sha256)
                                          .toHex();
        RemovableMediaDevice device;
        device.deviceId = QStringLiteral("fake:%1")
                                  .arg(QString::fromLatin1(digest));
        device.removalGroupId = device.deviceId;
        device.displayName = QDir(normalizedRoot).dirName();
        if (device.displayName.isEmpty()) {
            device.displayName = QStringLiteral("Fake removable media");
        }
        device.mountPoints = {normalizedRoot};
        device.canStop = true;
        devices.append(std::move(device));
    }
    return devices;
}

} // namespace

std::unique_ptr<IRemovableMediaService> createRemovableMediaService(
        QObject* parent) {
    const QString mode = QString::fromLocal8Bit(
            qgetenv("LASTCUE_REMOVABLE_MEDIA_MODE"))
                                 .trimmed()
                                 .toLower();
    if (mode == QStringLiteral("fake")) {
        return std::make_unique<FakeRemovableMediaService>(
                fakeDevicesFromEnvironment(), parent);
    }
    bool safeStopEnabled = false;
    bool modeAvailable = mode.isEmpty() ||
            mode == QStringLiteral("observe-only");
#if defined(__LINUX__)
    if (mode == QStringLiteral("linux-udisks2")) {
        safeStopEnabled = true;
        modeAvailable = true;
    }
#endif
    if (!modeAvailable) {
        qWarning() << "Unknown or unavailable removable-media adapter mode"
                   << mode << "- using observe-only";
    }
    return std::make_unique<PlatformRemovableMediaService>(
            safeStopEnabled, parent);
}

void notifyNativeTopologyChange() {
    if (PlatformRemovableMediaService::s_instance) {
        QMetaObject::invokeMethod(PlatformRemovableMediaService::s_instance, [] {
                    if (PlatformRemovableMediaService::s_instance) {
                        PlatformRemovableMediaService::s_instance
                                ->handleNativeTopologyHint();
                    } }, Qt::QueuedConnection);
    }
}

FakeRemovableMediaService::FakeRemovableMediaService(QObject* parent)
        : IRemovableMediaService(parent) {
}

FakeRemovableMediaService::FakeRemovableMediaService(
        QList<RemovableMediaDevice> devices,
        QObject* parent)
        : IRemovableMediaService(parent) {
    replaceDevices(std::move(devices));
}

QList<RemovableMediaDevice> FakeRemovableMediaService::devices() const {
    return m_devices;
}

quint64 FakeRemovableMediaService::generation() const {
    return m_generation;
}

void FakeRemovableMediaService::requestRescan() {
    QTimer::singleShot(0, this, [this] { emit devicesChanged(); });
}

bool FakeRemovableMediaService::requestSafeStop(
        const QString& deviceId,
        quint64 expectedGeneration) {
    const auto deviceIt = std::find_if(m_devices.cbegin(),
            m_devices.cend(),
            [&deviceId](const RemovableMediaDevice& device) {
                return device.deviceId == deviceId;
            });
    if (deviceIt == m_devices.cend()) {
        const SafeStopResult result = makeFailure(deviceId,
                ErrorCode::NotFound,
                tr("The fake removable device is no longer present."));
        QTimer::singleShot(0, this, [this, result] {
            emit safeStopFinished(result);
        });
        return false;
    }
    if (expectedGeneration != m_generation ||
            expectedGeneration != deviceIt->generation) {
        const SafeStopResult result = makeFailure(deviceId,
                ErrorCode::StaleGeneration,
                tr("The fake removable device generation is stale."));
        QTimer::singleShot(0, this, [this, result] {
            emit safeStopFinished(result);
        });
        return false;
    }
    if (!deviceIt->canStop) {
        const SafeStopResult result = makeFailure(deviceId,
                ErrorCode::Unsupported,
                tr("Safe removal is disabled for this fake device."));
        QTimer::singleShot(0, this, [this, result] {
            emit safeStopFinished(result);
        });
        return false;
    }
    if (m_safeStopFailure != ErrorCode::None) {
        const SafeStopResult result = makeFailure(
                deviceId, m_safeStopFailure, m_safeStopFailureMessage);
        QTimer::singleShot(0, this, [this, result] {
            emit safeStopFinished(result);
        });
        return true;
    }

    SafeStopResult result;
    result.deviceId = deviceId;
    result.state = MediaState::SafeToRemove;
    result.errorCode = ErrorCode::None;
    const QString removalGroupId = deviceIt->removalGroupId;
    QTimer::singleShot(0, this, [this, removalGroupId, result] {
        QList<RemovableMediaDevice> remaining;
        for (const auto& device : std::as_const(m_devices)) {
            if (device.removalGroupId != removalGroupId) {
                remaining.append(device);
            }
        }
        replaceDevices(std::move(remaining));
        emit devicesChanged();
        emit safeStopFinished(result);
    });
    return true;
}

void FakeRemovableMediaService::setDevices(
        QList<RemovableMediaDevice> devices) {
    sortDevices(&devices);
    if (sameTopology(m_devices, devices)) {
        return;
    }
    replaceDevices(std::move(devices));
    emit devicesChanged();
}

void FakeRemovableMediaService::setSafeStopFailure(
        ErrorCode errorCode,
        QString errorMessage) {
    m_safeStopFailure = errorCode;
    m_safeStopFailureMessage = std::move(errorMessage);
}

void FakeRemovableMediaService::replaceDevices(
        QList<RemovableMediaDevice> devices) {
    ++m_generation;
    for (auto& device : devices) {
        device.generation = m_generation;
    }
    m_devices = std::move(devices);
}

} // namespace removablemedia
} // namespace mixxx
