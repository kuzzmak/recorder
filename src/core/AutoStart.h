#pragma once

#include <QObject>
#include <QString>

/// Launch-at-login via an XDG autostart entry
/// (~/.config/autostart/recorder.desktop) written by the app itself.
///
/// Policy: sync() is run by `recorder --sync-autostart` (make install), when
/// the setting is toggled, and at normal startup only when the entry is
/// missing or stale, so a dev build never overwrites an installed entry.
class AutoStart : public QObject
{
    Q_OBJECT

public:
    explicit AutoStart(QObject *parent = nullptr);

    /// Override the executable recorded in the entry (defaults to this app).
    void setExecPath(const QString &path) { m_execPath = path; }
    QString execPath() const { return m_execPath; }
    /// Override where the entry is written (tests point this at a temp dir).
    void setEntryPath(const QString &path) { m_entryPath = path; }
    QString entryPath() const;

    bool isEnabled() const;          ///< entry exists
    bool setEnabled(bool on);        ///< write / remove the entry for execPath()
    /// Entry exists but its Exec= binary no longer does (e.g. a build dir was
    /// deleted), so startup may rewrite it.
    bool isStale() const;
    /// Bring the entry in line with `enabled` for execPath().
    bool sync(bool enabled) { return setEnabled(enabled); }

    /// Contents of the .desktop autostart entry (pure, for testing).
    static QString desktopEntry(const QString &execPath);
    /// Exec= program (first word) of an entry's contents; empty if none.
    static QString execOf(const QString &entryContents);

private:
    QString defaultEntryPath() const;
    QString m_execPath;
    QString m_entryPath;   // empty => defaultEntryPath()
};
