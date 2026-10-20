// Wiring only: settings, autostart policy, single instance, icons, the
// Recorder with its real backends, the tray UI and process signals.

#include "core/AutoStart.h"
#include "core/OpusWriter.h"
#include "core/OutputPath.h"
#include "core/PulseCapture.h"
#include "core/Recorder.h"
#include "core/Settings.h"
#include "core/TrayIcon.h"
#include "core/WavWriter.h"
#include "ui/TrayController.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLockFile>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QtDebug>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

// --- SIGINT / SIGTERM -> QCoreApplication::quit() via a self-pipe -----------
int g_signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = 1;
    const ssize_t r = ::write(g_signalPipe[1], &byte, 1);
    (void)r;
}

void installSignalHandlers(QObject *parent)
{
    if (::pipe(g_signalPipe) != 0)
        return;
    auto *notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, parent);
    QObject::connect(notifier, &QSocketNotifier::activated, parent, [] {
        char byte;
        const ssize_t r = ::read(g_signalPipe[0], &byte, 1);
        (void)r;
        QCoreApplication::quit();
    });
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// --- Recover `.part` files left behind by a crash ---------------------------
void sweepPartFiles(const QString &dir)
{
    const QDir d(dir);
    for (const QFileInfo &fi : d.entryInfoList({QStringLiteral("*.part")}, QDir::Files)) {
        const QString part = fi.absoluteFilePath();
        QString finalName = fi.fileName();
        finalName.chop(5);  // ".part"
        if (finalName.endsWith(QStringLiteral(".wav"))) {
            QString err;
            if (!WavWriter::repairHeader(part, &err)) {
                qWarning() << "Could not repair" << part << err;
                continue;
            }
        }
        const QString target = OutputPath::unique(dir, finalName);
        if (!QFile::rename(part, target))
            qWarning() << "Could not rename" << part << "to" << target;
    }
}

// --- Install the themed tray icons into the user's hicolor theme ------------
bool installIcons()
{
    // Packaged install: /usr/share/icons/hicolor already has both names (and
    // the hicolor-icon-theme trigger refreshed its cache), so write nothing.
    // Caveat: old per-user copies in ~/.local/share/icons also satisfy this
    // check and shadow the system ones; the pixels are identical today.
    if (QIcon::hasThemeIcon(QLatin1String(TrayIcon::kIdleName))
        && QIcon::hasThemeIcon(QLatin1String(TrayIcon::kRecordingName)))
        return true;

    const QString iconsRoot =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/icons");
    if (!TrayIcon::installToTheme(iconsRoot))
        return false;
    // A running gnome-shell only re-resolves themed names after the hicolor
    // cache changes; without this a first install shows a placeholder until
    // the next shell restart. Best effort.
    QProcess::execute(QStringLiteral("gtk-update-icon-cache"),
                      {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("-f"),
                       iconsRoot + QStringLiteral("/hicolor")});
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    // Before QApplication: without a display Qt aborts in the constructor,
    // and --version must work from scripts, package tooling and headless
    // builders.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("recorder %s\n", RECORDER_VERSION);
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Recorder"));
    QApplication::setApplicationName(QStringLiteral("Recorder"));
    QApplication::setApplicationVersion(QStringLiteral(RECORDER_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);  // tray app: outlive windows

    const QStringList args = app.arguments();
    Settings settings;
    settings.load();

    // `make install` runs this against the installed binary so the autostart
    // entry points at it, without a running instance, a lock or a GUI.
    if (args.contains(QStringLiteral("--sync-autostart"))) {
        AutoStart autoStart;
        return autoStart.sync(settings.launchAtLogin()) ? 0 : 1;
    }

    // Dev tool: write the hicolor PNGs that the package installs
    // (`make icons` runs this offscreen into data/icons).
    const int exportIdx = args.indexOf(QStringLiteral("--export-icons"));
    if (exportIdx >= 0 && exportIdx + 1 < args.size())
        return TrayIcon::installToTheme(args.at(exportIdx + 1)) ? 0 : 1;

    const QString configDir = QFileInfo(Settings::defaultFilePath()).absolutePath();
    QDir().mkpath(configDir);
    QLockFile lock(configDir + QStringLiteral("/recorder.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        qInfo() << "Recorder is already running.";
        return 0;
    }

    const bool themedIcons = installIcons();
    app.setWindowIcon(QIcon::fromTheme(QString::fromLatin1(TrayIcon::kIdleName),
                                       QIcon(QPixmap::fromImage(TrayIcon::render(256, false)))));

    // Autostart policy: a dev build never overwrites an installed entry; it
    // only writes one when the entry is missing or points at a dead binary.
    AutoStart autoStart;
    if (!args.contains(QStringLiteral("--no-autostart-sync"))
        && (!autoStart.isEnabled() || autoStart.isStale()))
        autoStart.sync(settings.launchAtLogin());
    QObject::connect(&settings, &Settings::launchAtLoginChanged, &autoStart,
                     [&] { autoStart.sync(settings.launchAtLogin()); });

    sweepPartFiles(settings.outputDir());

    Recorder recorder(
        [](const Recorder::Queues &queues, const Recorder::SessionSpec &spec) -> std::unique_ptr<AudioSource> {
            return std::make_unique<PulseCapture>(queues.mic, queues.monitor, spec.micDevice);
        },
        [](const Recorder::SessionSpec &spec) -> std::unique_ptr<AudioWriter> {
            if (spec.format == QLatin1String("wav"))
                return std::make_unique<WavWriter>();
            return std::make_unique<OpusWriter>(spec.opusBitrate);
        });

    TrayController tray(&recorder, &settings);
    tray.setUseThemedIcons(themedIcons);

    installSignalHandlers(&app);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &recorder, [&] { recorder.shutdown(5000); });

    // Dev hook: --record-seconds N records once without touching the tray.
    const int idx = args.indexOf(QStringLiteral("--record-seconds"));
    if (idx >= 0 && idx + 1 < args.size()) {
        const int seconds = args.at(idx + 1).toInt();
        QObject::connect(&recorder, &Recorder::stopped, &app,
                         [&](const QString &path, qint64 ms, Recorder::Outcome outcome) {
                             const CaptureResult &r = recorder.lastResult();
                             qInfo().noquote() << Recorder::outcomeText(outcome, path, ms, r.error);
                             qInfo().noquote()
                                 << QStringLiteral("alignment=%1 frames=%2 micTrim=%3 gaps=%4 "
                                                   "lateDropped=%5 driftPad=%6 driftDrop=%7")
                                        .arg(r.stats.alignment).arg(r.framesWritten).arg(r.stats.micTrim)
                                        .arg(r.stats.gapsPatched()).arg(r.stats.lateDroppedFrames())
                                        .arg(r.stats.driftPadFrames).arg(r.stats.driftDropFrames);
                             QCoreApplication::quit();
                         });
        QObject::connect(&recorder, &Recorder::error, &app,
                         [](const QString &m) { qWarning().noquote() << "error:" << m; });
        QTimer::singleShot(0, &recorder, [&] {
            if (!recorder.start(tray.sessionSpec()))
                QCoreApplication::exit(1);
        });
        QTimer::singleShot(seconds * 1000, &recorder, &Recorder::stop);
    }

    return app.exec();
}
