#include "StreamClock.h"

#include <cmath>

namespace {
constexpr qint64 kRateErrorSpanUs = 60'000'000;  // snapshots must span 60 s
}

StreamClock::StreamClock(int rate)
    : m_rate(rate)
{
}

void StreamClock::addSnapshot(const TimingSnapshot &snapshot)
{
    if (!snapshot.valid)
        return;
    const qint64 captureTimeUs = snapshot.systemTimeUs - snapshot.captureOffsetUs;
    const double impliedOffsetUs =
        double(captureTimeUs) - double(snapshot.positionFrames) * 1e6 / m_rate;
    if (m_count == 0) {
        m_offsetUs = impliedOffsetUs;
        m_firstPos = snapshot.positionFrames;
        m_firstTimeUs = captureTimeUs;
    } else {
        m_offsetUs += kSmoothing * (impliedOffsetUs - m_offsetUs);
    }
    m_latestPos = snapshot.positionFrames;
    m_latestTimeUs = captureTimeUs;
    m_anchorPos = snapshot.positionFrames;
    m_anchorTimeUs = qint64(std::llround(m_offsetUs + double(m_anchorPos) * 1e6 / m_rate));
    ++m_count;
}

bool StreamClock::isAvailable() const { return m_count > 0; }

qint64 StreamClock::timeAtUs(qint64 positionFrames) const
{
    return m_anchorTimeUs + qint64(std::llround(double(positionFrames - m_anchorPos) * 1e6 / m_rate));
}

qint64 StreamClock::positionAt(qint64 systemTimeUs) const
{
    return m_anchorPos + qint64(std::llround(double(systemTimeUs - m_anchorTimeUs) * m_rate / 1e6));
}

double StreamClock::rateError() const
{
    const qint64 spanUs = m_latestTimeUs - m_firstTimeUs;
    if (m_count < 2 || spanUs < kRateErrorSpanUs)
        return 0.0;
    const double framesPerSecond = double(m_latestPos - m_firstPos) * 1e6 / double(spanUs);
    return framesPerSecond / m_rate - 1.0;
}
