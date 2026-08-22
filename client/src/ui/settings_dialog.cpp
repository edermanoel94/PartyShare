#include "ui/settings_dialog.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace dv::ui {
namespace {

/// Section 6 of SPEC.md suggests 1.5 to 3 Mbps. The range offered around it is
/// wide enough to be useful on a bad link and on a local network, and narrow
/// enough that nobody sets something absurd by dragging.
constexpr int kMinBitrateKbps = 200;
constexpr int kMaxBitrateKbps = 8000;
constexpr int kBitrateStepKbps = 100;

/// Fills a device box and selects the one actually in use.
///
/// Selecting it is not decoration. Before this the box was filled and left on
/// whichever device the system listed first, so a person who had chosen their
/// headset opened the dialog, read the name of a different microphone, and had
/// no way to tell that the headset was the one being captured.
void fill_devices(QComboBox* box, const Result<std::vector<client::media::AudioDevice>>& listed,
                  const QString& in_use, const QString& nothing) {
  if (!listed || listed.value().empty()) {
    box->addItem(nothing, QString{});
    box->setEnabled(false);
    return;
  }

  // First, and always offered: an empty setting means the system decides, and
  // that has to stay reachable. Without a row for it, naming a device once is
  // a decision there is no way back out of.
  box->addItem(QStringLiteral("System default"), QString{});
  for (const client::media::AudioDevice& device : listed.value()) {
    box->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
  }

  int index = box->findData(in_use);
  if (index < 0 && !in_use.isEmpty()) {
    // Configured, and not on this machine right now: an unplugged headset, or
    // a virtual device whose software is not running. Named as what it is
    // rather than quietly replaced by the system default, because it is still
    // the setting, and it is what will be used again when the thing comes back.
    box->addItem(QStringLiteral("%1  (not connected)").arg(in_use), in_use);
    index = box->count() - 1;
  }
  box->setCurrentIndex(index < 0 ? 0 : index);
}

}  // namespace

SettingsDialog::SettingsDialog(client::app::CallSession& session, QWidget* parent)
    : QDialog(parent), session_(session) {
  setWindowTitle(QStringLiteral("Settings"));
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);

  auto* audio = new QGroupBox(QStringLiteral("Audio"), this);
  auto* audio_column = new QVBoxLayout(audio);
  auto* audio_form = new QFormLayout();
  input_ = new QComboBox(audio);
  output_ = new QComboBox(audio);
  audio_form->addRow(QStringLiteral("Microphone"), input_);
  audio_form->addRow(QStringLiteral("Output"), output_);

  // Inside the audio group and not at the bottom of the dialog, because it is
  // true of these two rows and not of the ones below them.
  storage_ = new QLabel(QString{}, audio);
  storage_->setWordWrap(true);
  storage_->setProperty("hint", true);

  audio_column->addLayout(audio_form);
  audio_column->addWidget(storage_);

  auto* video = new QGroupBox(QStringLiteral("Screen"), this);
  auto* video_form = new QFormLayout(video);
  monitor_ = new QComboBox(video);

  min_bitrate_ = new QSpinBox(video);
  min_bitrate_->setRange(kMinBitrateKbps, kMaxBitrateKbps);
  min_bitrate_->setSingleStep(kBitrateStepKbps);
  min_bitrate_->setSuffix(QStringLiteral(" kbps"));

  max_bitrate_ = new QSpinBox(video);
  max_bitrate_->setRange(kMinBitrateKbps, kMaxBitrateKbps);
  max_bitrate_->setSingleStep(kBitrateStepKbps);
  max_bitrate_->setSuffix(QStringLiteral(" kbps"));

  video_form->addRow(QStringLiteral("Monitor"), monitor_);
  video_form->addRow(QStringLiteral("Minimum bitrate"), min_bitrate_);
  video_form->addRow(QStringLiteral("Maximum bitrate"), max_bitrate_);

  auto* note =
      new QLabel(QStringLiteral("Changes take effect immediately, including during a call."), this);
  note->setProperty("hint", true);

  // Named here rather than taken from QDialogButtonBox::Close, whose label
  // comes from Qt's own translations. Without a translation file loaded that
  // is the English word, sitting in the middle of an interface that is not.
  auto* buttons = new QDialogButtonBox(this);
  auto* close = buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::AcceptRole);
  close->setProperty("accent", true);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

  layout->addWidget(audio);
  layout->addWidget(video);
  layout->addWidget(note);
  layout->addWidget(buttons);

  load_devices();
  load_monitors();

  const auto [minimum, maximum] = session_.video_bitrate();
  min_bitrate_->setValue(minimum);
  max_bitrate_->setValue(maximum);

  // Connected after the initial values are in, so that filling the widgets
  // does not look like the user changing something.
  connect(input_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_input_changed);
  connect(output_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_output_changed);
  connect(min_bitrate_, &QSpinBox::editingFinished, this, &SettingsDialog::on_bitrate_changed);
  connect(max_bitrate_, &QSpinBox::editingFinished, this, &SettingsDialog::on_bitrate_changed);
}

void SettingsDialog::load_devices() {
  fill_devices(input_, client::media::input_devices(),
               QString::fromStdString(session_.input_device()),
               QStringLiteral("no microphone available"));
  fill_devices(output_, client::media::output_devices(),
               QString::fromStdString(session_.output_device()),
               QStringLiteral("no output available"));

  const std::filesystem::path file = config::user_config_file();
  storage_->setText(file.empty()
                        ? QStringLiteral("This system does not say where settings belong, so the "
                                         "devices chosen here last only until the program closes.")
                        : QStringLiteral("Kept in %1").arg(QString::fromStdString(file.string())));
}

void SettingsDialog::load_monitors() {
  const auto listed = session_.monitors();
  if (listed) {
    for (const client::video::Monitor& screen : listed.value()) {
      monitor_->addItem(QString::fromStdString(screen.name), QString::fromStdString(screen.id));
    }
  }
  if (monitor_->count() == 0) {
    monitor_->addItem(QStringLiteral("no monitor available"), QString{});
    monitor_->setEnabled(false);
  }
}

void SettingsDialog::remember(const QString& key, const QString& value) {
  const std::filesystem::path file = config::user_config_file();
  if (file.empty()) {
    // Already said so under the two rows when the dialog opened. Saying it
    // again per change would be a growing pile of the same sentence.
    return;
  }

  const auto written = config::save_ini_settings(
      file, {{.section = "audio", .key = key.toStdString(), .value = value.toStdString()}});
  if (written) {
    storage_->setProperty("error", false);
    storage_->setText(QStringLiteral("Kept in %1").arg(QString::fromStdString(file.string())));
  } else {
    DV_LOG_WARN("Could not save audio.{} to {}: {}", key.toStdString(), file.string(),
                written.error().message);
    storage_->setProperty("error", true);
    storage_->setText(QStringLiteral("This device is in use now, but could not be saved to %1: %2")
                          .arg(QString::fromStdString(file.string()),
                               QString::fromStdString(written.error().message)));
  }
  // A property a stylesheet selects on only changes what is drawn once the
  // widget is asked to work out its style again.
  storage_->style()->unpolish(storage_);
  storage_->style()->polish(storage_);
}

void SettingsDialog::on_input_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = input_->itemData(index).toString();
  (void)session_.set_input_device(device.toStdString());
  remember(QStringLiteral("input_device"), device);
}

void SettingsDialog::on_output_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = output_->itemData(index).toString();
  (void)session_.set_output_device(device.toStdString());
  remember(QStringLiteral("output_device"), device);
}

void SettingsDialog::on_bitrate_changed() {
  // The maximum cannot sit below the minimum, and rather than refusing the
  // edit the other end is moved to keep it sensible.
  if (max_bitrate_->value() < min_bitrate_->value()) {
    max_bitrate_->setValue(min_bitrate_->value());
  }
  (void)session_.set_video_bitrate(min_bitrate_->value(), max_bitrate_->value());
}

QString SettingsDialog::selected_monitor() const {
  return monitor_->currentData().toString();
}

}  // namespace dv::ui
