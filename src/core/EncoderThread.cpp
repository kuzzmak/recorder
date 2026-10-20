#include "EncoderThread.h"

#include "AudioFormat.h"
#include "AudioMixer.h"

#include <QElapsedTimer>
#include <algorithm>
#include <vector>

EncoderThread::EncoderThread(quint64 sessionId,
                             std::shared_ptr<SampleQueue> mic,
                             std::shared_ptr<SampleQueue> monitor,
                             WriterFactory writerFactory,
                             Config config,
                             QObject *parent)
    : QThread(parent)
    , m_id(sessionId)
    , m_mic(std::move(mic))
    , m_monitor(std::move(monitor))
    , m_writerFactory(std::move(writerFactory))
    , m_config(std::move(config))
{
}

EncoderThread::~EncoderThread() = default;

void EncoderThread::requestStop() { m_stop = true; }

CaptureResult EncoderThread::result() const { return m_result; }

void EncoderThread::run()
{
    constexpr int kChunk = AudioFormat::kFramesPerChunk;
    std::vector<int16_t> buf(kChunk);
    QString err;

    // First failure only: later ones are consequences of the first.
    const auto fail = [this](const QString &message) {
        if (!m_result.error.isEmpty())
            return;
        m_result.error = message;
        emit sessionFailed(m_id, message);
    };
    // One pass of "wait for the producer" that never busy-loops once a
    // queue is closed (waitForChange returns at once then).
    const auto waitForProducer = [this] {
        m_mic->waitForChange(m_config.waitMs);
        if (m_mic->isClosed() && m_mic->pendingFrames() == 0)
            msleep(unsigned(std::max(1, std::min(m_config.waitMs, 10))));
    };
    // After a fatal error: keep the queues empty so the producer never
    // overflows; the owner decides when the session ends.
    const auto drainAndDiscardUntilStop = [this, &waitForProducer] {
        while (!m_stop) {
            waitForProducer();
            m_mic->take();
            if (m_monitor)
                m_monitor->take();
        }
    };

    std::unique_ptr<AudioWriter> writer = m_writerFactory();
    if (!writer->open(m_config.tempPath, &err)) {
        fail(QStringLiteral("Cannot create file: %1").arg(err));
        drainAndDiscardUntilStop();
        m_result.fileFinalized = false;
        return;
    }
    emit captureStarted(m_id);

    StreamAligner aligner(m_mic, m_monitor, m_config.aligner);
    bool writeFailed = false;
    QElapsedTimer progress;
    progress.start();
    qint64 chunks = 0;
    // Write every chunk the aligner can emit; false after a write failure.
    const auto encodeAvailable = [&] {
        while (aligner.nextChunk(buf.data())) {
            if (!writer->write(buf.data(), kChunk, &err)) {
                writeFailed = true;
                fail(QStringLiteral("Write failed: %1").arg(err));
                return false;
            }
            m_result.framesWritten += kChunk;
            progress.restart();
            if (m_config.levelEveryChunks > 0 && ++chunks % m_config.levelEveryChunks == 0)
                emit levelChanged(m_id, AudioMixer::peak(buf.data(), kChunk));
        }
        return true;
    };

    while (!m_stop) {
        waitForProducer();
        encodeAvailable();
        if (writeFailed) {
            drainAndDiscardUntilStop();
            break;
        }
        const auto queueError = [](const std::shared_ptr<SampleQueue> &q) {
            return q && q->isClosed() ? q->error() : QString();
        };
        QString qerr = queueError(m_mic);
        if (qerr.isEmpty())
            qerr = queueError(m_monitor);
        if (!qerr.isEmpty())
            fail(qerr);
        if (progress.elapsed() >= m_config.noProgressMs) {
            fail(QStringLiteral("No audio received for %1 s").arg(m_config.noProgressMs / 1000.0));
            progress.restart();   // report once per silent interval, never spin
        }
    }

    // After a write failure nothing more goes to disk, but what was written
    // is still worth finalizing (a readable truncated file).
    if (!writeFailed) {
        aligner.beginDrain();
        encodeAvailable();
    }
    m_result.fileFinalized = writer->finalize(&err);
    if (!m_result.fileFinalized)
        fail(QStringLiteral("Could not finalize: %1").arg(err));
    m_result.stats = aligner.stats();
}
