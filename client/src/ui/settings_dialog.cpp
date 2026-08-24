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
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "video/screen_quality.hpp"

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

  // Off, which is what makes valueChanged usable below. With tracking on, a
  // spin box reports every keystroke, so typing 2000 into it announces 2, then
  // 20, then 200 - three bitrates nobody asked for, each one applied to a live
  // call and written down as a choice. Off, it reports when an arrow is
  // pressed and when a typed number is committed, which is the same list of
  // moments a person would call "I changed it".
  for (QSpinBox* box : {min_bitrate_, max_bitrate_}) {
    box->setKeyboardTracking(false);
  }

  resolution_ = new QComboBox(video);
  frame_rate_ = new QComboBox(video);

  quality_hint_ = new QLabel(QString{}, video);
  quality_hint_->setWordWrap(true);
  quality_hint_->setProperty("hint", true);

  video_form->addRow(QStringLiteral("Monitor"), monitor_);
  video_form->addRow(QStringLiteral("Resolution"), resolution_);
  video_form->addRow(QStringLiteral("Frame rate"), frame_rate_);
  video_form->addRow(QStringLiteral("Minimum bitrate"), min_bitrate_);
  video_form->addRow(QStringLiteral("Maximum bitrate"), max_bitrate_);
  // Spanning the form rather than in the value column, because it is a
  // sentence and the value column is as wide as a spin box.
  video_form->addRow(quality_hint_);

  auto* note =
      new QLabel(QStringLiteral("Changes take effect immediately, including during a call."), this);
  note->setProperty("hint", true);

  // At the bottom rather than under the audio rows, because the bitrate is kept
  // in the same file. The monitor is the one thing here that is not: it is the
  // choice for the next share rather than a setting.
  storage_ = new QLabel(QString{}, this);
  storage_->setWordWrap(true);
  storage_->setProperty("hint", true);

  // Named here rather than taken from QDialogButtonBox::Save and ::Close,
  // whose labels come from Qt's own translations. Without a translation file
  // loaded those are the English words, sitting in the middle of an interface
  // that is not.
  auto* buttons = new QDialogButtonBox(this);

  // ApplyRole and not AcceptRole. A button in the accept role makes
  // QDialogButtonBox emit accepted(), which closes the dialog, and a Save that
  // closes the window is a Save nobody can press twice - which is exactly what
  // somebody whose first attempt failed needs to do.
  save_ = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::ApplyRole);
  save_->setProperty("accent", true);
  save_->setEnabled(false);
  connect(save_, &QPushButton::clicked, this, &SettingsDialog::on_save);

  auto* close = buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
  // Through reject() rather than accept(), so that this button, the escape key
  // and the window's own close box all arrive at done() by the same route.
  connect(close, &QPushButton::clicked, this, &QDialog::reject);

  layout->addWidget(audio);
  layout->addWidget(video);
  layout->addWidget(note);
  layout->addWidget(storage_);
  layout->addWidget(buttons);

  load_devices();
  load_monitors();
  load_quality();
  show_storage();

  const auto [minimum, maximum] = session_.video_bitrate();
  min_bitrate_->setValue(minimum);
  max_bitrate_->setValue(maximum);

  // After the bitrate is in, because what it says is the two compared.
  show_quality_hint();

  // Connected after the initial values are in, so that filling the widgets
  // does not look like the user changing something.
  connect(input_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_input_changed);
  connect(output_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_output_changed);
  // valueChanged and not editingFinished, which only fires when the box gives
  // up the focus. Pressing the arrow moved the number on screen and nothing
  // else: the call kept the old rate, and the line at the bottom went on
  // saying everything was saved, over a box showing a figure that was neither
  // in use nor written down. Whoever pressed it once and looked at the result
  // had every reason to believe it had taken.
  connect(min_bitrate_, &QSpinBox::valueChanged, this, &SettingsDialog::on_bitrate_changed);
  connect(max_bitrate_, &QSpinBox::valueChanged, this, &SettingsDialog::on_bitrate_changed);
  connect(resolution_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_quality_changed);
  connect(frame_rate_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_quality_changed);
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

  if (file.empty()) {
    save_->setEnabled(false);
    storage_->setText(
        QStringLiteral("This system does not say where settings belong, so the devices, the "
                       "bitrate and the screen quality chosen here last only until the program "
                       "closes."));
    restyle();
    return;
  }

  // The button is the state. Enabled means there is something outstanding, and
  // greyed out means everything on screen is also on disk, which saves the
  // dialog a sentence saying so.
  save_->setEnabled(!pending_.empty());

  const QString where = QString::fromStdString(file.string());
  if (pending_.empty()) {
    storage_->setText(
        QStringLiteral("The devices, the bitrate and the screen quality are kept in %1")
            .arg(where));
  } else if (pending_.size() == 1) {
    storage_->setText(
        QStringLiteral("One change is in use now and is not in %1 yet. Save puts it there.")
            .arg(where));
  } else {
    storage_->setText(
        QStringLiteral("%1 changes are in use now and are not in %2 yet. Save puts them there.")
            .arg(pending_.size())
            .arg(where));
  }
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

void SettingsDialog::load_quality() {
  const client::video::ScreenCaptureOptions quality = session_.video_quality();

  for (const client::video::ScreenResolution& row : client::video::kScreenResolutions) {
    resolution_->addItem(QString::fromUtf8(row.label.data(), qsizetype(row.label.size())),
                         QSize(row.size.width, row.size.height));
  }
  const QSize in_use(quality.max_size.width, quality.max_size.height);
  int index = resolution_->findData(in_use);
  if (index < 0) {
    // Configured to something this dialog does not offer. Shown as the size it
    // is, so that opening settings does not read as "you are on 720p".
    resolution_->addItem(QStringLiteral("%1x%2").arg(in_use.width()).arg(in_use.height()), in_use);
    index = resolution_->count() - 1;
  }
  resolution_->setCurrentIndex(index);

  for (const int fps : client::video::kScreenFrameRates) {
    frame_rate_->addItem(QStringLiteral("%1 fps").arg(fps), fps);
  }
  index = frame_rate_->findData(quality.max_fps);
  if (index < 0) {
    frame_rate_->addItem(QStringLiteral("%1 fps").arg(quality.max_fps), quality.max_fps);
    index = frame_rate_->count() - 1;
  }
  frame_rate_->setCurrentIndex(index);
}

void SettingsDialog::show_quality_hint() {
  const QSize size = resolution_->currentData().toSize();
  const int fps = frame_rate_->currentData().toInt();
  const int wants = client::video::recommended_max_bitrate_kbps(
      {.width = size.width(), .height = size.height()}, fps);

  if (wants <= max_bitrate_->value()) {
    // The common case, and the one that has nothing to say. An empty label
    // rather than a hidden one, so the rows below do not jump as the quality
    // is changed.
    quality_hint_->clear();
    return;
  }

  quality_hint_->setText(
      QStringLiteral("%1 at %2 fps is worth about %3 kbps. The maximum below is %4, so this will "
                     "be sent softer than it could be.")
          .arg(resolution_->currentText())
          .arg(fps)
          .arg(wants)
          .arg(max_bitrate_->value()));
}

void SettingsDialog::stage(const std::vector<config::IniSetting>& settings) {
  if (config::user_config_file().empty()) {
    // The dialog said at the bottom, when it opened, that there is nowhere on
    // this system to keep these. Collecting them anyway would light up a Save
    // button with no file to write to.
    return;
  }

  for (const config::IniSetting& setting : settings) {
    const auto same_key = [&setting](const config::IniSetting& staged) {
      return staged.section == setting.section && staged.key == setting.key;
    };
    const auto found = std::find_if(pending_.begin(), pending_.end(), same_key);
    if (found != pending_.end()) {
      found->value = setting.value;
      continue;
    }
    pending_.push_back(setting);
  }
  show_storage();
}

void SettingsDialog::on_save() {
  const std::filesystem::path file = config::user_config_file();
  if (pending_.empty() || file.empty()) {
    return;
  }

  // Every pending setting in one pass, which is the whole reason they are
  // collected. save_ini_settings parses and validates the result before it
  // replaces anything, so a half written range - a maximum below its minimum -
  // is refused as a file rather than left on disk as one.
  const auto written = config::save_ini_settings(file, pending_);
  if (written) {
    pending_.clear();
    show_storage();
    return;
  }

  for (const config::IniSetting& setting : pending_) {
    DV_LOG_WARN("Could not save {}.{} to {}: {}", setting.section, setting.key, file.string(),
                written.error().message);
  }

  // Kept rather than dropped. A write fails for reasons that go away - a full
  // disk, a file held open by something else - and throwing the list out would
  // mean the only way to try again was to make every change a second time.
  //
  // What was chosen is in use either way, and saying otherwise would send
  // somebody looking for a problem with their microphone.
  storage_->setProperty("error", true);
  storage_->setText(QStringLiteral("This is in use now, but could not be saved to %1: %2")
                        .arg(QString::fromStdString(file.string()),
                             QString::fromStdString(written.error().message)));
  restyle();
  save_->setEnabled(true);
}

void SettingsDialog::done(int result) {
  if (pending_.empty()) {
    QDialog::done(result);
    return;
  }

  // Asked rather than decided. Saving on the way out keeps settings somebody
  // was only trying out; discarding on the way out loses settings somebody
  // meant. Neither is safe to guess, and the question costs one keystroke.
  const QMessageBox::StandardButton answer = QMessageBox::question(
      this, QStringLiteral("Save your settings?"),
      QStringLiteral("What you changed is in use now, but it has not been written to %1. Close "
                     "without saving and it lasts until PartyShare closes.")
          .arg(QString::fromStdString(config::user_config_file().string())),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

  if (answer == QMessageBox::Cancel) {
    return;
  }

  if (answer == QMessageBox::Save) {
    on_save();
    if (!pending_.empty()) {
      // The write failed, and on_save has just put the reason on the storage
      // line. Closing now would take that sentence off the screen along with
      // the only chance to do anything about it.
      return;
    }
  }

  QDialog::done(result);
}

void SettingsDialog::on_input_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = input_->itemData(index).toString();
  (void)session_.set_input_device(device.toStdString());
  stage({{.section = "audio", .key = "input_device", .value = device.toStdString()}});
}

void SettingsDialog::on_output_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString device = output_->itemData(index).toString();
  (void)session_.set_output_device(device.toStdString());
  stage({{.section = "audio", .key = "output_device", .value = device.toStdString()}});
}

void SettingsDialog::on_bitrate_changed() {
  // The maximum cannot sit below the minimum, and rather than refusing the
  // edit the other end is moved to keep it sensible.
  //
  // Quietly, now that this runs off valueChanged: moving the other box would
  // otherwise come straight back here as a change of its own, and the range
  // would be applied and staged twice for one thing the person did.
  if (max_bitrate_->value() < min_bitrate_->value()) {
    const QSignalBlocker quiet(max_bitrate_);
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

  // Raising the ceiling can be what the quality above was waiting for, and
  // lowering it can be what makes the quality above worth a word.
  show_quality_hint();

  // Both keys in one pass. They are a range: written one at a time, a file read
  // between the two writes would hold a maximum below its minimum, which is a
  // configuration that does not start.
  stage({
      {.section = "video", .key = "min_bitrate_kbps", .value = std::to_string(minimum)},
      {.section = "video", .key = "max_bitrate_kbps", .value = std::to_string(maximum)},
  });
}

void SettingsDialog::on_quality_changed() {
  const QSize chosen = resolution_->currentData().toSize();
  const int fps = frame_rate_->currentData().toInt();
  const client::video::Size size{.width = chosen.width(), .height = chosen.height()};

  if (const auto applied = session_.set_video_quality(size, fps); !applied) {
    // Reachable, unlike the bitrate case: during a share this restarts the
    // capture, and a monitor that was unplugged between opening this dialog
    // and changing the row does not come back.
    DV_LOG_WARN("Refused a screen quality of {}x{} at {} fps: {}", size.width, size.height, fps,
                applied.error().message);

    // Back onto what is actually being sent. Signals blocked, or putting them
    // back arrives as another change and asks the session all over again.
    const client::video::ScreenCaptureOptions in_use = session_.video_quality();
    const QSignalBlocker resolution_quiet(resolution_);
    const QSignalBlocker frame_rate_quiet(frame_rate_);
    resolution_->setCurrentIndex(
        std::max(0, resolution_->findData(QSize(in_use.max_size.width, in_use.max_size.height))));
    frame_rate_->setCurrentIndex(std::max(0, frame_rate_->findData(in_use.max_fps)));
    show_quality_hint();
    return;
  }

  show_quality_hint();

  // All three in one pass, for the reason the bitrate pair is: a file read
  // between two of the writes would hold a width from one choice and a height
  // from another, which is a shape no monitor has.
  stage({
      {.section = "video", .key = "width", .value = std::to_string(size.width)},
      {.section = "video", .key = "height", .value = std::to_string(size.height)},
      {.section = "video", .key = "fps", .value = std::to_string(fps)},
  });
}

QString SettingsDialog::selected_monitor() const {
  return monitor_->currentData().toString();
}

}  // namespace dv::ui
