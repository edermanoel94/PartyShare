#pragma once

#include <vector>

#include <dv/config/config.hpp>

#include <QDialog>
#include <QString>

#include "app/call_session.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace dv::ui {

/// Devices, monitor and bitrate, in one place.
///
/// Section 19 of SPEC.md keeps the room screen down to what is needed during a
/// call. Everything that is chosen once and then forgotten lives here instead
/// of taking up room next to the controls.
///
/// Changes apply as they are made rather than on closing: a microphone that
/// only takes effect after a dialog is dismissed cannot be tested by speaking
/// into it.
///
/// The server address is the one exception, and it is not one of degree. Every
/// other setting here describes how this machine behaves, and can be changed
/// under a running call without the call noticing. The address says which
/// server the room and everybody in it live on, so applying it at once would
/// not be a setting taking effect, it would be hanging up. It is adopted at the
/// next sign-in instead, and the row says so rather than leaving somebody to
/// wonder whether it took.
///
/// Keeping them is a separate act, and that is what the Save button is for.
/// The two used to be one - every change went straight into this user's
/// config.ini as it was made - and that is a worse arrangement than it looks.
/// Trying four microphones to find the right one wrote four times, each write
/// a read, a parse, a validate and a replace of the whole file, and the three
/// microphones nobody chose were saved just as firmly as the one they did. It
/// also left no way to try something and put it back, because there was no
/// point at which it had not already been kept.
///
/// So a change is applied and remembered as pending, and Save writes every
/// pending one in a single pass. Closing with changes outstanding asks, rather
/// than deciding on somebody's behalf in either direction.
///
/// The monitor is deliberately not among them: it is which screen to share
/// next, which is a decision per share rather than a setting.
class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SettingsDialog(client::app::CallSession& session, QWidget* parent = nullptr);

 protected:
  /// The one door out of a QDialog: the Close button, the window's own close
  /// box and the escape key all arrive here. Overridden so that none of the
  /// three can quietly drop settings that are in use but not written down.
  void done(int result) override;

 private slots:
  void on_signaling_url_changed();
  void on_input_changed(int index);
  void on_output_changed(int index);
  void on_bitrate_changed();
  void on_auto_bitrate_changed(bool automatic);
  void on_quality_changed();
  void on_screen_audio_changed();
  void on_noise_suppression_changed(bool on);
  void on_screen_volume_changed(int percent);
  void on_room_sounds_changed(bool on);
  void on_save();

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  void load_devices();
  void load_monitors();

  /// Puts the percentage into the volume slider's label, and greys the pair out
  /// while the share is set to carry no sound.
  ///
  /// Greyed out rather than hidden. A control that appears and disappears as
  /// the box above it changes is harder to find a second time than one that is
  /// plainly there and plainly not in use, and the label goes on saying what
  /// the level will be when sound is turned back on.
  void show_screen_volume();

  /// Fills the application box with what the machine can currently be heard
  /// playing, and enables it only when one application is what was asked for.
  ///
  /// Refilled every time the mode changes rather than once on opening: the list
  /// is of what is playing *now*, and a dialog left open while somebody starts
  /// a video would otherwise offer a menu from a minute ago.
  void load_audio_sources();

  /// Fills the resolution and frame rate boxes and selects what is in use.
  ///
  /// A configuration is free to name a size or a rate this dialog does not
  /// offer, `[video] width = 2560` is a perfectly valid one, so whatever is in
  /// use gets a row of its own when it is not already on the menu. The
  /// alternative is a box quietly showing 720p while 1440p is being sent, and
  /// turning any visit to this dialog into a silent downgrade.
  void load_quality();

  /// Says, under the quality rows, when the chosen size and rate want more
  /// bitrate than the configured maximum allows.
  ///
  /// A note and not a correction. Raising somebody's ceiling because they
  /// picked 1080p would undo a setting they made on purpose, possibly one made
  /// to fit a link that cannot carry more.
  ///
  /// In automatic mode there is nothing to warn about, because the ceiling is
  /// the recommendation. It says what was chosen instead, which is the only
  /// thing on screen that explains two greyed out boxes.
  void show_quality_hint();

  /// Puts the range the session holds into the two spin boxes, and greys them
  /// out when automatic mode is on.
  ///
  /// Signals blocked throughout: these values come from the session, and
  /// letting them arrive as edits would ask the session for what it just said,
  /// then stage it as though somebody had typed it.
  void sync_bitrate();

  /// The `[video]` keys that describe the bitrate right now, as one list.
  ///
  /// The mode and the range travel together for the reason the two ends of the
  /// range do: staged apart, a Save in between leaves a file that says
  /// automatic while holding the range from before it, and that file starts a
  /// client on a bitrate nobody picked.
  [[nodiscard]] std::vector<config::IniSetting> bitrate_settings() const;

  /// Adds settings to what Save will write, replacing any earlier value for
  /// the same key.
  ///
  /// Replacing rather than appending, because the file is written from this
  /// list in one pass: three visits to the microphone box would otherwise put
  /// three input_device lines into it, and which one survives would come down
  /// to the order the writer happens to walk them in.
  ///
  /// Takes a list because some settings only make sense together. The two ends
  /// of the bitrate range are the case: staged apart, a Save between the two
  /// would leave a file whose maximum sits below its minimum.
  void stage(const std::vector<config::IniSetting>& settings);

  /// Says which file the settings are kept in, whether anything is waiting to
  /// go into it, and what went wrong when it could not be written.
  ///
  /// Reported rather than swallowed. A dialog that accepts a microphone,
  /// cannot save it and says nothing produces "it keeps forgetting my
  /// settings", which is a bug report with nothing in it to act on.
  void show_storage();

  /// Makes the storage line pick up a change to its `error` property.
  void restyle();

  /// Says what the address on the row will and will not do, and why one was
  /// refused. Never empty: a row with nothing under it reads as a setting that
  /// behaves like the others on this page, and this one does not.
  void show_signaling_hint(const QString& refusal = {});

  client::app::CallSession& session_;

  /// Where the next sign-in connects. A line edit and not a box of choices,
  /// because the address of a server nobody has connected to yet cannot be
  /// offered as one.
  QLineEdit* signaling_url_ = nullptr;
  QLabel* signaling_hint_ = nullptr;

  QComboBox* input_ = nullptr;
  QComboBox* output_ = nullptr;
  /// Whether libwebrtc's noise suppressor runs over the microphone.
  ///
  /// Worth a switch because it is tuned for one thing - a voice in a room - and
  /// treats everything else as the room: an instrument, a record playing
  /// behind somebody, a hiss that is actually part of what they meant to send.
  /// Off is the right answer often enough that reaching it should not need a
  /// text editor and a restart.
  QCheckBox* noise_suppression_ = nullptr;
  /// The chime when somebody joins or leaves a room.
  ///
  /// Beside the devices rather than under the screen settings, because it is
  /// about what this machine plays and not about what a share carries.
  QCheckBox* room_sounds_ = nullptr;
  QComboBox* monitor_ = nullptr;
  /// None, everything but this client, or one application.
  QComboBox* screen_audio_ = nullptr;
  /// Which application, when that is the mode. Its data is a process id.
  QComboBox* audio_source_ = nullptr;
  /// Says why the box is empty or disabled, which is otherwise a dead control
  /// with no explanation: an old Windows, or a build without the capture.
  QLabel* screen_audio_hint_ = nullptr;
  /// How loud the shared sound goes out beside this microphone, in percent.
  ///
  /// Not the same control as the volume slider in the room, and deliberately
  /// not next to it: that one is playback and affects only the person moving
  /// it, this one is applied before the sound is encoded and is therefore
  /// heard by everybody. Putting them side by side would invite the reading
  /// that one of them is "my copy" of the other.
  QSlider* screen_volume_ = nullptr;
  /// The percentage in words, above the slider. A slider carrying no number can
  /// be put back where it was by eye and never by hand, and a level somebody
  /// tuned once against a particular game is one they will want back exactly.
  QLabel* screen_volume_label_ = nullptr;
  QComboBox* resolution_ = nullptr;
  QComboBox* frame_rate_ = nullptr;
  QCheckBox* auto_bitrate_ = nullptr;
  QSpinBox* min_bitrate_ = nullptr;
  QSpinBox* max_bitrate_ = nullptr;
  /// Empty unless the chosen quality wants more than the configured ceiling.
  QLabel* quality_hint_ = nullptr;
  /// Which file the settings are kept in, and what went wrong when one could
  /// not be written to it.
  QLabel* storage_ = nullptr;
  /// Enabled only when there is something to write, so that the button says
  /// whether anything is outstanding without needing a word for it.
  QPushButton* save_ = nullptr;

  /// In use, and not yet in the file. Empty is the resting state.
  std::vector<config::IniSetting> pending_;

 public:
  /// Puts `id` in the monitor box, for a dialog opened while a choice made
  /// elsewhere - the share button's menu, or an earlier visit here - is in
  /// force. An empty or unknown id leaves the primary monitor selected, which
  /// is what an empty id means to the capturer as well.
  void select_monitor(const QString& id);

  /// The monitor the user picked, for whoever starts the share.
  [[nodiscard]] QString selected_monitor() const;

  /// What the next share should carry besides the picture.
  [[nodiscard]] client::app::ScreenAudio selected_screen_audio() const;
};

}  // namespace dv::ui
