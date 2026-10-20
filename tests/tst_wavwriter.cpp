#include <QtTest>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>

#include "core/WavWriter.h"

#include <vector>

class TstWavwriter : public QObject
{
    Q_OBJECT

private slots:
    void header_isCanonical44BytePcm16Mono48k();
    void open_writesZeroSizedHeader();
    void open_missingDirectory_failsWithMessage();
    void writeFinalize_patchesSizesAndRoundTripsSamples();
    void finalize_isIdempotent();
    void repairHeader_fixesSizesOfUnfinalizedFile();
    void repairHeader_rejectsNonWavFile();
};

namespace {
// Parse the fixed fields of a 44-byte canonical header.
struct Header {
    QByteArray riff, wave, fmt, data;
    quint32 chunkSize = 0, fmtSize = 0, rate = 0, byteRate = 0, dataSize = 0;
    quint16 format = 0, channels = 0, blockAlign = 0, bits = 0;
};

Header parseHeader(const QByteArray &bytes)
{
    Header h;
    QDataStream in(bytes);
    in.setByteOrder(QDataStream::LittleEndian);
    h.riff.resize(4); in.readRawData(h.riff.data(), 4);
    in >> h.chunkSize;
    h.wave.resize(4); in.readRawData(h.wave.data(), 4);
    h.fmt.resize(4); in.readRawData(h.fmt.data(), 4);
    in >> h.fmtSize >> h.format >> h.channels >> h.rate >> h.byteRate >> h.blockAlign >> h.bits;
    h.data.resize(4); in.readRawData(h.data.data(), 4);
    in >> h.dataSize;
    return h;
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}
}  // namespace

void TstWavwriter::header_isCanonical44BytePcm16Mono48k()
{
    const QByteArray bytes = WavWriter::header(1000);
    QCOMPARE(bytes.size(), 44);
    const Header h = parseHeader(bytes);
    QCOMPARE(h.riff, QByteArray("RIFF"));
    QCOMPARE(h.chunkSize, quint32(36 + 1000));
    QCOMPARE(h.wave, QByteArray("WAVE"));
    QCOMPARE(h.fmt, QByteArray("fmt "));
    QCOMPARE(h.fmtSize, quint32(16));
    QCOMPARE(h.format, quint16(1));
    QCOMPARE(h.channels, quint16(1));
    QCOMPARE(h.rate, quint32(48000));
    QCOMPARE(h.byteRate, quint32(96000));
    QCOMPARE(h.blockAlign, quint16(2));
    QCOMPARE(h.bits, quint16(16));
    QCOMPARE(h.data, QByteArray("data"));
    QCOMPARE(h.dataSize, quint32(1000));
}

void TstWavwriter::open_writesZeroSizedHeader()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("a.wav");
    WavWriter w;
    QString err;
    QVERIFY2(w.open(path, &err), qPrintable(err));
    QCOMPARE(w.extension(), QStringLiteral("wav"));

    const QByteArray bytes = readAll(path);
    QCOMPARE(bytes.size(), 44);
    const Header h = parseHeader(bytes);
    QCOMPARE(h.riff, QByteArray("RIFF"));
    QCOMPARE(h.chunkSize, quint32(36));
    QCOMPARE(h.dataSize, quint32(0));
}

void TstWavwriter::open_missingDirectory_failsWithMessage()
{
    QTemporaryDir dir;
    WavWriter w;
    QString err;
    QVERIFY(!w.open(dir.filePath("no/such/dir/a.wav"), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!w.write(nullptr, 0, &err));  // not open
}

void TstWavwriter::writeFinalize_patchesSizesAndRoundTripsSamples()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("b.wav");
    const std::vector<int16_t> pcm = {0, 1, -1, 32767, -32768, 1234, -4321};

    WavWriter w;
    QString err;
    QVERIFY2(w.open(path, &err), qPrintable(err));
    QVERIFY2(w.write(pcm.data(), 3, &err), qPrintable(err));
    QVERIFY2(w.write(pcm.data() + 3, pcm.size() - 3, &err), qPrintable(err));
    QVERIFY2(w.finalize(&err), qPrintable(err));

    const QByteArray bytes = readAll(path);
    const int dataBytes = int(pcm.size() * 2);
    QCOMPARE(bytes.size(), 44 + dataBytes);
    const Header h = parseHeader(bytes);
    QCOMPARE(h.chunkSize, quint32(36 + dataBytes));
    QCOMPARE(h.dataSize, quint32(dataBytes));

    QDataStream in(bytes.mid(44));
    in.setByteOrder(QDataStream::LittleEndian);
    for (int16_t expected : pcm) {
        qint16 v = 0;
        in >> v;
        QCOMPARE(v, qint16(expected));
    }
}

void TstWavwriter::finalize_isIdempotent()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("c.wav");
    const int16_t pcm[] = {5, 6};

    WavWriter w;
    QString err;
    QVERIFY(w.open(path, &err));
    QVERIFY(w.write(pcm, 2, &err));
    QVERIFY(w.finalize(&err));
    const QByteArray once = readAll(path);

    QVERIFY(w.finalize(&err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readAll(path), once);
    QVERIFY(!w.write(pcm, 2, &err));  // closed
}

void TstWavwriter::repairHeader_fixesSizesOfUnfinalizedFile()
{
    // Simulate a crash: header(0) followed by samples, never finalized.
    QTemporaryDir dir;
    const QString path = dir.filePath("crashed.wav.part");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(WavWriter::header(0));
        const int16_t pcm[] = {1, 2, 3, 4, 5};
        f.write(reinterpret_cast<const char *>(pcm), sizeof(pcm));
    }
    QCOMPARE(parseHeader(readAll(path)).dataSize, quint32(0));

    QString err;
    QVERIFY2(WavWriter::repairHeader(path, &err), qPrintable(err));

    const QByteArray bytes = readAll(path);
    QCOMPARE(bytes.size(), 44 + 10);
    const Header h = parseHeader(bytes);
    QCOMPARE(h.chunkSize, quint32(36 + 10));
    QCOMPARE(h.dataSize, quint32(10));
    QCOMPARE(h.rate, quint32(48000));  // rest of the header untouched
}

void TstWavwriter::repairHeader_rejectsNonWavFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("notwav.part");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(60, 'x'));
    }
    const QByteArray before = readAll(path);

    QString err;
    QVERIFY(!WavWriter::repairHeader(path, &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(readAll(path), before);  // left untouched

    err.clear();
    QVERIFY(!WavWriter::repairHeader(dir.filePath("missing.wav"), &err));
    QVERIFY(!err.isEmpty());
}

QTEST_GUILESS_MAIN(TstWavwriter)
#include "tst_wavwriter.moc"
