#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QStringList>

#include "library/removablemedia/removablemediaservice.h"

namespace {

using mixxx::removablemedia::ErrorCode;
using mixxx::removablemedia::FakeRemovableMediaService;
using mixxx::removablemedia::MediaState;
using mixxx::removablemedia::RemovableMediaDevice;
using mixxx::removablemedia::SafeStopResult;

RemovableMediaDevice makeDevice(
        const QString& deviceId,
        const QString& removalGroupId,
        bool canStop = true) {
    RemovableMediaDevice device;
    device.deviceId = deviceId;
    device.removalGroupId = removalGroupId;
    device.displayName = deviceId;
    device.mountPoints = {QStringLiteral("/fake/") + deviceId};
    device.canStop = canStop;
    return device;
}

void processQueuedSignals() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

TEST(RemovableMediaServiceTest, SafeStopRemovesWholeGroupBeforeSuccess) {
    FakeRemovableMediaService service(
            {makeDevice(QStringLiteral("partition-a"), QStringLiteral("drive-1")),
                    makeDevice(QStringLiteral("partition-b"), QStringLiteral("drive-1")),
                    makeDevice(QStringLiteral("partition-c"), QStringLiteral("drive-2"))});
    const quint64 generation = service.generation();
    QStringList eventOrder;
    SafeStopResult terminalResult;

    QObject::connect(&service,
            &FakeRemovableMediaService::devicesChanged,
            [&] {
                eventOrder.append(QStringLiteral("devices"));
                EXPECT_EQ(service.devices().size(), 1);
                EXPECT_EQ(service.devices().constFirst().deviceId,
                        QStringLiteral("partition-c"));
            });
    QObject::connect(&service,
            &FakeRemovableMediaService::safeStopFinished,
            [&](const SafeStopResult& result) {
                eventOrder.append(QStringLiteral("safe"));
                terminalResult = result;
            });

    EXPECT_TRUE(service.requestSafeStop(
            QStringLiteral("partition-a"), generation));
    processQueuedSignals();

    EXPECT_EQ(eventOrder,
            QStringList({QStringLiteral("devices"), QStringLiteral("safe")}));
    EXPECT_GT(service.generation(), generation);
    EXPECT_EQ(terminalResult.state, MediaState::SafeToRemove);
    EXPECT_EQ(terminalResult.errorCode, ErrorCode::None);
}

TEST(RemovableMediaServiceTest, StaleGenerationFailsWithoutMutation) {
    FakeRemovableMediaService service(
            {makeDevice(QStringLiteral("partition-a"), QStringLiteral("drive-1"))});
    const quint64 generation = service.generation();
    SafeStopResult terminalResult;
    int deviceChangeCount = 0;
    QObject::connect(&service,
            &FakeRemovableMediaService::devicesChanged,
            [&] { ++deviceChangeCount; });
    QObject::connect(&service,
            &FakeRemovableMediaService::safeStopFinished,
            [&](const SafeStopResult& result) { terminalResult = result; });

    EXPECT_FALSE(service.requestSafeStop(
            QStringLiteral("partition-a"), generation + 1));
    processQueuedSignals();

    EXPECT_EQ(deviceChangeCount, 0);
    EXPECT_EQ(service.devices().size(), 1);
    EXPECT_EQ(terminalResult.state, MediaState::Failed);
    EXPECT_EQ(terminalResult.errorCode, ErrorCode::StaleGeneration);
}

TEST(RemovableMediaServiceTest, BusyFailureKeepsDevicePresent) {
    FakeRemovableMediaService service(
            {makeDevice(QStringLiteral("partition-a"), QStringLiteral("drive-1"))});
    service.setSafeStopFailure(
            ErrorCode::Busy, QStringLiteral("test busy"));
    SafeStopResult terminalResult;
    QObject::connect(&service,
            &FakeRemovableMediaService::safeStopFinished,
            [&](const SafeStopResult& result) { terminalResult = result; });

    EXPECT_TRUE(service.requestSafeStop(
            QStringLiteral("partition-a"), service.generation()));
    processQueuedSignals();

    EXPECT_EQ(service.devices().size(), 1);
    EXPECT_EQ(terminalResult.state, MediaState::Busy);
    EXPECT_EQ(terminalResult.errorCode, ErrorCode::Busy);
    EXPECT_EQ(terminalResult.errorMessage, QStringLiteral("test busy"));
}

TEST(RemovableMediaServiceTest, UnsupportedCapabilityFailsWithoutMutation) {
    FakeRemovableMediaService service(
            {makeDevice(QStringLiteral("partition-a"),
                    QStringLiteral("drive-1"),
                    false)});
    const quint64 generation = service.generation();
    SafeStopResult terminalResult;
    int deviceChangeCount = 0;
    QObject::connect(&service,
            &FakeRemovableMediaService::devicesChanged,
            [&] { ++deviceChangeCount; });
    QObject::connect(&service,
            &FakeRemovableMediaService::safeStopFinished,
            [&](const SafeStopResult& result) { terminalResult = result; });

    ASSERT_EQ(service.devices().size(), 1);
    EXPECT_FALSE(service.devices().constFirst().canStop);
    EXPECT_FALSE(service.requestSafeStop(
            QStringLiteral("partition-a"), generation));
    processQueuedSignals();

    EXPECT_EQ(deviceChangeCount, 0);
    ASSERT_EQ(service.devices().size(), 1);
    EXPECT_EQ(service.devices().constFirst().deviceId,
            QStringLiteral("partition-a"));
    EXPECT_EQ(terminalResult.state, MediaState::Failed);
    EXPECT_EQ(terminalResult.errorCode, ErrorCode::Unsupported);
}

TEST(RemovableMediaServiceTest, PublicStateAndErrorTokensAreStable) {
    using mixxx::removablemedia::errorCodeToString;
    using mixxx::removablemedia::mediaStateToString;

    EXPECT_EQ(mediaStateToString(MediaState::SafeToRemove),
            QStringLiteral("safeToRemove"));
    EXPECT_EQ(errorCodeToString(ErrorCode::None), QString());
    EXPECT_EQ(errorCodeToString(ErrorCode::NotFound),
            QStringLiteral("not-found"));
    EXPECT_EQ(errorCodeToString(ErrorCode::PermissionDenied),
            QStringLiteral("not-authorized"));
    EXPECT_EQ(errorCodeToString(ErrorCode::Unsupported),
            QStringLiteral("not-supported"));
    EXPECT_EQ(errorCodeToString(ErrorCode::TimedOut),
            QStringLiteral("timeout"));
    EXPECT_EQ(errorCodeToString(ErrorCode::OsRejected),
            QStringLiteral("system-veto"));
}

} // namespace
