#pragma once

#include "AudioSource.h"
#include "AudioWriter.h"
#include "EncoderThread.h"
#include "SampleQueue.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

/// Main-thread orchestrator of one recording session at a time.
///
/// States: Idle -> Starting (source opening, worker starting) -> Recording
/// -> Stopping (source stopped, worker draining/finalizing) -> Idle.
/// Every terminal event (user stop, source failure, worker failure, start
/// deadline, shutdown) funnels through one path, finishSession(), which stops
/// the source first and completes exactly once per session.
///
/// Backends are injected through factories so the lifecycle is testable with
/// a fake source and fake writer.
class Recorder : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Starting, Recording, Stopping };
    Q_ENUM(State)

    enum class Outcome {
        Saved,           ///< no error, finalized, renamed: path is final
        SavedTruncated,  ///< error mid-way, finalized, renamed: path is final
        RenameFailed,    ///< finalized but rename failed: path is the .part
        Unfinalized,     ///< finalize failed, frames > 0: path is the .part
        Failed           ///< no usable file: path is empty, temp removed
    };
    Q_ENUM(Outcome)

    /// Everything one session needs, captured from Settings at start().
    struct SessionSpec {
        QString outputDir;
        QString format = QStringLiteral("opus");  // "opus" | "wav"
        bool captureSystemAudio = true;
        int opusBitrate = 32000;
        double systemGain = 1.0;
        QString micDevice;                        // "" => default source
    };

    struct Queues {
        std::shared_ptr<SampleQueue> mic;
        std::shared_ptr<SampleQueue> monitor;     // null when system audio is off
    };

    using SourceFactory = std::function<std::unique_ptr<AudioSource>(const Queues &, const SessionSpec &)>;
    using WriterFactory = std::function<std::unique_ptr<AudioWriter>(const SessionSpec &)>;

    Recorder(SourceFactory sourceFactory, WriterFactory writerFactory, QObject *parent = nullptr);
    ~Recorder() override;

    /// Begin a session. False (with error()) unless Idle, no cleanup pending
    /// and not shutting down. State becomes Starting immediately.
    bool start(const SessionSpec &spec);
    /// Finish the current session (any phase). No-op when Idle.
    void stop();
    /// Finish any session and wait up to `ms` for the worker. Returns false if
    /// the worker is still running; it is then detached and left to finish or
    /// die with the process. start() is refused afterwards.
    bool shutdown(int ms);

    State state() const { return m_state; }
    qint64 elapsedMs() const;
    QString currentPath() const { return m_finalPath; }
    const CaptureResult &lastResult() const { return m_lastResult; }

    // Timeouts (ms); tests shorten them.
    void setStartDeadlineMs(int ms) { m_startDeadlineMs = ms; }
    void setFinishWatchdogMs(int ms) { m_finishWatchdogMs = ms; }
    void setEncoderConfig(int waitMs, int noProgressMs)
    {
        m_encoderWaitMs = waitMs;
        m_encoderNoProgressMs = noProgressMs;
    }
    /// Capacity of each capture queue (frames); tests shrink it to provoke
    /// an overflow quickly.
    void setQueueCapacityFrames(qint64 frames) { m_queueCapacityFrames = frames; }

    static QString outcomeText(Outcome o, const QString &path, qint64 durationMs, const QString &error);

signals:
    void stateChanged(Recorder::State state);
    void started(const QString &finalPath);
    void stopped(const QString &path, qint64 durationMs, Recorder::Outcome outcome);
    void error(const QString &message);
    void levelChanged(int peak);
    void tick();  ///< once a second while recording

private:
    void setState(State s);
    void finishSession(quint64 id, const QString &error);
    void complete();

    void onSourceEvent(quint64 id, AudioSource::Event event, const QString &message);
    void onCaptureStarted(quint64 id);
    void onSessionFailed(quint64 id, const QString &message);
    void onThreadFinished(quint64 id);

    SourceFactory m_sourceFactory;
    WriterFactory m_writerFactory;

    State m_state = State::Idle;
    quint64 m_sessionId = 0;
    SessionSpec m_spec;
    Queues m_queues;
    std::unique_ptr<AudioSource> m_source;
    EncoderThread *m_thread = nullptr;
    QString m_finalPath;
    QString m_tempPath;
    QElapsedTimer m_clock;
    qint64 m_durationMs = 0;
    QTimer m_tick;
    QTimer m_deadline;
    QTimer m_finishWatchdog;
    QString m_pendingError;
    bool m_finishing = false;
    bool m_completed = false;
    bool m_shuttingDown = false;
    CaptureResult m_lastResult;

    int m_startDeadlineMs = 5000;
    int m_finishWatchdogMs = 5000;
    int m_encoderWaitMs = 100;
    int m_encoderNoProgressMs = 5000;
    qint64 m_queueCapacityFrames = SampleQueue::kDefaultMaxFrames;
};
