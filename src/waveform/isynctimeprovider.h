#pragma once

#include "util/performancetimer.h"

// Supplies the display timing used to project an audio-engine position onto
// the frame that is about to become visible. QWidget and QML render paths must
// use this same contract so they cannot drift onto different clock sources.
class VSyncTimeProvider {
  public:
    virtual ~VSyncTimeProvider() = default;

    virtual int fromTimerToNextSyncMicros(const PerformanceTimer& timer) = 0;
    virtual int getSyncIntervalTimeMicros() const = 0;
    virtual const char* timingSourceName() const = 0;
};
