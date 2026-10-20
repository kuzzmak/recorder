#include "Recorder.h"

#include "AudioFormat.h"

#include "OutputPath.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

Recorder::Recorder(SourceFactory sourceFactory, WriterFactory writerFactory, QObject *parent)
    : QObject(parent)
    , m_sourceFactory(std::move(sourceFactory))
    , m_writerFactory(std::move(writerFactory))
{
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Recorder::tick);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        finishSession(m_sessionId, QStringLiteral("Sound server did not respond"));
    });
    m_finishWatchdog.setSingleShot(true);
    connect(&m_finishWatchdog, &QTimer::timeout, this, [this] {
        if (m_state != State::Stopping || m_thread == nullptr)
            return;
        // Still Stopping, the worker is kept: it owns the file and the only
        // safe thing is to wait for it. Warn and keep watching.
        emit error(QStringLiteral("Finishing the file is taking longer than expected"));
        m_finishWatchdog.start(m_finishWatchdogMs);
    });
}

Recorder::~Recorder()
{
    // Never delete a running worker: it owns everything it touches and
    // finishes (or dies with the process) on its own.
    if (m_thread) {
        disconnect(m_thread, nullptr, this, nullptr);
        if (m_thread->isFinished())
            delete m_thread;
        m_thread = nullptr;
    }
    // m_source's own destructor stops it (AudioSource contract).
}

bool Recorder::start(const SessionSpec &spec)
{
    if (m_state != State::Idle || m_thread != nullptr || m_shuttingDown) {
        emit error(m_shuttingDown ? QStringLiteral("Cannot start: the recorder is shutting down")
                                  : QStringLiteral("Cannot start: a session is still active"));
        return false;
    }

    // Fresh per-session bookkeeping.
    ++m_sessionId;
    m_spec = spec;
    m_completed = false;
    m_finishing = false;
    m_pendingError.clear();
    m_durationMs = 0;
    m_clock.invalidate();
    m_lastResult = {};
    setState(State::Starting);
    m_deadline.start(m_startDeadlineMs);

    QDir().mkpath(spec.outputDir);
    const QString ext = m_writerFactory(spec)->extension();
    m_finalPath = OutputPath::unique(
        spec.outputDir, OutputPath::fileName(QDateTime::currentDateTime(), spec.captureSystemAudio, ext));
    m_tempPath = OutputPath::tempFor(m_finalPath);

    m_queues.mic = std::make_shared<SampleQueue>(m_queueCapacityFrames);
    m_queues.monitor =
        spec.captureSystemAudio ? std::make_shared<SampleQueue>(m_queueCapacityFrames) : nullptr;

    m_source = m_sourceFactory(m_queues, spec);
    const quint64 id = m_sessionId;
    // Fires on the backend's thread, possibly with its lock held: relay only.
    m_source->openAsync([this, id](AudioSource::Event event, const QString &message) {
        QMetaObject::invokeMethod(
            this, [this, id, event, message] { onSourceEvent(id, event, message); },
            Qt::QueuedConnection);
    });
    return true;
}

void Recorder::stop()
{
    if (m_state != State::Idle)
        finishSession(m_sessionId, QString());
}

bool Recorder::shutdown(int ms)
{
    m_shuttingDown = true;
    if (m_state != State::Idle)
        finishSession(m_sessionId, QString());   // may complete synchronously
    if (m_thread == nullptr)
        return true;
    if (m_thread->wait(ms)) {
        if (!m_completed)
            complete();
        return true;
    }
    // Still running: detach. It is intentionally leaked (owns its writer,
    // aligner and queue references) and will finalize or die with the process.
    disconnect(m_thread, nullptr, this, nullptr);
    m_thread = nullptr;
    return false;
}

qint64 Recorder::elapsedMs() const
{
    return m_clock.isValid() ? m_clock.elapsed() : m_durationMs;
}

namespace {
/// "mm:ss", or "h:mm:ss" from one hour on.
QString formatDuration(qint64 ms)
{
    const qint64 totalSeconds = ms / 1000;
    const qint64 h = totalSeconds / 3600;
    const int m = int(totalSeconds / 60 % 60);
    const int s = int(totalSeconds % 60);
    const QString ms_ = QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return h > 0 ? QStringLiteral("%1:%2").arg(h).arg(ms_) : ms_;
}
}  // namespace

QString Recorder::outcomeText(Outcome o, const QString &path, qint64 durationMs, const QString &error)
{
    const QString name = QFileInfo(path).fileName();
    switch (o) {
    case Outcome::Saved:
        return QStringLiteral("Saved %1 \u2192 %2").arg(formatDuration(durationMs), name);
    case Outcome::SavedTruncated:
        return QStringLiteral("Recording stopped early (%1). Saved %2 \u2192 %3")
            .arg(error, formatDuration(durationMs), name);
    case Outcome::RenameFailed:
        return QStringLiteral("Saved but could not rename; file is %1").arg(name);
    case Outcome::Unfinalized:
        return QStringLiteral("Could not finalize; partial file %1").arg(name);
    case Outcome::Failed:
        break;
    }
    return QStringLiteral("Could not record: %1")
        .arg(error.isEmpty() ? QStringLiteral("no audio was captured") : error);
}

void Recorder::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void Recorder::finishSession(quint64 id, const QString &err)
{
    if (id != m_sessionId || m_finishing)
        return;
    m_finishing = true;
    setState(State::Stopping);
    m_deadline.stop();
    m_tick.stop();
    if (m_clock.isValid()) {
        m_durationMs = m_clock.elapsed();
        m_clock.invalidate();
    }
    if (m_pendingError.isEmpty())
        m_pendingError = err;

    // The source goes first: with the queues closed the worker's drain is bounded.
    if (m_source)
        m_source->stop();
    if (m_thread) {
        m_thread->requestStop();
        m_finishWatchdog.start(m_finishWatchdogMs);
        return;   // continues in onThreadFinished()
    }
    complete();
}

void Recorder::complete()
{
    m_completed = true;
    m_finishWatchdog.stop();

    if (m_thread) {
        m_lastResult = m_thread->result();
    } else {
        m_lastResult = {};
        m_lastResult.fileFinalized = false;
    }

    // Finish every transition first so a stopped() handler may start() again.
    m_source.reset();
    m_queues = {};
    if (m_thread) {
        m_thread->wait();   // finished() precedes the thread's own teardown
        m_thread->deleteLater();
        m_thread = nullptr;
    }

    const QString errorText = !m_pendingError.isEmpty() ? m_pendingError : m_lastResult.error;
    Outcome outcome = Outcome::Failed;
    QString path;
    if (m_lastResult.framesWritten == 0) {
        QFile::remove(m_tempPath);
    } else if (m_lastResult.fileFinalized) {
        if (QFile::rename(m_tempPath, m_finalPath)) {
            outcome = errorText.isEmpty() ? Outcome::Saved : Outcome::SavedTruncated;
            path = m_finalPath;
        } else {
            outcome = Outcome::RenameFailed;
            path = m_tempPath;
        }
    } else {
        outcome = Outcome::Unfinalized;
        path = m_tempPath;
    }

    // Report the audio actually in the file, not wall-clock time: a frozen
    // server or a stall adds nothing to the file but still ticks the clock.
    const qint64 durationMs = m_lastResult.framesWritten > 0
        ? AudioFormat::msForFrames(m_lastResult.framesWritten)
        : m_durationMs;
    m_durationMs = durationMs;  // elapsedMs() reports the same figure afterwards
    setState(State::Idle);
    if (!errorText.isEmpty())
        emit error(errorText);
    emit stopped(path, durationMs, outcome);
}

void Recorder::onSourceEvent(quint64 id, AudioSource::Event event, const QString &message)
{
    if (id != m_sessionId)
        return;
    if (event == AudioSource::Event::Failed) {
        finishSession(id, message);
        return;
    }
    if (m_state != State::Starting || m_thread != nullptr)
        return;

    EncoderThread::Config config;
    config.tempPath = m_tempPath;
    config.aligner.systemGain = m_spec.systemGain;
    config.waitMs = m_encoderWaitMs;
    config.noProgressMs = m_encoderNoProgressMs;
    const WriterFactory factory = m_writerFactory;
    const SessionSpec spec = m_spec;
    m_thread = new EncoderThread(id, m_queues.mic, m_queues.monitor,
                                 [factory, spec] { return factory(spec); }, config);
    connect(m_thread, &EncoderThread::captureStarted, this, &Recorder::onCaptureStarted,
            Qt::QueuedConnection);
    connect(m_thread, &EncoderThread::levelChanged, this,
            [this](quint64 sid, int peak) {
                if (sid == m_sessionId)
                    emit levelChanged(peak);
            },
            Qt::QueuedConnection);
    connect(m_thread, &EncoderThread::sessionFailed, this, &Recorder::onSessionFailed,
            Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, this, [this, id] { onThreadFinished(id); },
            Qt::QueuedConnection);
    m_thread->start();
}

void Recorder::onCaptureStarted(quint64 id)
{
    if (id != m_sessionId || m_state != State::Starting)
        return;
    m_deadline.stop();
    setState(State::Recording);
    m_clock.start();
    m_tick.start();
    emit started(m_finalPath);
}

void Recorder::onSessionFailed(quint64 id, const QString &message)
{
    finishSession(id, message);
}

void Recorder::onThreadFinished(quint64 id)
{
    if (id != m_sessionId || m_completed)
        return;
    complete();
}
