#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/Settings.h"

class TstSettings : public QObject
{
    Q_OBJECT

private slots:
    void defaults_areSane();
    void setter_emitsSignal();
    void setter_dedupes();
    void saveLoad_roundTrips();
    void load_missingFile_keepsDefaults();
    void load_partialFile_keepsOtherDefaults();
    void reset_restoresDefaultsAndSaves();
    void setter_autosavesAfterDebounce();
    void load_doesNotTriggerAutosave();
};

void TstSettings::defaults_areSane()
{
    Settings s;
    QCOMPARE(s.outputDir(), QDir::homePath() + QStringLiteral("/Recordings"));
    QCOMPARE(s.format(), QStringLiteral("opus"));
    QCOMPARE(s.captureSystemAudio(), true);
    QCOMPARE(s.launchAtLogin(), true);
    QCOMPARE(s.opusBitrate(), 32000);
    QCOMPARE(s.systemGain(), 1.0);
    QCOMPARE(s.micDevice(), QString());

    QCOMPARE(s.filePath(), Settings::defaultFilePath());
    QCOMPARE(Settings::defaultFilePath(),
             QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                 + QStringLiteral("/Recorder/settings.json"));
    QCOMPARE(Settings::defaultOutputDir(), s.outputDir());
}

void TstSettings::setter_emitsSignal()
{
    Settings s;
    QSignalSpy spy(&s, &Settings::opusBitrateChanged);
    s.setOpusBitrate(64000);
    QCOMPARE(s.opusBitrate(), 64000);
    QCOMPARE(spy.count(), 1);
}

void TstSettings::setter_dedupes()
{
    Settings s;
    QSignalSpy spy(&s, &Settings::captureSystemAudioChanged);
    s.setCaptureSystemAudio(true);  // already true
    QCOMPARE(spy.count(), 0);
    s.setCaptureSystemAudio(false);
    QCOMPARE(spy.count(), 1);
}

void TstSettings::saveLoad_roundTrips()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("sub/settings.json");  // save creates the folder

    {
        Settings s;
        s.setFilePath(path);
        s.setOutputDir("/tmp/rec");
        s.setFormat("wav");
        s.setCaptureSystemAudio(false);
        s.setLaunchAtLogin(false);
        s.setOpusBitrate(48000);
        s.setSystemGain(0.5);
        s.setMicDevice("alsa_input.usb");
        s.save();
    }
    QVERIFY(QFile::exists(path));

    Settings loaded;
    loaded.setFilePath(path);
    loaded.load();
    QCOMPARE(loaded.outputDir(), QStringLiteral("/tmp/rec"));
    QCOMPARE(loaded.format(), QStringLiteral("wav"));
    QCOMPARE(loaded.captureSystemAudio(), false);
    QCOMPARE(loaded.launchAtLogin(), false);
    QCOMPARE(loaded.opusBitrate(), 48000);
    QCOMPARE(loaded.systemGain(), 0.5);
    QCOMPARE(loaded.micDevice(), QStringLiteral("alsa_input.usb"));
}

void TstSettings::load_missingFile_keepsDefaults()
{
    QTemporaryDir dir;
    Settings s;
    s.setFilePath(dir.filePath("does-not-exist.json"));
    s.load();
    QCOMPARE(s.opusBitrate(), 32000);  // untouched default
    QCOMPARE(s.format(), QStringLiteral("opus"));
}

void TstSettings::load_partialFile_keepsOtherDefaults()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("partial.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"format": "wav", "systemGain": 0.25})");
    f.close();

    Settings s;
    s.setFilePath(path);
    s.load();
    QCOMPARE(s.format(), QStringLiteral("wav"));  // from file
    QCOMPARE(s.systemGain(), 0.25);                 // from file
    QCOMPARE(s.opusBitrate(), 32000);               // default preserved
    QCOMPARE(s.captureSystemAudio(), true);         // default preserved
}

void TstSettings::reset_restoresDefaultsAndSaves()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("settings.json");
    Settings s;
    s.setFilePath(path);
    s.setOpusBitrate(96000);
    s.setFormat("wav");
    s.setMicDevice("x");
    s.reset();
    QCOMPARE(s.opusBitrate(), 32000);
    QCOMPARE(s.format(), QStringLiteral("opus"));
    QCOMPARE(s.micDevice(), QString());
    QVERIFY(QFile::exists(path));  // persisted immediately

    Settings loaded;
    loaded.setFilePath(path);
    loaded.load();
    QCOMPARE(loaded.opusBitrate(), 32000);
}

void TstSettings::setter_autosavesAfterDebounce()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("settings.json");
    Settings s;
    s.setFilePath(path);
    s.setOpusBitrate(24000);
    s.setOpusBitrate(20000);  // coalesced into one write
    QVERIFY(!QFile::exists(path));  // nothing synchronous
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path), 2000);

    Settings loaded;
    loaded.setFilePath(path);
    loaded.load();
    QCOMPARE(loaded.opusBitrate(), 20000);
}

void TstSettings::load_doesNotTriggerAutosave()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("partial.json");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"format": "wav"})");
    }
    Settings s;
    s.setFilePath(path);
    s.load();
    QTest::qWait(600);  // past the debounce window
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray(R"({"format": "wav"})"));  // file untouched
}

QTEST_GUILESS_MAIN(TstSettings)
#include "tst_settings.moc"
