#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "core/OutputPath.h"

class TstOutputpath : public QObject
{
    Q_OBJECT

private slots:
    void fileName_isTimestampedWithExtension();
    void fileName_marksSystemAudio();
    void tempFor_appendsPartSuffix();
    void unique_noCollision_returnsPlainPath();
    void unique_existingFile_appendsCounterBeforeExtension();
    void unique_existingPartFile_alsoCounts();
};

void TstOutputpath::fileName_isTimestampedWithExtension()
{
    const QDateTime when(QDate(2026, 10, 20), QTime(14, 5, 9));
    QCOMPARE(OutputPath::fileName(when, false, "ogg"), QStringLiteral("Recording_2026-10-20_14-05-09.ogg"));
    QCOMPARE(OutputPath::fileName(when, false, "wav"), QStringLiteral("Recording_2026-10-20_14-05-09.wav"));
}

void TstOutputpath::fileName_marksSystemAudio()
{
    const QDateTime when(QDate(2026, 10, 20), QTime(14, 5, 9));
    QCOMPARE(OutputPath::fileName(when, true, "ogg"), QStringLiteral("Recording_2026-10-20_14-05-09_sys.ogg"));
}

void TstOutputpath::tempFor_appendsPartSuffix()
{
    QCOMPARE(OutputPath::tempFor("/home/me/Recordings/Recording_2026-10-20_14-05-09.ogg"),
             QStringLiteral("/home/me/Recordings/Recording_2026-10-20_14-05-09.ogg.part"));
}

void TstOutputpath::unique_noCollision_returnsPlainPath()
{
    QTemporaryDir dir;
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09.ogg"),
             dir.path() + QStringLiteral("/Recording_2026-10-20_14-05-09.ogg"));
}

namespace {
void touch(const QString &path)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
}
}  // namespace

void TstOutputpath::unique_existingFile_appendsCounterBeforeExtension()
{
    QTemporaryDir dir;
    touch(dir.filePath("Recording_2026-10-20_14-05-09.ogg"));
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09.ogg"),
             dir.filePath("Recording_2026-10-20_14-05-09_2.ogg"));

    touch(dir.filePath("Recording_2026-10-20_14-05-09_2.ogg"));
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09.ogg"),
             dir.filePath("Recording_2026-10-20_14-05-09_3.ogg"));

    // The counter goes after the _sys marker, before the extension.
    touch(dir.filePath("Recording_2026-10-20_14-05-09_sys.ogg"));
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09_sys.ogg"),
             dir.filePath("Recording_2026-10-20_14-05-09_sys_2.ogg"));
}

void TstOutputpath::unique_existingPartFile_alsoCounts()
{
    QTemporaryDir dir;
    // A recording in progress (or left by a crash) holds its name too.
    touch(dir.filePath("Recording_2026-10-20_14-05-09.ogg.part"));
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09.ogg"),
             dir.filePath("Recording_2026-10-20_14-05-09_2.ogg"));

    touch(dir.filePath("Recording_2026-10-20_14-05-09_2.ogg.part"));
    QCOMPARE(OutputPath::unique(dir.path(), "Recording_2026-10-20_14-05-09.ogg"),
             dir.filePath("Recording_2026-10-20_14-05-09_3.ogg"));
}

QTEST_GUILESS_MAIN(TstOutputpath)
#include "tst_outputpath.moc"
