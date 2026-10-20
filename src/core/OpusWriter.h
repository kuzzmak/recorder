#pragma once

#include "AudioWriter.h"

struct OggOpusEnc;  // libopusenc, forward-declared so the header stays pulse/opus-free

/// Ogg/Opus writer via libopusenc: mono 48 kHz, VBR, VOIP application,
/// complexity 10, `.ogg` extension (accepted by every transcription API's
/// extension check).
class OpusWriter : public AudioWriter
{
public:
    explicit OpusWriter(int bitrate = 32000);
    ~OpusWriter() override;

    QString extension() const override { return QStringLiteral("ogg"); }
    bool open(const QString &path, QString *err) override;
    bool write(const int16_t *pcm, std::size_t frames, QString *err) override;
    bool finalize(QString *err) override;

private:
    int m_bitrate;
    OggOpusEnc *m_enc = nullptr;
    QString m_error;   // first write error; sticky until finalize()
};
