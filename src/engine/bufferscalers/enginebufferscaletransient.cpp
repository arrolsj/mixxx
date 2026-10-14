#include "engine/bufferscalers/enginebufferscaletransient.h"

#include <QtDebug>

#include "engine/bufferscalers/transientwsola.h"
#include "engine/readaheadmanager.h"
#include "moc_enginebufferscaletransient.cpp"
#include "util/math.h"
#include "util/sample.h"

namespace {

constexpr SINT kBackBufferSize = 1024;
constexpr int kDefaultSampleRate = 44100;

} // namespace

EngineBufferScaleTransient::EngineBufferScaleTransient(ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_pStretch(std::make_unique<TransientWsola>(kDefaultSampleRate)),
          m_bufferBack(kBackBufferSize),
          m_bBackwards(false) {
}

EngineBufferScaleTransient::~EngineBufferScaleTransient() = default;

void EngineBufferScaleTransient::setScaleParameters(double base_rate,
        double* pTempoRatio,
        double* pPitchRatio) {
    // Negative speed means we are going backwards. Pitch does not affect
    // the playback direction.
    m_bBackwards = *pTempoRatio < 0;

    double speed_abs = fabs(*pTempoRatio);
    if (speed_abs > MAX_SEEK_SPEED) {
        speed_abs = MAX_SEEK_SPEED;
    } else if (speed_abs < MIN_SEEK_SPEED) {
        speed_abs = 0;
    }
    // Let the caller know if we clamped their value.
    *pTempoRatio = m_bBackwards ? -speed_abs : speed_abs;

    const double pitch = fabs(*pPitchRatio);
    if (pitch > 0.0) {
        // Stretch by tempo/pitch, then resample by base_rate * pitch: the
        // resampling raises the pitch and also speeds up by the same factor.
        m_pStretch->setTempo(speed_abs / pitch);
        m_pStretch->setRate(base_rate * pitch);
    }
    m_dBaseRate = base_rate;
    m_dTempoRatio = speed_abs;
    m_dPitchRatio = *pPitchRatio;
}

void EngineBufferScaleTransient::onSampleRateChanged() {
    m_bufferBack.clear();
    if (!getOutputSignal().isValid()) {
        return;
    }
    m_pStretch = std::make_unique<TransientWsola>(getOutputSignal().getSampleRate());
    const double pitch = fabs(m_dPitchRatio);
    if (pitch > 0.0) {
        m_pStretch->setTempo(m_dTempoRatio / pitch);
        m_pStretch->setRate(m_dBaseRate * pitch);
    }
}

void EngineBufferScaleTransient::clear() {
    m_pStretch->clear();
}

double EngineBufferScaleTransient::scaleBuffer(
        CSAMPLE* pOutputBuffer,
        SINT iOutputBufferSize) {
    if (m_dBaseRate == 0.0 || m_dTempoRatio == 0.0 || m_dPitchRatio == 0.0) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        // No actual samples/frames have been read from the
        // unscaled input buffer!
        return 0.0;
    }

    double readFramesProcessed = 0;
    SINT remaining_frames = getOutputSignal().samples2frames(iOutputBufferSize);
    CSAMPLE* read = pOutputBuffer;
    bool last_read_failed = false;
    while (remaining_frames > 0) {
        const SINT received_frames = m_pStretch->receiveSamples(read, remaining_frames);
        remaining_frames -= received_frames;
        readFramesProcessed += m_effectiveRate * received_frames;
        read += getOutputSignal().frames2samples(received_frames);

        if (remaining_frames > 0) {
            // The requested setting becomes effective after all previous frames have been processed
            m_effectiveRate = m_dBaseRate * m_dTempoRatio;
            const SINT wanted = math_min(m_bufferBack.size(),
                    getOutputSignal().frames2samples(m_pStretch->freeInputFrames()));
            const SINT iAvailSamples = m_pReadAheadManager->getNextSamples(
                    // The value doesn't matter here. All that matters is we
                    // are going forward or backward.
                    (m_bBackwards ? -1.0 : 1.0) * m_effectiveRate,
                    m_bufferBack.data(),
                    wanted);
            const SINT iAvailFrames = getOutputSignal().samples2frames(iAvailSamples);

            if (iAvailFrames > 0) {
                last_read_failed = false;
                m_pStretch->putSamples(m_bufferBack.data(), iAvailFrames);
            } else {
                // We may get 0 samples once if we just hit a loop trigger, e.g.
                // when reloop_toggle jumps back to loop_in, or when moving a
                // loop causes the play position to be moved along.
                if (last_read_failed) {
                    // If we get 0 samples repeatedly, add silence to flush
                    // the last samples out of the stretcher.
                    qDebug() << "ReadAheadManager::getNextSamples() returned "
                                "zero samples repeatedly. Padding with silence.";
                    SampleUtil::clear(m_bufferBack.data(), m_bufferBack.size());
                    m_pStretch->putSamples(m_bufferBack.data(),
                            getOutputSignal().samples2frames(m_bufferBack.size()));
                }
                last_read_failed = true;
            }
        }
    }

    // readFramesProcessed is interpreted as the total number of frames
    // consumed to produce the scaled buffer. Due to this, we do not take into
    // account directionality or starting point.
    return readFramesProcessed;
}
