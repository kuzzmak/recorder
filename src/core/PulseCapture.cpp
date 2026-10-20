#include "PulseCapture.h"

#include "AudioFormat.h"

#include <pulse/pulseaudio.h>

#include <cstdint>

namespace {

constexpr uint32_t kMaxlengthBytes = uint32_t(AudioFormat::framesForMs(200) * AudioFormat::kBytesPerFrame);
constexpr uint32_t kFragsizeBytes = uint32_t(AudioFormat::chunkBytes());
constexpr const char *kDefaultMonitor = "@DEFAULT_MONITOR@";
constexpr const char *kStreamLost = "Connection terminated";

QString errorString(pa_context *ctx)
{
    return QString::fromUtf8(pa_strerror(pa_context_errno(ctx)));
}

}  // namespace

struct PulseCapture::Impl {
    /// Everything libpulse sees as userdata. Owned by Impl and outliving every
    /// stream (streams are unref'd under the mainloop lock before the loop is
    /// stopped, and the loop is stopped before Impl dies).
    struct StreamCtx {
        Impl *owner = nullptr;
        std::shared_ptr<SampleQueue> queue;
        const char *name = nullptr;
        bool isMonitor = false;
        pa_stream *stream = nullptr;
        qint64 byteIndex = 0;  ///< bytes delivered by the server so far, holes included
        bool ready = false;
    };

    std::shared_ptr<SampleQueue> mic;
    std::shared_ptr<SampleQueue> monitor;
    QString micDevice;

    Callback cb;
    pa_threaded_mainloop *loop = nullptr;
    pa_context *ctx = nullptr;
    pa_operation *serverInfoOp = nullptr;
    StreamCtx micStream;
    StreamCtx monitorStream;
    bool openedReported = false;
    bool failedReported = false;
    bool monitorFallbackTried = false;

    // --- reporting (mainloop thread, lock held; or main thread inside openAsync)
    void reportOpened()
    {
        if (openedReported)
            return;
        openedReported = true;
        if (cb)
            cb(Event::Opened, QString());
    }
    void reportFailed(const QString &reason)
    {
        if (failedReported)
            return;
        failedReported = true;
        if (cb)
            cb(Event::Failed, reason);
    }
    /// Close every queue with `reason` (first reason wins per queue) and
    /// report Failed once.
    void failAll(const QString &reason)
    {
        if (mic)
            mic->close(reason);
        if (monitor)
            monitor->close(reason);
        reportFailed(reason);
    }

    // --- stream setup --------------------------------------------------------
    bool createStream(StreamCtx &sc, const char *device)
    {
        pa_sample_spec ss{};
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = uint32_t(AudioFormat::kRate);
        ss.channels = uint8_t(AudioFormat::kChannels);

        sc.stream = pa_stream_new(ctx, sc.name, &ss, nullptr);
        if (!sc.stream)
            return false;
        pa_stream_set_state_callback(sc.stream, &Impl::streamStateCb, &sc);
        pa_stream_set_read_callback(sc.stream, &Impl::streamReadCb, &sc);
        pa_stream_set_latency_update_callback(sc.stream, &Impl::streamLatencyCb, &sc);

        pa_buffer_attr attr{};
        attr.maxlength = kMaxlengthBytes;
        attr.fragsize = kFragsizeBytes;
        attr.tlength = attr.prebuf = attr.minreq = uint32_t(-1);
        const auto flags = pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);
        return pa_stream_connect_record(sc.stream, device, &attr, flags) == 0;
    }

    void destroyStream(StreamCtx &sc)
    {
        if (!sc.stream)
            return;
        pa_stream_set_state_callback(sc.stream, nullptr, nullptr);
        pa_stream_set_read_callback(sc.stream, nullptr, nullptr);
        pa_stream_set_latency_update_callback(sc.stream, nullptr, nullptr);
        pa_stream_disconnect(sc.stream);
        pa_stream_unref(sc.stream);
        sc.stream = nullptr;
        sc.ready = false;
    }

    void onContextReady()
    {
        const QByteArray micDev = micDevice.toUtf8();
        if (!createStream(micStream, micDev.isEmpty() ? nullptr : micDev.constData())) {
            failAll(errorString(ctx));
            return;
        }
        if (monitor && !createStream(monitorStream, kDefaultMonitor))
            failAll(errorString(ctx));
    }

    void checkAllReady()
    {
        if (micStream.ready && (!monitor || monitorStream.ready))
            reportOpened();
    }

    /// Monitor stream failed to connect: retry once on the explicit
    /// "<default sink>.monitor" (servers that reject @DEFAULT_MONITOR@).
    void retryMonitorViaServerInfo()
    {
        monitorFallbackTried = true;
        destroyStream(monitorStream);
        serverInfoOp = pa_context_get_server_info(ctx, &Impl::serverInfoCb, this);
        if (!serverInfoOp)
            failAll(errorString(ctx));
    }

    // --- libpulse callbacks (mainloop thread, lock held) --------------------
    static void contextStateCb(pa_context *c, void *userdata)
    {
        auto *self = static_cast<Impl *>(userdata);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            self->onContextReady();
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->failAll(errorString(c));
            break;
        default:
            break;
        }
    }

    static void serverInfoCb(pa_context *c, const pa_server_info *info, void *userdata)
    {
        auto *self = static_cast<Impl *>(userdata);
        if (self->serverInfoOp) {
            pa_operation_unref(self->serverInfoOp);
            self->serverInfoOp = nullptr;
        }
        if (!info || !info->default_sink_name) {
            self->failAll(info ? QStringLiteral("No default sink") : errorString(c));
            return;
        }
        const QByteArray dev = QByteArray(info->default_sink_name) + ".monitor";
        if (!self->createStream(self->monitorStream, dev.constData()))
            self->failAll(errorString(c));
    }

    static void streamStateCb(pa_stream *s, void *userdata)
    {
        auto *sc = static_cast<StreamCtx *>(userdata);
        Impl *self = sc->owner;
        switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
            sc->ready = true;
            self->checkAllReady();
            break;
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            if (sc->ready) {
                // Lost after setup: fatal runtime condition.
                sc->ready = false;
                sc->queue->close(QString::fromLatin1(kStreamLost));
                self->reportFailed(QString::fromLatin1(kStreamLost));
            } else if (sc->isMonitor && !self->monitorFallbackTried
                       && pa_context_errno(self->ctx) == PA_ERR_NOENTITY) {
                self->retryMonitorViaServerInfo();
            } else {
                self->failAll(errorString(self->ctx));
            }
            break;
        default:
            break;
        }
    }

    static void streamReadCb(pa_stream *s, size_t /*length*/, void *userdata)
    {
        auto *sc = static_cast<StreamCtx *>(userdata);
        for (;;) {
            const void *data = nullptr;
            size_t nbytes = 0;
            if (pa_stream_peek(s, &data, &nbytes) < 0 || nbytes == 0)
                break;
            const std::size_t frames = nbytes / AudioFormat::kBytesPerFrame;
            const qint64 position = sc->byteIndex / AudioFormat::kBytesPerFrame;
            const bool ok = data ? sc->queue->push(static_cast<const int16_t *>(data), frames, position)
                                 : sc->queue->pushSilence(frames, position);
            sc->byteIndex += qint64(nbytes);
            pa_stream_drop(s);
            if (!ok) {
                // Overflow (or a queue closed by another failure): the queue
                // already carries the reason; report it once and keep draining
                // so the server-side buffer does not fill.
                const QString err = sc->queue->error();
                sc->owner->reportFailed(err.isEmpty() ? QString::fromLatin1(kStreamLost) : err);
            }
        }
    }

    static void streamLatencyCb(pa_stream *s, void *userdata)
    {
        auto *sc = static_cast<StreamCtx *>(userdata);
        const pa_timing_info *ti = pa_stream_get_timing_info(s);
        TimingSnapshot snap;
        if (ti) {
            snap.systemTimeUs = qint64(pa_timeval_load(&ti->timestamp));
            snap.positionFrames = qint64(ti->write_index) / AudioFormat::kBytesPerFrame;
            snap.captureOffsetUs = qint64(ti->source_usec) - (sc->isMonitor ? qint64(ti->sink_usec) : 0);
            snap.valid = !ti->write_index_corrupt && snap.systemTimeUs != 0;
        }
        sc->queue->pushSnapshot(snap);
    }
};

PulseCapture::PulseCapture(std::shared_ptr<SampleQueue> mic,
                           std::shared_ptr<SampleQueue> monitor,
                           QString micDevice)
    : d(new Impl)
{
    d->mic = std::move(mic);
    d->monitor = std::move(monitor);
    d->micDevice = std::move(micDevice);
    d->micStream.owner = d.get();
    d->micStream.queue = d->mic;
    d->micStream.name = "Microphone";
    d->monitorStream.owner = d.get();
    d->monitorStream.queue = d->monitor;
    d->monitorStream.name = "System audio";
    d->monitorStream.isMonitor = true;
}

PulseCapture::~PulseCapture()
{
    stop();
}

void PulseCapture::openAsync(Callback cb)
{
    if (d->loop)
        return;  // already opened (or stopped: a PulseCapture is single-use)
    d->cb = std::move(cb);

    d->loop = pa_threaded_mainloop_new();
    if (!d->loop || pa_threaded_mainloop_start(d->loop) < 0) {
        if (d->loop) {
            pa_threaded_mainloop_free(d->loop);
            d->loop = nullptr;
        }
        d->failAll(QStringLiteral("Cannot start the audio main loop"));
        return;
    }

    pa_threaded_mainloop_lock(d->loop);
    d->ctx = pa_context_new(pa_threaded_mainloop_get_api(d->loop), "Recorder");
    if (!d->ctx) {
        pa_threaded_mainloop_unlock(d->loop);
        d->failAll(QStringLiteral("Cannot create the audio context"));
        return;
    }
    pa_context_set_state_callback(d->ctx, &Impl::contextStateCb, d.get());
    if (pa_context_connect(d->ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
        d->failAll(errorString(d->ctx));  // the state callback may already have reported it
    pa_threaded_mainloop_unlock(d->loop);
}

void PulseCapture::stop()
{
    if (!d->loop)
        return;

    pa_threaded_mainloop_lock(d->loop);
    d->destroyStream(d->micStream);
    d->destroyStream(d->monitorStream);
    if (d->serverInfoOp) {
        pa_operation_cancel(d->serverInfoOp);
        pa_operation_unref(d->serverInfoOp);
        d->serverInfoOp = nullptr;
    }
    if (d->ctx) {
        pa_context_set_state_callback(d->ctx, nullptr, nullptr);
        pa_context_disconnect(d->ctx);
        pa_context_unref(d->ctx);
        d->ctx = nullptr;
    }
    pa_threaded_mainloop_unlock(d->loop);

    pa_threaded_mainloop_stop(d->loop);
    pa_threaded_mainloop_free(d->loop);
    d->loop = nullptr;

    if (d->mic)
        d->mic->close();
    if (d->monitor)
        d->monitor->close();
}
