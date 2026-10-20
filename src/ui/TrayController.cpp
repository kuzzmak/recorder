#include "TrayController.h"

#include "SettingsDialog.h"
#include "core/Settings.h"
#include "core/TrayHost.h"
#include "core/TrayIcon.h"

#include <QAction>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QUrl>

TrayController::TrayController(Recorder *recorder, Settings *settings, QObject *parent)
    : QObject(parent)
    , m_recorder(recorder)
    , m_settings(settings)
    , m_host(new TrayHost(this))
{
    m_menu = new QMenu;
    m_toggle = m_menu->addAction(QString(), this, &TrayController::toggleRecording);
    m_openFolder = m_menu->addAction(tr("Open recordings folder"), this,
                                     &TrayController::openRecordingsFolder);
    m_settingsAction = m_menu->addAction(tr("Settings…"), this, &TrayController::showSettings);
    m_menu->addSeparator();
    m_quit = m_menu->addAction(tr("Quit Recorder"), qApp, &QCoreApplication::quit);

    connect(m_recorder, &Recorder::stateChanged, this, [this] { refresh(); });
    connect(m_recorder, &Recorder::tick, this, [this] { refresh(); });
    connect(m_recorder, &Recorder::started, this, [this](const QString &path) {
        notify(tr("Recording started"), QFileInfo(path).fileName());
    });
    connect(m_recorder, &Recorder::stopped, this,
            [this](const QString &path, qint64 durationMs, Recorder::Outcome outcome) {
                const QString text = Recorder::outcomeText(
                    outcome, path, durationMs, m_recorder->lastResult().error);
                const int gaps = m_recorder->lastResult().stats.gapsPatched();
                const QString suffix = gaps > 0 ? tr(" (%n gap(s) patched)", nullptr, gaps) : QString();
                notify(tr("Recording stopped"), text + suffix, outcome == Recorder::Outcome::Failed);
            });
    connect(m_recorder, &Recorder::error, this, [this](const QString &message) {
        notify(tr("Recorder"), message, true);
    });

    connect(m_host, &TrayHost::readyChanged, this, [this] {
        if (m_host->ready())
            createTray();
        else
            destroyTray();
    });
    if (m_host->ready())
        createTray();
    refresh();
}

TrayController::~TrayController()
{
    destroyTray();
    delete m_menu;
    delete m_dialog;
}

void TrayController::setUseThemedIcons(bool themed)
{
    m_themedIcons = themed;
    refresh();
}

void TrayController::createTray()
{
    if (m_tray)
        return;
    m_tray = new QSystemTrayIcon(this);
    m_tray->setContextMenu(m_menu);
    // GNOME's SNI host opens the menu on either button; elsewhere make the
    // left click do the same so the behaviour is consistent.
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger && m_menu)
            m_menu->popup(QCursor::pos());
    });
    refresh();
    m_tray->show();
}

void TrayController::destroyTray()
{
    delete m_tray;
    m_tray = nullptr;
}

QIcon TrayController::icon(bool recording) const
{
    const QString name = QString::fromLatin1(recording ? TrayIcon::kRecordingName : TrayIcon::kIdleName);
    QIcon fallback(QPixmap::fromImage(TrayIcon::render(64, recording)));
    return m_themedIcons ? QIcon::fromTheme(name, fallback) : fallback;
}

QString TrayController::formatElapsed(qint64 ms)
{
    const qint64 s = ms / 1000;
    const qint64 h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
}

void TrayController::refresh()
{
    QString toggleText, tooltip;
    bool enabled = true;
    bool recording = false;
    switch (m_recorder->state()) {
    case Recorder::State::Idle:
        toggleText = tr("● Start recording");
        tooltip = tr("Recorder — idle");
        break;
    case Recorder::State::Starting:
        toggleText = tr("Starting… (click to cancel)");
        tooltip = tr("Recorder — starting");
        break;
    case Recorder::State::Recording:
        recording = true;
        toggleText = tr("■ Stop recording (%1)").arg(formatElapsed(m_recorder->elapsedMs()));
        tooltip = tr("Recorder — recording %1").arg(formatElapsed(m_recorder->elapsedMs()));
        break;
    case Recorder::State::Stopping:
        recording = true;
        toggleText = tr("Finishing…");
        tooltip = tr("Recorder — finishing");
        enabled = false;
        break;
    }
    m_toggle->setText(toggleText);
    m_toggle->setEnabled(enabled);
    if (m_tray) {
        m_tray->setIcon(icon(recording));
        m_tray->setToolTip(tooltip);
    }
}

Recorder::SessionSpec TrayController::sessionSpec() const
{
    Recorder::SessionSpec spec;
    spec.outputDir = m_settings->outputDir();
    spec.format = m_settings->format();
    spec.captureSystemAudio = m_settings->captureSystemAudio();
    spec.opusBitrate = m_settings->opusBitrate();
    spec.systemGain = m_settings->systemGain();
    spec.micDevice = m_settings->micDevice();
    return spec;
}

void TrayController::toggleRecording()
{
    if (m_recorder->state() == Recorder::State::Idle)
        m_recorder->start(sessionSpec());
    else
        m_recorder->stop();
}

void TrayController::openRecordingsFolder()
{
    QDir().mkpath(m_settings->outputDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings->outputDir()));
}

void TrayController::showSettings()
{
    if (!m_dialog) {
        m_dialog = new SettingsDialog(m_settings);
        m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
}

void TrayController::notify(const QString &title, const QString &body, bool warning)
{
    if (!m_tray)
        return;
    m_tray->showMessage(title, body,
                        warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 5000);
}
