#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>

/// Sink for finished PCM16 mono 48 kHz chunks. Owned and driven by the
/// encoder thread only. Every call reports failure through `*err`.
class AudioWriter
{
public:
    virtual ~AudioWriter() = default;
    /// File extension without the dot ("ogg", "wav").
    virtual QString extension() const = 0;
    virtual bool open(const QString &path, QString *err) = 0;
    virtual bool write(const int16_t *pcm, std::size_t frames, QString *err) = 0;
    /// Flush and close. Idempotent; a second call is a no-op returning true.
    virtual bool finalize(QString *err) = 0;
};
