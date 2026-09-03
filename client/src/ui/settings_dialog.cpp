#include "ui/settings_dialog.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "audio/screen_audio_mixer.hpp"
#include "ui/chimes.hpp"
#include "ui/update_checker.hpp"
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

/// Makes a widget pick up a change to a property the stylesheet selects on.
///
/// A property is not a repaint: the widget goes on being drawn the way it was
/// until it is asked to work its style out again.
void restyle_for_property(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

}  // namespace

SettingsDialog::SettingsDialog(client::app::CallSession& session, UpdateChecker& updates,
                               QWidget* parent)
    : QDialog(parent), session_(session), updates_(updates) {
  setWindowTitle(QStringLiteral("Settings"));
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);

  // First, above the devices, because it is the only row here that has to be
  // right before anything else works. A microphone chosen against the wrong
  // server is a microphone in a call that never happens.
  auto* connection = new QGroupBox(QStringLiteral("Connection"), this);
  auto* connection_form = new QFormLayout(connection);
  signaling_url_ = new QLineEdit(connection);
  signaling_url_->setPlaceholderText(QStringLiteral("ws://127.0.0.1:8080"));
  signaling_url_->setText(QString::fromStdString(session_.signaling_url()));
  signaling_hint_ = new QLabel(QString{}, connection);
  signaling_hint_->setWordWrap(true);
  signaling_hint_->setProperty("hint", true);
  // Its state comes from the checker itself and not from the configuration
  // read at startup, for the reason the room chime's box gives below: main()
  // has already put one into the other, and asking the thing that actually
  // decides means the box cannot disagree with what will happen next.
  update_checks_ = new QCheckBox(QStringLiteral("Check GitHub for new versions"), connection);
  update_checks_->setChecked(updates_.enabled());
  update_checks_->setToolTip(
      QStringLiteral("One request to github.com, shortly after this window opens and every few "
                     "hours after that. Nothing is downloaded and nothing is installed: a newer "
                     "release becomes a link beside the version in the status bar."));

  connection_form->addRow(QStringLiteral("Server"), signaling_url_);
  connection_form->addRow(signaling_hint_);
  connection_form->addRow(QStringLiteral("Updates"), update_checks_);

  auto* audio = new QGroupBox(QStringLiteral("Audio"), this);
  auto* audio_form = new QFormLayout(audio);
  input_ = new QComboBox(audio);
  output_ = new QComboBox(audio);
  // Off first, then the four levels libwebrtc offers, each named with what it
  // takes off the noise. The data is what the configuration and CallSession
  // call these, so nothing is translated between the box, the file and the
  // module; "off" is the one word the file spells as a separate key.
  noise_suppression_ = new QComboBox(audio);
  noise_suppression_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
  noise_suppression_->addItem(QStringLiteral("Low, 6 dB"), QStringLiteral("low"));
  noise_suppression_->addItem(QStringLiteral("Moderate, 12 dB"), QStringLiteral("moderate"));
  noise_suppression_->addItem(QStringLiteral("High, 18 dB"), QStringLiteral("high"));
  noise_suppression_->addItem(QStringLiteral("Very high, 21 dB"), QStringLiteral("very_high"));
  {
    const QString current = session_.noise_suppression()
                                ? QString::fromUtf8(std::string(
                                      client::media::to_string(session_.noise_suppression_level())))
                                : QStringLiteral("off");
    noise_suppression_->setCurrentIndex(std::max(0, noise_suppression_->findData(current)));
  }

  // Its state comes from ui::chimes_enabled and not from the configuration
  // that was read at startup: main() has already put one into the other, and
  // asking the thing that actually decides means the box cannot disagree with
  // what the next arrival will do.
  room_sounds_ = new QCheckBox(QStringLiteral("Play a sound when somebody joins or leaves"), audio);
  room_sounds_->setChecked(chimes_enabled());

  audio_form->addRow(QStringLiteral("Microphone"), input_);
  audio_form->addRow(QStringLiteral("Output"), output_);
  audio_form->addRow(QStringLiteral("Noise suppression"), noise_suppression_);
  audio_form->addRow(QStringLiteral("Room sounds"), room_sounds_);

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

  // Named for what it does rather than "Automatic", which on its own leaves
  // the reader to guess what it is automatic about.
  auto_bitrate_ =
      new QCheckBox(QStringLiteral("Choose the bitrate from the resolution and frame rate"), video);

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

  screen_audio_ = new QComboBox(video);
  // The data is what config and CallSession call these, so nothing has to be
  // translated between the box, the file and the capture.
  screen_audio_->addItem(QStringLiteral("None"), QStringLiteral("none"));
  screen_audio_->addItem(QStringLiteral("Everything but PartyShare"), QStringLiteral("system"));
  screen_audio_->addItem(QStringLiteral("One application"), QStringLiteral("process"));

  audio_source_ = new QComboBox(video);

  screen_volume_label_ = new QLabel(QString{}, video);
  screen_volume_label_->setProperty("hint", true);
  screen_volume_ = new QSlider(Qt::Horizontal, video);
  // The same range as the participant volume in the room, and for a different
  // reason: there the ceiling is what WebRTC accepts for playback, here it is
  // audio::kMaxScreenVolumePercent, which is what the mixer will clamp to. The
  // two agreeing at 200 is a coincidence worth not building on.
  screen_volume_->setRange(0, client::audio::kMaxScreenVolumePercent);
  screen_volume_->setValue(session_.screen_audio_volume());
  // A page's worth is a tenth of the range, so clicking the groove or pressing
  // Page Up moves in tens rather than in ones. Dragging and the arrow keys stay
  // on single percentage points, which is the resolution the mixer has anyway.
  screen_volume_->setPageStep(20);

  screen_audio_hint_ = new QLabel(QString{}, video);
  screen_audio_hint_->setWordWrap(true);
  screen_audio_hint_->setProperty("hint", true);

  video_form->addRow(QStringLiteral("Monitor"), monitor_);
  video_form->addRow(QStringLiteral("Resolution"), resolution_);
  video_form->addRow(QStringLiteral("Frame rate"), frame_rate_);
  // Above the two boxes it governs, because it decides whether they can be
  // touched at all, and a switch read after the thing it disables is a switch
  // read too late.
  video_form->addRow(QStringLiteral("Bitrate"), auto_bitrate_);
  video_form->addRow(QStringLiteral("Minimum bitrate"), min_bitrate_);
  video_form->addRow(QStringLiteral("Maximum bitrate"), max_bitrate_);
  // Under the picture settings, because it is part of what a share carries
  // rather than a property of this machine's sound.
  video_form->addRow(QStringLiteral("Share sound"), screen_audio_);
  video_form->addRow(QStringLiteral("Application"), audio_source_);
  // The label spans the form and the slider takes the value column under it,
  // for the reason the room's volume slider is stacked the same way: a label
  // that reads as a sentence and a slider wide enough to aim at do not fit on
  // one row of a form.
  video_form->addRow(screen_volume_label_);
  video_form->addRow(QStringLiteral("Shared volume"), screen_volume_);
  video_form->addRow(screen_audio_hint_);
  // Spanning the form rather than in the value column, because it is a
  // sentence and the value column is as wide as a spin box.
  video_form->addRow(quality_hint_);

  auto* note = new QLabel(
      QStringLiteral("Changes take effect immediately, including during a call. The server "
                     "above is the exception, for the reason written beside it."),
      this);
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

  layout->addWidget(connection);
  layout->addWidget(audio);
  layout->addWidget(video);
  layout->addWidget(note);
  layout->addWidget(storage_);
  layout->addWidget(buttons);

  show_signaling_hint();
  load_devices();
  load_monitors();
  load_quality();

  // What the configuration opens on, which is "system" unless somebody changed
  // it. findData returns -1 for a mode this build does not offer, and leaving
  // the box on its first row is the right answer to that.
  if (const int mode = screen_audio_->findData(QString::fromUtf8(
          client::app::to_string(session_.screen_audio_mode()).data(),
          qsizetype(client::app::to_string(session_.screen_audio_mode()).size())));
      mode >= 0) {
    screen_audio_->setCurrentIndex(mode);
  }
  load_audio_sources();
  // After load_audio_sources, because whether the slider is usable is decided
  // by the same mode that decides whether the application box is.
  show_screen_volume();
  show_storage();

  // One call for all three, because in automatic mode the values and whether
  // the boxes accept typing are the same question, answered by the session.
  sync_bitrate();

  // After the bitrate is in, because what it says is the two compared.
  show_quality_hint();

  // Connected after the initial values are in, so that filling the widgets
  // does not look like the user changing something.
  //
  // editingFinished and not textChanged, for the reason the spin boxes have
  // keyboard tracking off: typing ws://192.168.1.10:8080 one character at a
  // time would announce forty addresses, thirty-nine of them refused, and the
  // row would spend the whole time red at somebody who is typing correctly.
  // It fires on Enter and on losing the focus, and clicking Save takes the
  // focus, so a typed address is never left behind by the button.
  connect(signaling_url_, &QLineEdit::editingFinished, this,
          &SettingsDialog::on_signaling_url_changed);
  connect(input_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_input_changed);
  connect(output_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_output_changed);
  // valueChanged and not editingFinished, which only fires when the box gives
  // up the focus. Pressing the arrow moved the number on screen and nothing
  // else: the call kept the old rate, and the line at the bottom went on
  // saying everything was saved, over a box showing a figure that was neither
  // in use nor written down. Whoever pressed it once and looked at the result
  // had every reason to believe it had taken.
  connect(auto_bitrate_, &QCheckBox::toggled, this, &SettingsDialog::on_auto_bitrate_changed);
  connect(min_bitrate_, &QSpinBox::valueChanged, this, &SettingsDialog::on_bitrate_changed);
  connect(max_bitrate_, &QSpinBox::valueChanged, this, &SettingsDialog::on_bitrate_changed);
  connect(resolution_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_quality_changed);
  connect(frame_rate_, &QComboBox::currentIndexChanged, this, &SettingsDialog::on_quality_changed);
  connect(screen_audio_, &QComboBox::currentIndexChanged, this,
          &SettingsDialog::on_screen_audio_changed);
  connect(noise_suppression_, &QComboBox::currentIndexChanged, this,
          &SettingsDialog::on_noise_suppression_changed);
  // valueChanged and not sliderReleased, so that dragging is audible while it
  // happens. That is the whole way this control can be got right: nobody knows
  // what "sixty percent" sounds like against their own voice, they find it by
  // moving the slider and listening. Every step applies within one 10 ms block,
  // and each one replaces the last rather than queueing behind it.
  connect(screen_volume_, &QSlider::valueChanged, this, &SettingsDialog::on_screen_volume_changed);
  connect(room_sounds_, &QCheckBox::toggled, this, &SettingsDialog::on_room_sounds_changed);
  connect(update_checks_, &QCheckBox::toggled, this, &SettingsDialog::on_update_checks_changed);
}

void SettingsDialog::show_signaling_hint(const QString& refusal) {
  signaling_hint_->setProperty("error", !refusal.isEmpty());
  signaling_hint_->setText(
      refusal.isEmpty()
          ? QStringLiteral(
                "Where the next sign-in connects. A call already running stays on the server it "
                "was placed on, so this never interrupts one - and it does not need PartyShare "
                "restarted either: leave the room and sign in again.")
          : refusal);
  restyle_for_property(signaling_hint_);
}

void SettingsDialog::on_signaling_url_changed() {
  const QString typed = signaling_url_->text().trimmed();

  // Refused here, in front of whoever typed it, and not left for the sign-in
  // or for the file. dv::config::validate refuses the same two things when
  // config.ini is written, and a refusal that arrives after this dialog has
  // been closed is a refusal nobody reads.
  if (typed.isEmpty()) {
    show_signaling_hint(
        QStringLiteral("A server address is needed, so this one was not applied. The last one is "
                       "still in use."));
    return;
  }
  if (!typed.startsWith(QStringLiteral("ws://")) && !typed.startsWith(QStringLiteral("wss://"))) {
    show_signaling_hint(
        QStringLiteral("A server address starts with ws:// or wss://, so this one was not "
                       "applied. The last one is still in use."));
    return;
  }

  // Put back trimmed, so that what is on screen is what was applied. A space
  // on the end is invisible and would otherwise travel into config.ini and
  // come back as a server nobody can reach.
  if (typed != signaling_url_->text()) {
    const QSignalBlocker quiet(signaling_url_);
    signaling_url_->setText(typed);
  }

  // editingFinished also fires on merely tabbing through the field. Staging a
  // value identical to the one in use would light the Save button up over
  // nothing to write, and the button is this dialog's only word for whether
  // anything is outstanding.
  if (typed.toStdString() == session_.signaling_url()) {
    show_signaling_hint();
    return;
  }

  session_.set_signaling_url(typed.toStdString());
  stage({{.section = "network", .key = "signaling_url", .value = typed.toStdString()}});
  show_signaling_hint();
}

void SettingsDialog::load_audio_sources() {
  const bool one_application = screen_audio_->currentData().toString() == QStringLiteral("process");
  audio_source_->setEnabled(one_application);
  audio_source_->clear();
  screen_audio_hint_->clear();

  if (!client::audio::loopback_capture_is_available()) {
    screen_audio_->setEnabled(false);
    audio_source_->setEnabled(false);
    screen_audio_hint_->setText(
        QStringLiteral("This system cannot capture what an application is playing. On Windows "
                       "that needs build 20348 or newer."));
    return;
  }

  if (!one_application) {
    return;
  }

  const auto listed = session_.audio_sources();
  if (!listed) {
    audio_source_->addItem(QStringLiteral("could not read the applications"), 0U);
    audio_source_->setEnabled(false);
    screen_audio_hint_->setText(QString::fromStdString(listed.error().message));
    return;
  }
  for (const client::audio::AudioSource& source : listed.value()) {
    audio_source_->addItem(
        source.playing ? QStringLiteral("%1  (playing)").arg(QString::fromStdString(source.name))
                       : QString::fromStdString(source.name),
        static_cast<unsigned>(source.process_id));
  }
  if (audio_source_->count() == 0) {
    audio_source_->addItem(QStringLiteral("nothing is playing"), 0U);
    audio_source_->setEnabled(false);
    screen_audio_hint_->setText(
        QStringLiteral("Start playing something and open this dialog again."));
  }
}

void SettingsDialog::on_screen_audio_changed() {
  load_audio_sources();
  show_screen_volume();
  stage({config::IniSetting{.section = "screen_audio",
                            .key = "mode",
                            .value = screen_audio_->currentData().toString().toStdString()}});
}

void SettingsDialog::show_screen_volume() {
  // The slider is only meaningful while the share carries sound at all. It
  // follows the application box rather than repeating its reasoning: that box
  // is already disabled for a build or a Windows that cannot capture, and a
  // volume for a capture that cannot happen is a control with nothing behind
  // it.
  const bool carries_sound = screen_audio_->isEnabled() &&
                             screen_audio_->currentData().toString() != QStringLiteral("none");
  screen_volume_->setEnabled(carries_sound);

  const int percent = screen_volume_->value();
  if (!carries_sound) {
    screen_volume_label_->setText(
        QStringLiteral("Shared sound would go out at %1%. The share is set to carry none.")
            .arg(percent));
    return;
  }
  if (percent == 0) {
    screen_volume_label_->setText(
        QStringLiteral("Shared sound is silent. The picture still goes out."));
    return;
  }
  if (percent > 100) {
    // Said plainly rather than left to be discovered. Above 100 the mixer
    // clamps peaks instead of wrapping them, so the failure mode is a share
    // that sounds harsh, and somebody who was not told will look for the cause
    // anywhere but the setting they just moved.
    screen_volume_label_->setText(
        QStringLiteral("Shared sound goes out at %1% - louder than the application plays it, "
                       "which can distort. Everyone in the room hears this level.")
            .arg(percent));
    return;
  }
  screen_volume_label_->setText(
      QStringLiteral("Shared sound goes out at %1% next to your microphone. Everyone in the room "
                     "hears this level.")
          .arg(percent));
}

void SettingsDialog::on_screen_volume_changed(int percent) {
  if (const auto applied = session_.set_screen_audio_volume(percent); !applied) {
    DV_LOG_WARN("Could not set the shared screen volume: {}", applied.error().message);
  }
  show_screen_volume();
  stage({config::IniSetting{
      .section = "screen_audio", .key = "volume_percent", .value = std::to_string(percent)}});
}

void SettingsDialog::on_noise_suppression_changed() {
  const std::string choice = noise_suppression_->currentData().toString().toStdString();
  const bool on = choice != "off";
  // "Off" keeps the level the session had, so that turning the suppressor
  // back on returns to it rather than to whatever the box happened to say.
  const client::media::NoiseSuppressionLevel level =
      on ? client::media::parse_noise_suppression_level(choice).value_or(
               session_.noise_suppression_level())
         : session_.noise_suppression_level();
  // The other two blocks are passed through as they stand rather than as the
  // values this dialog was opened with. They are not on this page, so anything
  // that moved them moved them elsewhere, and sending a stale copy back would
  // turn a change of one setting into a quiet revert of two.
  if (const auto applied = session_.set_audio_processing(session_.echo_cancellation(), on,
                                                         session_.automatic_gain_control(), level);
      !applied) {
    DV_LOG_WARN("Could not set noise suppression: {}", applied.error().message);
  }
  std::vector<config::IniSetting> settings{config::IniSetting{
      .section = "audio", .key = "noise_suppression", .value = on ? "true" : "false"}};
  if (on) {
    settings.push_back(
        config::IniSetting{.section = "audio", .key = "noise_suppression_level", .value = choice});
  }
  stage(settings);
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
        QStringLiteral("This system does not say where settings belong, so the server, the "
                       "devices, the sound and the screen settings chosen here last only until "
                       "the program closes."));
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
        QStringLiteral("The server, the devices, the sound and the screen settings are kept "
                       "in %1")
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
  restyle_for_property(storage_);
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

  if (auto_bitrate_->isChecked()) {
    // The warning below cannot fire here: the ceiling is the recommendation.
    // What is worth saying instead is where the greyed out numbers came from,
    // since a disabled box with a value in it otherwise reads as a setting
    // that has got stuck.
    quality_hint_->setText(
        QStringLiteral("%1 at %2 fps is worth about %3 kbps, so that is the maximum. The two "
                       "below follow the rows above.")
            .arg(resolution_->currentText())
            .arg(fps)
            .arg(wants));
    return;
  }

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

void SettingsDialog::sync_bitrate() {
  const bool automatic = session_.auto_bitrate();
  const auto [minimum, maximum] = session_.video_bitrate();

  const QSignalBlocker auto_quiet(auto_bitrate_);
  const QSignalBlocker min_quiet(min_bitrate_);
  const QSignalBlocker max_quiet(max_bitrate_);

  auto_bitrate_->setChecked(automatic);
  min_bitrate_->setValue(minimum);
  max_bitrate_->setValue(maximum);

  // Disabled and not hidden. What automatic mode picked is the most useful
  // thing on this row, and a row that disappears takes the answer to "what is
  // it actually sending" with it.
  min_bitrate_->setEnabled(!automatic);
  max_bitrate_->setEnabled(!automatic);
}

std::vector<config::IniSetting> SettingsDialog::bitrate_settings() const {
  return {
      {.section = "video",
       .key = "auto_bitrate",
       .value = auto_bitrate_->isChecked() ? "true" : "false"},
      {.section = "video",
       .key = "min_bitrate_kbps",
       .value = std::to_string(min_bitrate_->value())},
      {.section = "video",
       .key = "max_bitrate_kbps",
       .value = std::to_string(max_bitrate_->value())},
  };
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

  // All three keys in one pass. Two of them are a range: staged one at a time,
  // a Save between them would leave a file whose maximum sits below its
  // minimum, which is a configuration that does not start.
  stage(bitrate_settings());
}

void SettingsDialog::on_room_sounds_changed(bool on) {
  // Nothing to fail and nothing to ask the session: the chime is the
  // interface's own, and it does not travel to the room or into the call.
  // Applied before it is staged, so unticking the box is silent at once
  // rather than at the next launch - which is the whole point of a switch
  // somebody reaches for because a sound just went off.
  set_chimes_enabled(on);
  stage({{.section = "ui", .key = "room_sounds", .value = on ? "true" : "false"}});
}

void SettingsDialog::on_update_checks_changed(bool on) {
  // Applied before it is staged, like the chime above. Unticking this is
  // somebody saying "stop talking to the internet", and the honest answer to
  // that is to stop now rather than at the next launch - which is also why
  // UpdateChecker drops an answer that lands after the switch has moved.
  //
  // Ticking it schedules a check a few seconds out, so the box is not a switch
  // whose effect cannot be observed for six hours.
  updates_.set_enabled(on);
  stage({{.section = "ui", .key = "check_for_updates", .value = on ? "true" : "false"}});
}

void SettingsDialog::on_auto_bitrate_changed(bool automatic) {
  if (const auto applied = session_.set_auto_bitrate(automatic); !applied) {
    DV_LOG_WARN("Refused to turn automatic bitrate {}: {}", automatic ? "on" : "off",
                applied.error().message);
    // Back onto the mode actually in force, or the box says one thing while
    // the session does another.
    sync_bitrate();
    return;
  }

  // The session has just worked the range out, so the boxes are read back from
  // it rather than computed here a second time. Two places deriving the same
  // number is two places to keep in step.
  sync_bitrate();
  show_quality_hint();
  stage(bitrate_settings());
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

  // In automatic mode the session has just moved the range to match the new
  // rows, so the boxes are stale until this. Harmless in manual mode, where it
  // reads back exactly what is already shown.
  sync_bitrate();
  show_quality_hint();

  // All three in one pass, for the reason the bitrate keys are: a Save between
  // two of them would leave a width from one choice next to a height from
  // another, which is a shape no monitor has.
  std::vector<config::IniSetting> settings{
      {.section = "video", .key = "width", .value = std::to_string(size.width)},
      {.section = "video", .key = "height", .value = std::to_string(size.height)},
      {.section = "video", .key = "fps", .value = std::to_string(fps)},
  };
  if (auto_bitrate_->isChecked()) {
    // The range moved with the rows, and a file that keeps the new resolution
    // next to the old bitrate is a file that starts the next run on neither.
    const std::vector<config::IniSetting> bitrate = bitrate_settings();
    settings.insert(settings.end(), bitrate.begin(), bitrate.end());
  }
  stage(settings);
}

void SettingsDialog::select_monitor(const QString& id) {
  if (const int index = monitor_->findData(id); index >= 0) {
    monitor_->setCurrentIndex(index);
  }
}

QString SettingsDialog::selected_monitor() const {
  return monitor_->currentData().toString();
}

client::app::ScreenAudio SettingsDialog::selected_screen_audio() const {
  const QString mode = screen_audio_->currentData().toString();
  if (mode == QStringLiteral("system")) {
    return {.mode = client::app::ScreenAudio::Mode::System};
  }
  if (mode == QStringLiteral("process")) {
    return {.mode = client::app::ScreenAudio::Mode::Application,
            .source_id = audio_source_->currentData().toUInt()};
  }
  return {};
}

}  // namespace dv::ui
