#include "SampleQueue.h"

SampleQueue::SampleQueue(qint64 maxFrames)
    : m_maxFrames(maxFrames)
{
}

bool SampleQueue::push(const int16_t *samples, std::size_t frames, qint64 positionStart)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed)
        return false;
    if (m_pendingFrames + qint64(frames) > m_maxFrames) {
        m_closed = true;
        m_error = QString::fromLatin1(kOverflowError);
        m_cv.notify_all();
        return false;
    }
    Block b;
    b.positionStart = positionStart;
    if (samples)
        b.samples.assign(samples, samples + frames);
    else
        b.samples.assign(frames, 0);
    m_blocks.push_back(std::move(b));
    m_pendingFrames += qint64(frames);
    m_cv.notify_all();
    return true;
}

bool SampleQueue::pushSilence(std::size_t frames, qint64 positionStart)
{
    return push(nullptr, frames, positionStart);
}

void SampleQueue::pushSnapshot(const TimingSnapshot &snapshot)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = snapshot;
}

void SampleQueue::close(const QString &error)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
            return;
        m_closed = true;
        m_error = error;
    }
    m_cv.notify_all();
}

std::vector<SampleQueue::Block> SampleQueue::take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Block> out;
    out.swap(m_blocks);
    m_pendingFrames = 0;
    return out;
}

bool SampleQueue::waitForChange(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return !m_blocks.empty() || m_closed; });
}
TimingSnapshot SampleQueue::latestSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}
bool SampleQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

QString SampleQueue::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

qint64 SampleQueue::pendingFrames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingFrames;
}
