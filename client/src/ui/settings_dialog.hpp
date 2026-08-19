#pragma once

#include <QDialog>

#include "app/call_session.hpp"

class QComboBox;
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
class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SettingsDialog(client::app::CallSession& session, QWidget* parent = nullptr);

 private slots:
  void on_input_changed(int index);
  void on_output_changed(int index);
  void on_bitrate_changed();

 private:
  void load_devices();
  void load_monitors();

  client::app::CallSession& session_;

  QComboBox* input_ = nullptr;
  QComboBox* output_ = nullptr;
  QComboBox* monitor_ = nullptr;
  QSpinBox* min_bitrate_ = nullptr;
  QSpinBox* max_bitrate_ = nullptr;

 public:
  /// The monitor the user picked, for whoever starts the share.
  [[nodiscard]] QString selected_monitor() const;
};

}  // namespace dv::ui
