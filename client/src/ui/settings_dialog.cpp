#include "ui/settings_dialog.hpp"

#include <algorithm>
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
///
/// The lower end is only where this dialog stops offering; the real lower bound
/// is the configured floor, which is usually higher. See the constructor.
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
  auto* audio_form = new QFormLayout(audio);
  input_ = new QComboBox(audio);
  output_ = new QComboBox(audio);
  audio_form->addRow(QStringLiteral("Microphone"), input_);
  audio_form->addRow(QStringLiteral("Output"), output_);

  auto* video = new QGroupBox(QStringLiteral("Screen"), this);
  auto* video_form = new QFormLayout(video);
  monitor_ = new QComboBox(video);

  // Not kMinBitrateKbps on its own. dv::config::validate refuses a
  // configuration whose floor sits above its minimum, and the built-in floor is
  // 300 while this dialog used to offer 200. Nothing came of that while the
  // choice lasted only as long as the program was open; now that it is written
  // to config.ini, offering 200 would let somebody save a file that stops the
  // client from starting, and nothing on screen would connect the two.
  const int lowest = std::max(kMinBitrateKbps, session_.video_floor_bitrate_kbps());

  min_bitrate_ = new QSpinBox(video);
  min_bitrate_->setRange(lowest, kMaxBitrateKbps);
  min_bitrate_->setSingleStep(kBitrateStepKbps);
  min_bitrate_->setSuffix(QStringLiteral(" kbps"));

  max_bitrate_ = new QSpinBox(video);
  max_bitrate_->setRange(lowest, kMaxBitrateKbps);
  max_bitrate_->setSingleStep(kBitrateStepKbps);
  max_bitrate_->setSuffix(QStringLiteral(" kbps"));

  video_form->addRow(QStringLiteral("Monitor"), monitor_);
  video_form->addRow(QStringLiteral("Minimum bitrate"), min_bitrate_);
  video_form->addRow(QStringLiteral("Maximum bitrate"), max_bitrate_);

  auto* note =
      new QLabel(QStringLiteral("Changes take effect immediately, including during a call."), this);
  note->setProperty("hint", true);

  // At the bottom rather than under the audio rows, because the bitrate is kept
  // in the same file. The monitor is the one thing here that is not: it is the
  // choice for the next share rather than a setting.
  storage_ = new QLabel(QString{}, this);
  storage_->setWordWrap(true);
  storage_->setProperty("hint", true);

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
  layout->addWidget(storage_);
  layout->addWidget(buttons);

  load_devices();
  load_monitors();
  show_storage();

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
}

void SettingsDialog::show_storage() {
  const std::filesystem::path file = config::user_config_file();
  storage_->setProperty("error", false);
  storage_->setText(
      file.empty()
          ? QStringLiteral("This system does not say where settings belong, so the devices and the "
                           "bitrate chosen here last only until the program closes.")
          : QStringLiteral("The devices and the bitrate are kept in %1")
                .arg(QString::fromStdString(file.string())));
  restyle();
}

void SettingsDialog::restyle() {
  // A property a stylesheet selects on only changes what is drawn once the
  // widget is asked to work out its style again.
  storage_->style()->unpolish(storage_);
  storage_->style()->polish(storage_);
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

void SettingsDialog::remember(const std::vector<config::IniSetting>& settings) {
  const std::filesystem::path file = config::user_config_file();
  if (file.empty()) {
    // Already said so at the bottom of the dialog when it opened. Saying it
    // again per change would be a growing pile of the same sentence.
    return;
  }

  const auto written = config::save_ini_settings(file, settings);
  if (written) {
    show_storage();
    return;
  }

  for (const config::IniSetting& setting : settings) {
    DV_LOG_WARN("Could not save {}.{} to {}: {}", setting.section, setting.key, file.string(),
                written.error().message);
  }
  // What was chosen is in use either way, and saying otherwise would send
  // somebody looking for a problem with their microphone.
  storage_->setProperty("error", true);
  storage_->setText(QStringLiteral("This is in use now, but could not be saved to %1: %2")
                        .arg(QString::fromStdString(file.string()),
                             QString::fromStdString(written.error().message)));
  restyle();
}

void SettingsDialog::on_input_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = input_->itemData(index).toString();
  (void)session_.set_input_device(device.toStdString());
  remember({{.section = "audio", .key = "input_device", .value = device.toStdString()}});
}

void SettingsDialog::on_output_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = output_->itemData(index).toString();
  (void)session_.set_output_device(device.toStdString());
  remember({{.section = "audio", .key = "output_device", .value = device.toStdString()}});
}

void SettingsDialog::on_bitrate_changed() {
  // The maximum cannot sit below the minimum, and rather than refusing the
  // edit the other end is moved to keep it sensible.
  if (max_bitrate_->value() < min_bitrate_->value()) {
    max_bitrate_->setValue(min_bitrate_->value());
  }
  const int minimum = min_bitrate_->value();
  const int maximum = max_bitrate_->value();
  if (const auto applied = session_.set_video_bitrate(minimum, maximum); !applied) {
    // Nothing this dialog can produce should land here - the spin boxes cannot
    // be dragged out of range and the pair is ordered above. If it does, the
    // range is not in use, and writing it down would be recording a setting
    // that is not in force.
    DV_LOG_WARN("Refused a bitrate range of {} to {} kbps: {}", minimum, maximum,
                applied.error().message);
    return;
  }

  // Both keys in one pass. They are a range: written one at a time, a file read
  // between the two writes would hold a maximum below its minimum, which is a
  // configuration that does not start.
  remember({
      {.section = "video", .key = "min_bitrate_kbps", .value = std::to_string(minimum)},
      {.section = "video", .key = "max_bitrate_kbps", .value = std::to_string(maximum)},
  });
}

QString SettingsDialog::selected_monitor() const {
  return monitor_->currentData().toString();
}

}  // namespace dv::ui
