#pragma once

#include <memory>

#include "engine/bufferscalers/enginebufferscale.h"
#include "util/samplebuffer.h"

class ReadAheadManager;
class TransientWsola;

// Keylock scaler that keeps drum attacks intact by placing its splices
// away from detected onsets. See TransientWsola.
class EngineBufferScaleTransient : public EngineBufferScale {
    Q_OBJECT
  public:
    explicit EngineBufferScaleTransient(
            ReadAheadManager* pReadAheadManager);
    ~EngineBufferScaleTransient() override;

    void setScaleParameters(double base_rate,
            double* pTempoRatio,
            double* pPitchRatio) override;

    double scaleBuffer(
            CSAMPLE* pOutputBuffer,
            SINT iOutputBufferSize) override;

    void clear() override;

  private:
    void onSampleRateChanged() override;

    ReadAheadManager* m_pReadAheadManager;
    std::unique_ptr<TransientWsola> m_pStretch;
    mixxx::SampleBuffer m_bufferBack;
    bool m_bBackwards;
};
