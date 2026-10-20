#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_filePath(defaultFilePath())
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(250);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { save(); });
}

QString Settings::defaultFilePath()
{
    // ~/.config/Recorder/settings.json. GenericConfigLocation + a fixed app
    // folder avoids the org/app double-nesting AppConfigLocation produces.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return dir + QStringLiteral("/Recorder/settings.json");
}

QString Settings::defaultOutputDir()
{
    return QDir::homePath() + QStringLiteral("/Recordings");
}

void Settings::scheduleSave()
{
    if (m_loading)
        return;
    m_saveTimer.start();  // restarts: a burst of changes becomes one write
}
QJsonObject Settings::toJson() const
{
    QJsonObject o;
    o["outputDir"] = m_outputDir;
    o["format"] = m_format;
    o["captureSystemAudio"] = m_captureSystemAudio;
    o["launchAtLogin"] = m_launchAtLogin;
    o["opusBitrate"] = m_opusBitrate;
    o["systemGain"] = m_systemGain;
    o["micDevice"] = m_micDevice;
    return o;
}

void Settings::fromJson(const QJsonObject &o)
{
    // Only assign keys present in the file; everything else keeps its default,
    // so older/partial files load forward-compatibly. Setters emit + dedupe.
    if (o.contains("outputDir")) setOutputDir(o["outputDir"].toString());
    if (o.contains("format")) setFormat(o["format"].toString());
    if (o.contains("captureSystemAudio")) setCaptureSystemAudio(o["captureSystemAudio"].toBool());
    if (o.contains("launchAtLogin")) setLaunchAtLogin(o["launchAtLogin"].toBool());
    if (o.contains("opusBitrate")) setOpusBitrate(o["opusBitrate"].toInt());
    if (o.contains("systemGain")) setSystemGain(o["systemGain"].toDouble());
    if (o.contains("micDevice")) setMicDevice(o["micDevice"].toString());
}

void Settings::load()
{
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly))
        return;  // no file yet => keep defaults
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return;

    m_loading = true;
    fromJson(doc.object());
    m_loading = false;
}

void Settings::save() const
{
    const QFileInfo fi(m_filePath);
    QDir().mkpath(fi.absolutePath());

    QFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    f.close();
}
void Settings::reset()
{
    // Pull every default from a freshly-constructed instance, then persist.
    Settings defaults;
    m_loading = true;
    fromJson(defaults.toJson());
    m_loading = false;
    save();
}
