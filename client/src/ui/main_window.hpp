#pragma once

#include <memory>
#include <vector>

#include <QHash>
#include <QMainWindow>
#include <QString>

#include "app/call_session.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;

namespace dv::ui {

/// The provisional interface of M4: log in, create or join a room, mute.
///
/// Section 19 of SPEC.md describes the real one, which is M7. What matters
/// here is the wiring, not the looks: this window drives client::app::CallSession and
/// never touches signaling, media or capture itself.
///
/// The session reports from networking and media threads. Every one of those
/// reports is turned into a queued invocation before a widget is touched,
/// because Qt widgets may only be used from the thread that owns them.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(client::app::CallSession& session, QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void on_connect();
  void on_create_room();
  void on_join_room();
  void on_leave_room();
  void on_toggle_mute();
  void on_input_device_changed(int index);
  void on_output_device_changed(int index);
  void on_participant_selected();
  void on_volume_changed(int value);

  // Called on the UI thread, from the session's callbacks.
  void apply_state(int state, const QString& detail);
  void apply_participants(const QStringList& names);
  void apply_metrics(const QString& summary);
  void apply_local_level(double level, bool speaking);
  void apply_error(const QString& code, const QString& message);
  void apply_room_created(const QString& room_id);

 private:
  void build_widgets();
  void wire_session();
  void refresh_controls();
  void load_devices();
  void update_volume_label(const QString& participant, int volume);

  client::app::CallSession& session_;

  QLineEdit* username_ = nullptr;
  QLineEdit* password_ = nullptr;
  QLineEdit* room_id_ = nullptr;
  QPushButton* connect_button_ = nullptr;
  QPushButton* create_button_ = nullptr;
  QPushButton* join_button_ = nullptr;
  QPushButton* leave_button_ = nullptr;
  QPushButton* mute_button_ = nullptr;
  QListWidget* participants_ = nullptr;
  QComboBox* input_device_ = nullptr;
  QComboBox* output_device_ = nullptr;
  QProgressBar* microphone_level_ = nullptr;
  QSlider* volume_ = nullptr;
  QLabel* volume_label_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* metrics_ = nullptr;

  client::app::CallSession::State state_ = client::app::CallSession::State::Idle;
  /// Whose volume the slider is showing. Empty when nobody is selected.
  QString selected_participant_;
  /// The volume applied to each participant, by user id, as a percentage.
  /// Anyone missing is at 100, which is the volume they were sent at.
  QHash<QString, int> volumes_;
};

}  // namespace dv::ui
