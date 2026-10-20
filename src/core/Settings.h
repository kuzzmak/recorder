#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QJsonObject;

// Declares a Q_PROPERTY backed by a member, with an inline getter/setter.
// The matching NOTIFY signal is declared once in the signals: block below.
// Setters route through setField() so every change emits + schedules a save.
#define H_PROP(type, name, Name, defval)                                     \
    Q_PROPERTY(type name READ name WRITE set##Name NOTIFY name##Changed)     \
public:                                                                      \
    type name() const { return m_##name; }                                   \
    void set##Name(const type &v) { setField(m_##name, v, &Settings::name##Changed); } \
private:                                                                     \
    type m_##name = defval;

/// All user-facing settings. Loads/saves a single JSON file in
/// ~/.config/Recorder/settings.json; changes auto-save (debounced).
class Settings : public QObject
{
    Q_OBJECT

    H_PROP(QString, outputDir, OutputDir, defaultOutputDir())
    H_PROP(QString, format, Format, QStringLiteral("opus"))   // "opus" | "wav"
    H_PROP(bool, captureSystemAudio, CaptureSystemAudio, true)
    H_PROP(bool, launchAtLogin, LaunchAtLogin, true)
    H_PROP(int, opusBitrate, OpusBitrate, 32000)
    H_PROP(double, systemGain, SystemGain, 1.0)
    H_PROP(QString, micDevice, MicDevice, QString())          // "" => default source

public:
    explicit Settings(QObject *parent = nullptr);

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString &path) { m_filePath = path; }
    static QString defaultFilePath();
    static QString defaultOutputDir();

    void load();        ///< Read file into properties; missing file => defaults.
    void save() const;  ///< Write current values to disk now.
    void reset();       ///< Restore every field to its default and save.

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

Q_SIGNALS:
    void outputDirChanged();
    void formatChanged();
    void captureSystemAudioChanged();
    void launchAtLoginChanged();
    void opusBitrateChanged();
    void systemGainChanged();
    void micDeviceChanged();

private:
    template <typename T>
    void setField(T &field, const T &value, void (Settings::*sig)())
    {
        if (field == value)
            return;
        field = value;
        emit(this->*sig)();
        scheduleSave();
    }
    void scheduleSave();

    QString m_filePath;
    QTimer m_saveTimer;
    bool m_loading = false;
};

#undef H_PROP
