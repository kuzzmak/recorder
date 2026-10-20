#pragma once

#include "AudioWriter.h"
#include "SampleQueue.h"
#include "StreamAligner.h"

#include <QString>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

/// What a finished (or abandoned) session produced.
struct CaptureResult {
    qint64 framesWritten = 0;
    bool fileFinalized = false;
    QString error;          ///< first fatal error, empty when none
    AlignerStats stats;
};

/// Worker that owns the writer, the aligner and (shared) queues, pulls aligned
/// chunks and writes them to disk off the audio thread.
///
/// Lifecycle: start() -> opens the writer on `tempPath` -> captureStarted(id)
/// -> loop until requestStop(): wait on the mic queue <= 100 ms, encode every
/// available chunk, emit levelChanged every 5th chunk; a write failure, an
/// error-closed queue or `noProgressMs` without a chunk -> sessionFailed(id)
/// and the loop keeps draining+discarding until requestStop(). On stop:
/// aligner.beginDrain() into the writer (unless a write failed), finalize,
/// then the thread finishes and result() is valid.
///
/// The worker never ends a session by itself, so the owner always stops the
/// source first and the drain is bounded. It touches nothing it does not own:
/// after a shutdown timeout the owner may detach it and let it run to
/// completion on its own (queues are shared_ptr, writer/aligner are owned).
class EncoderThread : public QThread
{
    Q_OBJECT

public:
    using WriterFactory = std::function<std::unique_ptr<AudioWriter>()>;

    struct Config {
        QString tempPath;
        AlignerConfig aligner;
        int waitMs = 100;
        int noProgressMs = 5000;
        int levelEveryChunks = 5;
    };

    EncoderThread(quint64 sessionId,
                  std::shared_ptr<SampleQueue> mic,
                  std::shared_ptr<SampleQueue> monitor,  ///< nullptr => mic only
                  WriterFactory writerFactory,
                  Config config,
                  QObject *parent = nullptr);
    ~EncoderThread() override;

    quint64 sessionId() const { return m_id; }
    void requestStop();
    /// Valid once the thread has finished.
    CaptureResult result() const;

signals:
    void captureStarted(quint64 sessionId);
    void levelChanged(quint64 sessionId, int peak);
    void sessionFailed(quint64 sessionId, const QString &message);

protected:
    void run() override;

private:
    quint64 m_id;
    std::shared_ptr<SampleQueue> m_mic;
    std::shared_ptr<SampleQueue> m_monitor;
    WriterFactory m_writerFactory;
    Config m_config;
    std::atomic<bool> m_stop{false};
    CaptureResult m_result;
};
