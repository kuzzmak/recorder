#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class Settings;

/// Non-modal settings window. Every control writes straight into Settings
/// (which auto-saves); changes apply to the next recording.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(Settings *settings, QWidget *parent = nullptr);

private:
    void browse();

    Settings *m_settings;
    QLineEdit *m_folder;
    QComboBox *m_format;
    QCheckBox *m_systemAudio;
    QCheckBox *m_launchAtLogin;
};
