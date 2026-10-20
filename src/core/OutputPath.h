#pragma once

#include <QDateTime>
#include <QString>

/// Naming of recordings: `Recording_yyyy-MM-dd_HH-mm-ss[_sys].<ext>` in the
/// output folder, written to `<name>.part` first and renamed on success.
class OutputPath
{
public:
    static QString fileName(const QDateTime &when, bool systemAudio, const QString &ext);
    /// `dir/fileName`, or `dir/stem_2.ext`, `_3`… if `name.ext` or
    /// `name.ext.part` already exists.
    static QString unique(const QString &dir, const QString &fileName);
    static QString tempFor(const QString &finalPath);
};
