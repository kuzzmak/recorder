#pragma once

#include "AudioWriter.h"

#include <QByteArray>
#include <QFile>

/// Hand-rolled PCM16 mono 48 kHz WAV writer (44-byte canonical header).
/// open() writes a header with zero sizes, write() appends raw samples, and
/// finalize() patches the RIFF (offset 4) and data (offset 40) sizes.
class WavWriter : public AudioWriter
{
public:
    QString extension() const override { return QStringLiteral("wav"); }
    bool open(const QString &path, QString *err) override;
    bool write(const int16_t *pcm, std::size_t frames, QString *err) override;
    bool finalize(QString *err) override;

    /// The 44-byte header for `dataBytes` bytes of PCM16 mono 48 kHz (pure).
    static QByteArray header(quint32 dataBytes);
    /// Patch the sizes of a `.wav(.part)` left behind by a crash so players
    /// accept it. Returns false if the file is not a WAV we wrote.
    static bool repairHeader(const QString &path, QString *err);

private:
    QFile m_file;
    quint64 m_dataBytes = 0;
    bool m_open = false;
};
