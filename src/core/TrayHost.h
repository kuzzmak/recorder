#pragma once

#include <QObject>

/// Tells the UI when it is safe to create the system tray icon.
///
/// On autostart, Recorder launches before gnome-shell's appindicator host has
/// claimed `org.kde.StatusNotifierWatcher` on the session bus. Qt's
/// SystemTrayIcon checks tray availability when it is first created and, finding
/// no host, silently skips StatusNotifierItem registration without a reliable
/// retry — so the icon never appears. This gate exposes a `ready`
/// flag that flips true only once a StatusNotifier host is registered, and back
/// as it comes and goes, so the tray is (re)created at the right moment.
///
/// Linux-only behaviour; everywhere else `ready` is constant true.
class TrayHost : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit TrayHost(QObject *parent = nullptr);

    bool ready() const { return m_ready; }

signals:
    void readyChanged();

private slots:
    /// Re-evaluate watcher service + host registration and update `ready`.
    /// Declared unconditionally so moc always exposes it for the D-Bus
    /// signal connection; a no-op off Linux.
    void reevaluate();

private:
    void setReady(bool ready);

#if defined(Q_OS_LINUX)
    bool m_ready = false;
#else
    bool m_ready = true;
#endif
};
