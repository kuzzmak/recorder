#include "AutoStart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

AutoStart::AutoStart(QObject *parent)
    : QObject(parent)
    , m_execPath(QCoreApplication::applicationFilePath())
{
}

QString AutoStart::defaultEntryPath() const
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return config + QStringLiteral("/autostart/recorder.desktop");
}

QString AutoStart::entryPath() const
{
    return m_entryPath.isEmpty() ? defaultEntryPath() : m_entryPath;
}

bool AutoStart::isEnabled() const
{
    return QFileInfo::exists(entryPath());
}

bool AutoStart::setEnabled(bool on)
{
    const QString path = entryPath();
    if (!on) {
        if (!QFileInfo::exists(path))
            return true;
        return QFile::remove(path);
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream(&f) << desktopEntry(m_execPath);
    f.close();
    return true;
}
bool AutoStart::isStale() const
{
    QFile f(entryPath());
    if (!f.open(QIODevice::ReadOnly))
        return false;  // no entry => nothing to repair
    const QString exec = execOf(QString::fromUtf8(f.readAll()));
    return !QFileInfo::exists(exec);
}
QString AutoStart::desktopEntry(const QString &execPath)
{
    return QStringLiteral(
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=Recorder\n"
               "Comment=Meeting audio recorder\n"
               "Icon=recorder\n"
               "Exec=%1\n"
               "X-GNOME-Autostart-enabled=true\n"
               "Terminal=false\n")
        .arg(execPath);
}
QString AutoStart::execOf(const QString &entryContents)
{
    const QStringList lines = entryContents.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1String("Exec=")))
            continue;
        const QString value = line.mid(5).trimmed();
        const int space = value.indexOf(QLatin1Char(' '));
        return space < 0 ? value : value.left(space);
    }
    return QString();
}
