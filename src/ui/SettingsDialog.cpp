#include "SettingsDialog.h"

#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>

SettingsDialog::SettingsDialog(Settings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Recorder settings"));
    setModal(false);

    auto *form = new QFormLayout;

    m_folder = new QLineEdit(m_settings->outputDir());
    auto *browse = new QPushButton(tr("Browse…"));
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    form->addRow(tr("Recordings folder"), folderRow);
    connect(m_folder, &QLineEdit::editingFinished, this,
            [this] { m_settings->setOutputDir(m_folder->text().trimmed()); });
    connect(browse, &QPushButton::clicked, this, &SettingsDialog::browse);

    m_format = new QComboBox;
    m_format->addItem(tr("Ogg/Opus (small, for transcription)"), QStringLiteral("opus"));
    m_format->addItem(tr("WAV (uncompressed)"), QStringLiteral("wav"));
    m_format->setCurrentIndex(qMax(0, m_format->findData(m_settings->format())));
    form->addRow(tr("Format"), m_format);
    connect(m_format, &QComboBox::currentIndexChanged, this,
            [this](int i) { m_settings->setFormat(m_format->itemData(i).toString()); });

    m_systemAudio = new QCheckBox(tr("Also record system audio (other participants)"));
    m_systemAudio->setChecked(m_settings->captureSystemAudio());
    form->addRow(QString(), m_systemAudio);
    connect(m_systemAudio, &QCheckBox::toggled, m_settings, &Settings::setCaptureSystemAudio);

    m_launchAtLogin = new QCheckBox(tr("Launch at login"));
    m_launchAtLogin->setChecked(m_settings->launchAtLogin());
    form->addRow(QString(), m_launchAtLogin);
    connect(m_launchAtLogin, &QCheckBox::toggled, m_settings, &Settings::setLaunchAtLogin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void SettingsDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose recordings folder"),
                                                          m_settings->outputDir());
    if (dir.isEmpty())
        return;
    m_folder->setText(dir);
    m_settings->setOutputDir(dir);
}
