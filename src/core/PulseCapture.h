#pragma once

#include "AudioSource.h"
#include "SampleQueue.h"

#include <QString>
#include <memory>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;

/// AudioSource over libpulse's asynchronous API: one pa_threaded_mainloop,
/// one pa_context, one record stream for the microphone and (optionally) one
/// for the default sink's monitor. Entirely callback-driven; nothing ever
/// waits for the server, so setup, data delivery and teardown are bounded
/// regardless of server responsiveness.
///
/// Callbacks run on the mainloop thread with the mainloop lock held and only
/// ever push into SampleQueues (lock order: mainloop lock -> queue mutex).
/// Every fatal condition (stream/context failure, queue overflow) closes the
/// affected queues and is reported through the open callback as Failed, at
/// most once, independent of whether the encoder thread is alive.
///
/// The open callback is invoked with the mainloop lock held (usually on the
/// mainloop thread; on the caller's thread when connecting fails at once), so
/// it must not call stop() or block: relay the event to the owner's thread.
/// An instance is single-use: openAsync() once, stop() any number of times.
class PulseCapture : public AudioSource
{
public:
    PulseCapture(std::shared_ptr<SampleQueue> mic,
                 std::shared_ptr<SampleQueue> monitor,  ///< nullptr => mic only
                 QString micDevice = QString());         ///< "" => default source
    ~PulseCapture() override;

    void openAsync(Callback cb) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
