#include <QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/AutoStart.h"
#include "core/TrayIcon.h"

class TstAutostart : public QObject
{
    Q_OBJECT

private slots:
    // AutoStart
    void autostart_desktopEntry_hasRequiredKeys();
    void autostart_execOf_returnsFirstWordOfExecLine();
    void autostart_enableWritesEntry_disableRemoves();
    void autostart_defaults_pointAtThisAppAndConfigDir();
    void autostart_isStale_whenExecNoLongerExists();

    // TrayIcon
    void trayicon_render_whiteCentreTransparentCorners();
    void trayicon_render_tealWhenIdle_redWhenRecording();
    void trayicon_installToTheme_writesBothIconsAtThreeSizes();
    void trayicon_installToTheme_failsWhenRootUnwritable();
};

void TstAutostart::autostart_desktopEntry_hasRequiredKeys()
{
    const QString entry = AutoStart::desktopEntry("/opt/recorder/recorder");
    QVERIFY(entry.startsWith("[Desktop Entry]\n"));
    QVERIFY(entry.contains("\nType=Application\n"));
    QVERIFY(entry.contains("\nName=Recorder\n"));
    QVERIFY(entry.contains("\nComment=Meeting audio recorder\n"));
    QVERIFY(entry.contains("\nIcon=recorder\n"));
    QVERIFY(entry.contains("\nExec=/opt/recorder/recorder\n"));  // no extra flags
    QVERIFY(entry.contains("\nX-GNOME-Autostart-enabled=true\n"));
    QVERIFY(entry.contains("\nTerminal=false\n"));
}

void TstAutostart::autostart_execOf_returnsFirstWordOfExecLine()
{
    QCOMPARE(AutoStart::execOf(AutoStart::desktopEntry("/opt/recorder/recorder")),
             QStringLiteral("/opt/recorder/recorder"));
    QCOMPARE(AutoStart::execOf("[Desktop Entry]\nExec=/usr/bin/recorder --hidden %U\nTerminal=false\n"),
             QStringLiteral("/usr/bin/recorder"));
    QCOMPARE(AutoStart::execOf("[Desktop Entry]\nType=Application\n"), QString());
    QCOMPARE(AutoStart::execOf(QString()), QString());
}

void TstAutostart::autostart_enableWritesEntry_disableRemoves()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("autostart/recorder.desktop");  // folder does not exist yet

    AutoStart a;
    a.setExecPath("/opt/recorder/recorder");
    a.setEntryPath(path);
    QCOMPARE(a.entryPath(), path);

    QVERIFY(!a.isEnabled());
    QVERIFY(a.setEnabled(true));
    QVERIFY(a.isEnabled());
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), AutoStart::desktopEntry("/opt/recorder/recorder"));
    f.close();

    QVERIFY(a.setEnabled(false));
    QVERIFY(!a.isEnabled());
    QVERIFY(!QFile::exists(path));
    QVERIFY(a.setEnabled(false));  // already absent is still success
}

void TstAutostart::autostart_defaults_pointAtThisAppAndConfigDir()
{
    AutoStart a;
    QCOMPARE(a.execPath(), QCoreApplication::applicationFilePath());
    QCOMPARE(a.entryPath(),
             QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                 + QStringLiteral("/autostart/recorder.desktop"));
}

void TstAutostart::autostart_isStale_whenExecNoLongerExists()
{
    QTemporaryDir dir;
    AutoStart a;
    a.setEntryPath(dir.filePath("recorder.desktop"));

    QVERIFY(!a.isStale());  // no entry at all => nothing to repair

    // An entry for a binary that exists (this test executable) is current.
    a.setExecPath(QCoreApplication::applicationFilePath());
    QVERIFY(a.setEnabled(true));
    QVERIFY(!a.isStale());

    // An entry for a binary that has since been deleted (e.g. a wiped build
    // directory) is stale.
    const QString gone = dir.filePath("build/recorder");
    QVERIFY(QDir().mkpath(dir.filePath("build")));
    {
        QFile f(gone);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    a.setExecPath(gone);
    QVERIFY(a.setEnabled(true));
    QVERIFY(!a.isStale());
    QVERIFY(QFile::remove(gone));
    QVERIFY(a.isStale());
}

void TstAutostart::trayicon_render_whiteCentreTransparentCorners()
{
    for (bool recording : {false, true}) {
        const QImage img = TrayIcon::render(64, recording);
        QCOMPARE(img.size(), QSize(64, 64));
        QCOMPARE(img.format(), QImage::Format_ARGB32);
        QCOMPARE(img.pixelColor(32, 32), QColor(Qt::white));
        QCOMPARE(img.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(img.pixelColor(63, 63).alpha(), 0);
    }
}

void TstAutostart::trayicon_render_tealWhenIdle_redWhenRecording()
{
    // (16,32) at 64px lies inside the rounded square (inset 6) but left of the
    // centre dot (radius 9), so it shows the body colour.
    QCOMPARE(TrayIcon::render(64, false).pixelColor(16, 32), QColor("#1FA1AD"));
    QCOMPARE(TrayIcon::render(64, true).pixelColor(16, 32), QColor("#D93025"));
    // Scales with size: same relative point at 256px.
    QCOMPARE(TrayIcon::render(256, true).pixelColor(64, 128), QColor("#D93025"));
}

void TstAutostart::trayicon_installToTheme_writesBothIconsAtThreeSizes()
{
    QTemporaryDir dir;
    QVERIFY(TrayIcon::installToTheme(dir.path()));

    for (int size : {24, 48, 256}) {
        for (const char *name : {TrayIcon::kIdleName, TrayIcon::kRecordingName}) {
            const QString path = QStringLiteral("%1/hicolor/%2x%2/apps/%3.png")
                                     .arg(dir.path()).arg(size).arg(QLatin1String(name));
            QVERIFY2(QFile::exists(path), qPrintable(path));
            const QImage img(path);
            QCOMPARE(img.size(), QSize(size, size));
            QCOMPARE(img.pixelColor(size / 2, size / 2), QColor(Qt::white));
        }
    }
    // The two states are distinct files, not copies.
    const QImage idle(QStringLiteral("%1/hicolor/48x48/apps/recorder.png").arg(dir.path()));
    const QImage rec(QStringLiteral("%1/hicolor/48x48/apps/recorder-recording.png").arg(dir.path()));
    QVERIFY(idle.pixelColor(12, 24) != rec.pixelColor(12, 24));
}

void TstAutostart::trayicon_installToTheme_failsWhenRootUnwritable()
{
    QTemporaryDir dir;
    const QString blocker = dir.filePath("icons");
    {
        QFile f(blocker);  // a regular file where the icons root should go
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QVERIFY(!TrayIcon::installToTheme(blocker));
}

QTEST_GUILESS_MAIN(TstAutostart)
#include "tst_autostart.moc"
