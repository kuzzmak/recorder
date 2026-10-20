#include "OutputPath.h"

#include <QDir>
#include <QFileInfo>

QString OutputPath::fileName(const QDateTime &when, bool systemAudio, const QString &ext)
{
    return QStringLiteral("Recording_%1%2.%3")
        .arg(when.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")),
             systemAudio ? QStringLiteral("_sys") : QString(),
             ext);
}

QString OutputPath::unique(const QString &dir, const QString &fileName)
{
    const QDir d(dir);
    const auto taken = [](const QString &path) {
        return QFileInfo::exists(path) || QFileInfo::exists(tempFor(path));
    };

    QString candidate = d.filePath(fileName);
    if (!taken(candidate))
        return candidate;

    // "stem.ext" => "stem_N.ext"; the suffix lands after any "_sys" marker.
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot < 0 ? fileName : fileName.left(dot);
    const QString ext = dot < 0 ? QString() : fileName.mid(dot);
    for (int n = 2;; ++n) {
        candidate = d.filePath(QStringLiteral("%1_%2%3").arg(stem).arg(n).arg(ext));
        if (!taken(candidate))
            return candidate;
    }
}
QString OutputPath::tempFor(const QString &finalPath)
{
    return finalPath + QStringLiteral(".part");
}
