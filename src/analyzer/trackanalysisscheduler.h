#pragma once

#include <QList>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "analyzer/analyzerscheduledtrack.h"
#include "analyzer/analyzerthread.h"
#include "util/db/dbconnectionpool.h"

/// Callbacks for triggering side-effects in the outer context of
/// TrackAnalysisScheduler.
///
/// All functions will only be called from the host thread of
/// TrackAnalysisScheduler, not from worker threads.
class TrackAnalysisSchedulerEnvironment {
  public:
    virtual ~TrackAnalysisSchedulerEnvironment() = default;

    virtual TrackPointer loadTrackById(TrackId trackId) const = 0;
};

class TrackAnalysisScheduler : public QObject {
    Q_OBJECT

  public:
    typedef std::unique_ptr<TrackAnalysisScheduler, void(*)(TrackAnalysisScheduler*)> Pointer;
    // Subclass that provides a default constructor and nothing else
    class NullPointer: public Pointer {
      public:
        NullPointer();
    };

    static Pointer createInstance(
            std::unique_ptr<const TrackAnalysisSchedulerEnvironment> pEnvironment,
            int numWorkerThreads,
            const mixxx::DbConnectionPoolPtr& pDbConnectionPool,
            const UserSettingsPointer& pConfig,
            AnalyzerModeFlags modeFlags);

    /*private*/ TrackAnalysisScheduler(
            std::unique_ptr<const TrackAnalysisSchedulerEnvironment> pEnvironment,
            int numWorkerThreads,
            const mixxx::DbConnectionPoolPtr& pDbConnectionPool,
            const UserSettingsPointer& pUserSettings,
            AnalyzerModeFlags modeFlags);
    ~TrackAnalysisScheduler() override;

    // Schedule single or multiple tracks. After all tracks have been scheduled
    // the caller must invoke resume() once.
    bool scheduleTrack(AnalyzerScheduledTrack track);
    int scheduleTracks(const QList<AnalyzerScheduledTrack>& tracks);

    // LastCUE Box: Drops queued and abandons running analyses of tracks that
    // are not contained in loadedTrackIds, and moves priorityTrackId to the
    // front of the queue. Used for tracks loaded into players, so that a newly
    // loaded track does not wait behind tracks that have been replaced.
    void prioritizeLoadedTracks(
            TrackId priorityTrackId,
            const QList<TrackId>& loadedTrackIds);

  public slots:
    void suspend();

    // After scheduling tracks the analysis must be resumed once.
    // Resume must also be called after suspending the analysis.
    void resume();

    // Stops a running analysis and discards all enqueued tracks.
    void stop();

  signals:
    // Progress for individual tracks is passed-through from the workers
    void trackProgress(TrackId trackId, AnalyzerProgress analyzerProgress);
    // Current average progress for all scheduled tracks and from all workers
    void progress(AnalyzerProgress currentTrackProgress, int currentTrackNumber, int totalTracksCount);
    void finished();

  private slots:
    void onWorkerThreadProgress(int threadId, AnalyzerThreadState threadState, TrackId trackId, AnalyzerProgress analyzerProgress);

  private:
    // Owns an analyzer thread and buffers the most recent progress update
    // received from this thread during analysis. It does not need to be
    // thread-safe, because all functions are invoked from the host thread
    // that runs the TrackAnalysisScheduler.
    class Worker {
      public:
        explicit Worker(AnalyzerThread::Pointer pThread = AnalyzerThread::NullPointer())
                : m_pThread(std::move(pThread)),
                  m_analyzerProgress(kAnalyzerProgressUnknown) {
        }
        Worker(const Worker&) = delete;
        Worker(Worker&&) = default;

        ~Worker() {
            if (m_pThread) {
                m_pThread->stop();
                bool success = m_pThread->wait(5000); // 5 s
                DEBUG_ASSERT(success);
                m_pThread->wait();
                delete m_pThread.release();
            }
        }

        bool hasThread() const {
            return static_cast<bool>(m_pThread);
        }

        AnalyzerThread* thread() const {
            DEBUG_ASSERT(m_pThread);
            return m_pThread.get();
        }

        AnalyzerProgress analyzerProgress() const {
            return m_analyzerProgress;
        }

        bool submitNextTrack(const AnalyzerTrack& track, TrackId trackId) {
            DEBUG_ASSERT(m_pThread);
            if (m_cancelledTrackId == trackId) {
                // The track has been loaded again after it was abandoned.
                // Its previous analysis has finished, because tracks are
                // only submitted again after the done signal.
                m_pThread->cancelTrack(TrackId());
                m_cancelledTrackId = TrackId();
            }
            if (!m_pThread->submitNextTrack(std::move(track))) {
                return false;
            }
            m_submittedTrackId = trackId;
            return true;
        }

        TrackId submittedTrackId() const {
            return m_submittedTrackId;
        }

        void onTrackDone(TrackId trackId) {
            if (m_submittedTrackId == trackId) {
                m_submittedTrackId = TrackId();
            }
        }

        void cancelSubmittedTrack() {
            if (m_pThread && m_submittedTrackId.isValid()) {
                m_pThread->cancelTrack(m_submittedTrackId);
                m_cancelledTrackId = m_submittedTrackId;
            }
        }

        void suspendThread() {
            if (m_pThread) {
                m_pThread->suspend();
            }
        }

        void resumeThread() {
            if (m_pThread) {
                m_pThread->resume();
            }
        }

        void stopThread() {
            if (m_pThread) {
                m_pThread->stop();
            }
        }

        void onAnalyzerProgress(AnalyzerProgress analyzerProgress) {
            DEBUG_ASSERT(m_pThread);
            m_analyzerProgress = analyzerProgress;
        }

        void onThreadExit() {
            DEBUG_ASSERT(m_pThread);
            m_pThread.reset();
            m_analyzerProgress = kAnalyzerProgressUnknown;
        }

      private:
        AnalyzerThread::Pointer m_pThread;
        AnalyzerProgress m_analyzerProgress;
        TrackId m_submittedTrackId;
        TrackId m_cancelledTrackId;
    };

    bool submitNextTrack(Worker* worker);
    void emitProgressOrFinished();

    bool allTracksFinished() const {
        return m_queuedTracks.empty() &&
                m_pendingTrackIds.empty();
    }

    const std::unique_ptr<const TrackAnalysisSchedulerEnvironment> m_pEnvironment;

    std::vector<Worker> m_workers;

    std::deque<AnalyzerScheduledTrack> m_queuedTracks;

    // Tracks that have already been submitted to workers
    // and not yet reported back as finished.
    std::set<TrackId> m_pendingTrackIds;

    AnalyzerProgress m_currentTrackProgress;

    int m_currentTrackNumber;

    int m_dequeuedTracksCount;

    typedef std::chrono::steady_clock Clock;
    Clock::time_point m_lastProgressEmittedAt;
};
