#include <QtTest>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/AudioFormat.h"
#include "core/OpusWriter.h"

#include <cmath>
#include <vector>

class TstOpuswriter : public QObject
{
    Q_OBJECT

private slots:
    void openWriteFinalize_producesOggOpusFile();
    void open_missingDirectory_failsWithMessage();
    void write_beforeOpen_failsWithMessage();
    void finalize_isIdempotent_andDestructorFinalizes();
    void silence_oneSecondAt32kbps_isSmall();
    void bitrate_constructorArgumentChangesOutputSize();
    void ffprobe_reportsOpusMono48k();
    void write_afterIoError_staysFailed();
};

namespace {
QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// Encode `seconds` of a 440 Hz sine at `bitrate`; returns the file size.
qint64 encodeSine(const QString &path, int bitrate, int seconds);

// One 960-frame chunk of a 440 Hz sine at half scale, continuing from `phase`.
std::vector<int16_t> sineChunk(double &phase)
{
    std::vector<int16_t> pcm(AudioFormat::kFramesPerChunk);
    const double step = 2.0 * M_PI * 440.0 / AudioFormat::kRate;
    for (auto &s : pcm) {
        s = int16_t(std::lround(16000.0 * std::sin(phase)));
        phase += step;
    }
    return pcm;
}

qint64 encodeSine(const QString &path, int bitrate, int seconds)
{
    OpusWriter w(bitrate);
    QString err;
    if (!w.open(path, &err))
        return -1;
    double phase = 0;
    for (int i = 0; i < seconds * 50; ++i) {
        const auto pcm = sineChunk(phase);
        if (!w.write(pcm.data(), pcm.size(), &err))
            return -1;
    }
    if (!w.finalize(&err))
        return -1;
    return QFileInfo(path).size();
}
}  // namespace

void TstOpuswriter::openWriteFinalize_producesOggOpusFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("take.ogg");

    OpusWriter w;
    QCOMPARE(w.extension(), QStringLiteral("ogg"));
    QString err;
    QVERIFY2(w.open(path, &err), qPrintable(err));
    double phase = 0;
    for (int i = 0; i < 50; ++i) {
        const auto pcm = sineChunk(phase);
        QVERIFY2(w.write(pcm.data(), pcm.size(), &err), qPrintable(err));
    }
    QVERIFY2(w.finalize(&err), qPrintable(err));

    QVERIFY(QFile::exists(path));
    const QByteArray bytes = readAll(path);
    QVERIFY(bytes.startsWith("OggS"));
    QVERIFY(bytes.contains("OpusHead"));
    QVERIFY(bytes.contains("OpusTags"));
    QVERIFY(bytes.contains("DATE="));
    QVERIFY2(bytes.size() > 500, qPrintable(QString::number(bytes.size())));
    QVERIFY2(bytes.size() < 100 * 1024, qPrintable(QString::number(bytes.size())));
}

void TstOpuswriter::open_missingDirectory_failsWithMessage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("no/such/dir/take.ogg");

    OpusWriter w;
    QString err;
    QVERIFY(!w.open(path, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY2(err.contains(path), qPrintable(err));
    QVERIFY(!QFile::exists(path));
    // Falls out of scope un-opened: destructor must cope.
}

void TstOpuswriter::write_beforeOpen_failsWithMessage()
{
    OpusWriter w;
    const std::vector<int16_t> pcm(AudioFormat::kFramesPerChunk, 0);
    QString err;
    QVERIFY(!w.write(pcm.data(), pcm.size(), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!w.write(pcm.data(), pcm.size(), nullptr));   // null err must be tolerated
}

void TstOpuswriter::finalize_isIdempotent_andDestructorFinalizes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::vector<int16_t> pcm(AudioFormat::kFramesPerChunk, 0);
    QString err;

    const QString twice = dir.filePath("twice.ogg");
    {
        OpusWriter w;
        QVERIFY2(w.open(twice, &err), qPrintable(err));
        QVERIFY2(w.write(pcm.data(), pcm.size(), &err), qPrintable(err));
        QVERIFY2(w.finalize(&err), qPrintable(err));
        QVERIFY2(w.finalize(&err), qPrintable(err));
        QVERIFY(err.isEmpty());
        QVERIFY(!w.write(pcm.data(), pcm.size(), &err));   // closed: no further writes
    }
    QVERIFY(readAll(twice).startsWith("OggS"));

    const QString dtor = dir.filePath("dtor.ogg");
    {
        OpusWriter w;
        QVERIFY2(w.open(dtor, &err), qPrintable(err));
        QVERIFY2(w.write(pcm.data(), pcm.size(), &err), qPrintable(err));
        // No finalize: the destructor drains and closes the file.
    }
    const QByteArray bytes = readAll(dtor);
    QVERIFY(bytes.startsWith("OggS"));
    QVERIFY(bytes.contains("OpusTags"));
}

void TstOpuswriter::silence_oneSecondAt32kbps_isSmall()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("silence.ogg");
    const std::vector<int16_t> pcm(AudioFormat::kFramesPerChunk, 0);
    QString err;

    OpusWriter w;   // default 32 kbps
    QVERIFY2(w.open(path, &err), qPrintable(err));
    for (int i = 0; i < 50; ++i)   // 50 x 20 ms = 1 s
        QVERIFY2(w.write(pcm.data(), pcm.size(), &err), qPrintable(err));
    QVERIFY2(w.finalize(&err), qPrintable(err));

    const qint64 size = QFileInfo(path).size();
    QVERIFY2(size > 0 && size < 8 * 1024, qPrintable(QString::number(size)));
}

void TstOpuswriter::bitrate_constructorArgumentChangesOutputSize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const qint64 low = encodeSine(dir.filePath("low.ogg"), 16000, 2);
    const qint64 high = encodeSine(dir.filePath("high.ogg"), 96000, 2);
    QVERIFY(low > 0);
    QVERIFY2(high > low * 2, qPrintable(QStringLiteral("%1 vs %2").arg(low).arg(high)));
}

void TstOpuswriter::ffprobe_reportsOpusMono48k()
{
#if defined(__SANITIZE_THREAD__)
    // Spawning a child process trips a check inside the TSan runtime itself.
    QSKIP("process spawning is not supported under ThreadSanitizer");
#endif
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty())
        QSKIP("ffprobe not installed");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("probe.ogg");
    QVERIFY(encodeSine(path, 32000, 1) > 0);

    QProcess p;
    p.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                      QStringLiteral("-select_streams"), QStringLiteral("a:0"),
                      QStringLiteral("-show_entries"),
                      QStringLiteral("stream=codec_name,sample_rate,channels"),
                      QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1"),
                      path});
    QVERIFY(p.waitForFinished(10000));
    QCOMPARE(p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(p.exitCode(), 0);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    QVERIFY2(out.contains(QStringLiteral("codec_name=opus")), qPrintable(out));
    QVERIFY2(out.contains(QStringLiteral("sample_rate=48000")), qPrintable(out));
    QVERIFY2(out.contains(QStringLiteral("channels=1")), qPrintable(out));
}

void TstOpuswriter::write_afterIoError_staysFailed()
{
    if (!QFile::exists(QStringLiteral("/dev/full")))
        QSKIP("/dev/full not available");

    // /dev/full accepts the open but fails every write with ENOSPC, so the
    // first flushed Ogg page turns into an encoder error.
    OpusWriter w;
    QString err;
    QVERIFY2(w.open(QStringLiteral("/dev/full"), &err), qPrintable(err));

    double phase = 0;
    bool failed = false;
    for (int i = 0; i < 500 && !failed; ++i) {   // 10 s is far beyond one Ogg page
        const auto pcm = sineChunk(phase);
        failed = !w.write(pcm.data(), pcm.size(), &err);
    }
    QVERIFY(failed);
    QVERIFY(!err.isEmpty());

    // Once failed, no later write may report success.
    err.clear();
    const auto pcm = sineChunk(phase);
    QVERIFY(!w.write(pcm.data(), pcm.size(), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!w.finalize(&err));
    QVERIFY(w.finalize(&err));   // closed now: idempotent
}

QTEST_GUILESS_MAIN(TstOpuswriter)
#include "tst_opuswriter.moc"
