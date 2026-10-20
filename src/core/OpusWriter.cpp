#include "OpusWriter.h"

#include "AudioFormat.h"

#include <QDateTime>
#include <QFile>

#include <opusenc.h>

OpusWriter::OpusWriter(int bitrate)
    : m_bitrate(bitrate)
{
}

OpusWriter::~OpusWriter()
{
    QString ignored;
    finalize(&ignored);
}

bool OpusWriter::open(const QString &path, QString *err)
{
    OggOpusComments *comments = ope_comments_create();
    const QByteArray date = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
    ope_comments_add(comments, "DATE", date.constData());

    int error = OPE_OK;
    const QByteArray local = QFile::encodeName(path);
    m_enc = ope_encoder_create_file(local.constData(), comments, AudioFormat::kRate,
                                    AudioFormat::kChannels, 0 /* family: mono/stereo */, &error);
    ope_comments_destroy(comments);
    if (!m_enc) {
        if (err)
            *err = QStringLiteral("Cannot create %1: %2").arg(path, QString::fromUtf8(ope_strerror(error)));
        return false;
    }

    // Tuning for speech; failures here only cost quality, never the recording.
    ope_encoder_ctl(m_enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP));
    ope_encoder_ctl(m_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    ope_encoder_ctl(m_enc, OPUS_SET_BITRATE(m_bitrate));
    ope_encoder_ctl(m_enc, OPUS_SET_VBR(1));
    ope_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(10));
    return true;
}

bool OpusWriter::write(const int16_t *pcm, std::size_t frames, QString *err)
{
    if (!m_enc) {
        if (err)
            *err = QStringLiteral("Opus writer is not open");
        return false;
    }
    // A failed page write leaves a hole in the stream, so the file is
    // broken for good: keep refusing rather than silently resuming.
    if (m_error.isEmpty()) {
        const int rc = ope_encoder_write(m_enc, pcm, int(frames));
        if (rc != OPE_OK)
            m_error = QStringLiteral("Opus encode failed: %1").arg(QString::fromUtf8(ope_strerror(rc)));
    }
    if (!m_error.isEmpty()) {
        if (err)
            *err = m_error;
        return false;
    }
    return true;
}

bool OpusWriter::finalize(QString *err)
{
    if (!m_enc)
        return true;
    const int rc = ope_encoder_drain(m_enc);
    ope_encoder_destroy(m_enc);
    m_enc = nullptr;
    QString failure = m_error;
    m_error.clear();
    if (failure.isEmpty() && rc != OPE_OK)
        failure = QStringLiteral("Opus finalize failed: %1").arg(QString::fromUtf8(ope_strerror(rc)));
    if (!failure.isEmpty()) {
        if (err)
            *err = failure;
        return false;
    }
    return true;
}
