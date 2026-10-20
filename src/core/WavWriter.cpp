#include "WavWriter.h"

#include "AudioFormat.h"

#include <QDataStream>

QByteArray WavWriter::header(quint32 dataBytes)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);

    const quint16 channels = AudioFormat::kChannels;
    const quint16 bits = 16;
    const quint16 blockAlign = channels * bits / 8;
    const quint32 rate = AudioFormat::kRate;

    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    out << quint32(16)              // fmt chunk size
        << quint16(1)               // PCM
        << channels
        << rate
        << quint32(rate * blockAlign)
        << blockAlign
        << bits;
    out.writeRawData("data", 4);
    out << dataBytes;
    return bytes;
}

bool WavWriter::open(const QString &path, QString *err)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("cannot create %1: %2").arg(path, m_file.errorString());
        return false;
    }
    const QByteArray h = header(0);
    if (m_file.write(h) != h.size() || !m_file.flush()) {
        if (err)
            *err = QStringLiteral("cannot write header to %1: %2").arg(path, m_file.errorString());
        m_file.close();
        return false;
    }
    m_dataBytes = 0;
    m_open = true;
    return true;
}
bool WavWriter::write(const int16_t *pcm, std::size_t frames, QString *err)
{
    if (!m_open) {
        if (err)
            *err = QStringLiteral("WavWriter: write() before open()");
        return false;
    }
    // Samples are stored little-endian; on every platform we target int16_t
    // already is, so the buffer goes out as-is.
    const qint64 n = qint64(frames) * AudioFormat::kBytesPerFrame;
    if (m_file.write(reinterpret_cast<const char *>(pcm), n) != n) {
        if (err)
            *err = QStringLiteral("cannot write to %1: %2").arg(m_file.fileName(), m_file.errorString());
        return false;
    }
    m_dataBytes += quint64(n);
    return true;
}

bool WavWriter::finalize(QString *err)
{
    if (!m_open)
        return true;  // idempotent
    m_open = false;

    const QByteArray h = header(quint32(m_dataBytes));
    // Only the two size fields differ from what open() wrote.
    bool ok = m_file.seek(4) && m_file.write(h.mid(4, 4)) == 4
           && m_file.seek(40) && m_file.write(h.mid(40, 4)) == 4
           && m_file.flush();
    if (!ok && err)
        *err = QStringLiteral("cannot finalize %1: %2").arg(m_file.fileName(), m_file.errorString());
    m_file.close();
    return ok;
}
bool WavWriter::repairHeader(const QString &path, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        if (err)
            *err = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
        return false;
    }
    const QByteArray magic = f.read(12);
    if (magic.size() != 12 || !magic.startsWith("RIFF") || magic.mid(8, 4) != "WAVE") {
        if (err)
            *err = QStringLiteral("%1 is not a WAV file").arg(path);
        return false;
    }
    const qint64 dataBytes = f.size() - 44;
    if (dataBytes < 0) {
        if (err)
            *err = QStringLiteral("%1 is shorter than a WAV header").arg(path);
        return false;
    }
    const QByteArray h = header(quint32(dataBytes));
    const bool ok = f.seek(4) && f.write(h.mid(4, 4)) == 4
                 && f.seek(40) && f.write(h.mid(40, 4)) == 4
                 && f.flush();
    if (!ok && err)
        *err = QStringLiteral("cannot patch %1: %2").arg(path, f.errorString());
    return ok;
}
