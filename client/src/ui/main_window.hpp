#pragma once

#include <memory>
#include <vector>

#include <QMainWindow>
#include <QString>

#include "app/call_session.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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

  // Called on the UI thread, from the session's callbacks.
  void apply_state(int state, const QString& detail);
  void apply_participants(const QStringList& names);
  void apply_metrics(const QString& summary);
  void apply_error(const QString& code, const QString& message);
  void apply_room_created(const QString& room_id);

 private:
  void build_widgets();
  void wire_session();
  void refresh_controls();

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
  QLabel* status_ = nullptr;
  QLabel* metrics_ = nullptr;

  client::app::CallSession::State state_ = client::app::CallSession::State::Idle;
};

}  // namespace dv::ui
