#pragma once

#include <QImage>
#include <QString>

/// Renders and installs the tray icons. GNOME's StatusNotifier host only
/// renders themed icon *names*, so both states are painted and installed
/// into the user's hicolor theme at launch.
class TrayIcon
{
public:
    static constexpr const char *kIdleName = "recorder";
    static constexpr const char *kRecordingName = "recorder-recording";

    /// Rounded square + white centre dot: teal when idle, red while recording.
    static QImage render(int size, bool recording);

    /// Install both icons at 24/48/256 px under
    /// <iconsRoot>/hicolor/<NxN>/apps/<name>.png. Returns false on failure.
    static bool installToTheme(const QString &iconsRoot);
};
