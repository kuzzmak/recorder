#pragma once

#include <QString>
#include <functional>

/// A capture backend that fills SampleQueues (created by the owner and shared
/// with the encoder) and never blocks the calling thread.
///
/// Contract:
/// - openAsync() returns immediately; `cb` fires later, on an arbitrary
///   thread, with Opened at most once and Failed at most once. A Failed after
///   Opened means a fatal runtime condition (stream lost, queue overflow …);
///   the owner must then stop() the source. The callee closes its queues with
///   the error before reporting it.
/// - stop() is bounded, idempotent, safe from the main thread at any phase
///   (including mid-connect) and closes every queue ("stopped", no error).
///   The destructor calls it.
class AudioSource
{
public:
    enum class Event { Opened, Failed };
    using Callback = std::function<void(Event, const QString &message)>;

    virtual ~AudioSource() = default;
    virtual void openAsync(Callback cb) = 0;
    virtual void stop() = 0;
};
