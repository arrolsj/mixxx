#pragma once

#include <QHash>
#include <QSet>

#include "library/removablemedia/removablemediadevice.h"

namespace mixxx {
namespace removablemedia {

struct UDisks2MediaSnapshot {
    bool valid{false};
    QList<RemovableMediaDevice> devices;
    QSet<QString> presentDeviceIds;
    QSet<QString> retainableDeviceIds;
    QHash<QString, RemovableMediaDevice> retainableDevices;
    QSet<QString> presentRemovalGroupIds;
};

UDisks2MediaSnapshot enumerateUDisks2RemovableMedia();
SafeStopResult requestUDisks2SafeStop(const RemovableMediaDevice& device);

} // namespace removablemedia
} // namespace mixxx
