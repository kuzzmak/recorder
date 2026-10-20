#pragma once

#include "core/Recorder.h"

#include <QIcon>
#include <QObject>
#include <QPointer>
#include <memory>

class QAction;
class QMenu;
class QSystemTrayIcon;
class Settings;
class SettingsDialog;
class TrayHost;

/// Owns the tray icon and its menu, and translates Recorder state into icon,
/// tooltip, menu text and desktop notifications.
///
/// The QSystemTrayIcon is only created while a StatusNotifier host is up
/// (TrayHost): at autostart GNOME's host appears after we do, and Qt never
/// retries registration on its own.
class TrayController : public QObject
{
    Q_OBJECT

public:
    TrayController(Recorder *recorder, Settings *settings, QObject *parent = nullptr);
    ~TrayController() override;

    /// Use the themed icon names installed by TrayIcon::installToTheme (GNOME's
    /// host only renders names); otherwise fall back to inline pixmaps.
    void setUseThemedIcons(bool themed);

    /// Session parameters for the next recording, taken from Settings.
    Recorder::SessionSpec sessionSpec() const;

    /// "12:34" / "1:02:03".
    static QString formatElapsed(qint64 ms);

public slots:
    void toggleRecording();
    void openRecordingsFolder();
    void showSettings();

private:
    void createTray();
    void destroyTray();
    void refresh();
    QIcon icon(bool recording) const;
    void notify(const QString &title, const QString &body, bool warning = false);

    Recorder *m_recorder;
    Settings *m_settings;
    TrayHost *m_host;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_toggle = nullptr;
    QAction *m_openFolder = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_quit = nullptr;
    QPointer<SettingsDialog> m_dialog;
    bool m_themedIcons = false;
};
