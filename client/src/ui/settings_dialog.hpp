#pragma once

#include <vector>

#include <dv/config/config.hpp>

#include <QDialog>
#include <QString>

#include "app/call_session.hpp"

class QComboBox;
class QLabel;
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
/// The two audio devices and the bitrate range are also written to this user's
/// config.ini as they are chosen, so the choice survives the program closing.
/// Chosen once and then forgotten is only true of a setting that is still there
/// next time.
///
/// The monitor is deliberately not among them: it is which screen to share
/// next, which is a decision per share rather than a setting.
class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SettingsDialog(client::app::CallSession& session, QWidget* parent = nullptr);

 private slots:
  void on_input_changed(int index);
  void on_output_changed(int index);
  void on_bitrate_changed();

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  void load_devices();
  void load_monitors();

  /// Writes settings to this user's config.ini and says on screen how it went.
  ///
  /// Reported rather than swallowed. A dialog that accepts a microphone, cannot
  /// save it and says nothing produces "it keeps forgetting my settings", which
  /// is a bug report with nothing in it to act on.
  ///
  /// Takes a list because some settings only make sense together: the two ends
  /// of the bitrate range have to reach the file in the same pass, or something
  /// reading it in between finds a maximum below its minimum.
  void remember(const std::vector<config::IniSetting>& settings);

  /// Says which file the settings are kept in, in its resting wording.
  void show_storage();

  /// Makes the storage line pick up a change to its `error` property.
  void restyle();

  client::app::CallSession& session_;

  QComboBox* input_ = nullptr;
  QComboBox* output_ = nullptr;
  QComboBox* monitor_ = nullptr;
  QSpinBox* min_bitrate_ = nullptr;
  QSpinBox* max_bitrate_ = nullptr;
  /// Which file the settings are kept in, and what went wrong when one could
  /// not be written to it.
  QLabel* storage_ = nullptr;

 public:
  /// The monitor the user picked, for whoever starts the share.
  [[nodiscard]] QString selected_monitor() const;
};

}  // namespace dv::ui
