#pragma once

#include <QObject>
#include <memory>

#include "library/removablemedia/removablemediadevice.h"

namespace mixxx {
namespace removablemedia {

class IRemovableMediaService : public QObject {
    Q_OBJECT

  public:
    explicit IRemovableMediaService(QObject* parent = nullptr)
            : QObject(parent) {
    }
    ~IRemovableMediaService() override = default;

    virtual QList<RemovableMediaDevice> devices() const = 0;
    virtual quint64 generation() const = 0;
    virtual void requestRescan() = 0;
    virtual bool requestSafeStop(
            const QString& deviceId,
            quint64 expectedGeneration) = 0;

  signals:
    void devicesChanged();
    void safeStopFinished(
            const mixxx::removablemedia::SafeStopResult& result);
};

// Windows is observe-only. LastCUE/Linux safe removal uses udisks2.
std::unique_ptr<IRemovableMediaService> createRemovableMediaService(
        QObject* parent = nullptr);

// WM_DEVICECHANGE is only a low-latency hint. The service always reconciles an
// authoritative filesystem snapshot after its debounce interval.
void notifyNativeTopologyChange();

class FakeRemovableMediaService final : public IRemovableMediaService {
  public:
    explicit FakeRemovableMediaService(QObject* parent = nullptr);
    FakeRemovableMediaService(
            QList<RemovableMediaDevice> devices,
            QObject* parent = nullptr);

    QList<RemovableMediaDevice> devices() const override;
    quint64 generation() const override;
    void requestRescan() override;
    bool requestSafeStop(
            const QString& deviceId,
            quint64 expectedGeneration) override;

    void setDevices(QList<RemovableMediaDevice> devices);
    void setSafeStopFailure(ErrorCode errorCode, QString errorMessage);

  private:
    void replaceDevices(QList<RemovableMediaDevice> devices);

    QList<RemovableMediaDevice> m_devices;
    quint64 m_generation{0};
    ErrorCode m_safeStopFailure{ErrorCode::None};
    QString m_safeStopFailureMessage;
};

} // namespace removablemedia
} // namespace mixxx
