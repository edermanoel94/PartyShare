#pragma once

#include <memory>

#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QString>

#include "app/call_session.hpp"
#include "app/network_quality.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QStackedWidget;

namespace dv::ui {

class AdminPanel;
class ScreenView;

/// The interface of section 19 of SPEC.md: three screens and a settings
/// dialog, over the core built in M2 to M6.
///
/// Login, then the home screen with create and join, then the room. They are
/// pages of one stack rather than separate windows, because a call is one
/// continuous thing and a window that disappears and reappears loses its place
/// on the desktop every time.
///
/// This window drives client::app::CallSession and never touches signaling,
/// media or capture itself. The session reports from networking and media
/// threads, and every one of those reports is turned into a queued invocation
/// before a widget is touched, because Qt widgets may only be used from the
/// thread that owns them.
// A QObject cannot be copied or moved: its identity is the thing Qt tracks.
// The destructor exists to unhook the session callbacks, and asking for the
// other four special members here would be asking for members that must not
// exist.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
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
  void on_toggle_share();
  void on_open_settings();
  void on_open_administration();
  void on_close_administration();
  void on_copy_room_id();
  void on_participant_selected();
  void on_participant_menu(const QPoint& where);
  void on_volume_changed(int value);
  void on_send_chat();

  // Called on the UI thread, from the session's callbacks.
  void apply_state(int state, const QString& detail);
  void apply_participants(const QStringList& names);
  void apply_metrics(const QString& summary, int quality);
  void apply_local_level(double level, bool speaking);
  void apply_error(const QString& code, const QString& message);
  void apply_room_created(const QString& room_id);
  void apply_screen_share(const QString& user_id);
  void apply_kicked(const QString& reason);
  void apply_forced_mute(const QString& name, const QString& by_name, bool muted);
  void apply_chat_message(const QString& line);
  /// Replaces what is on screen rather than adding to it. See
  /// CallSession::Callbacks::on_chat_history for why.
  void apply_chat_history(const QStringList& lines);

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  void build_login_page();
  void build_home_page();
  void build_room_page();
  void build_admin_page();
  void wire_session();
  void refresh_controls();
  /// Adds one line and keeps the view at the bottom, which is where a
  /// conversation is read from.
  void append_chat_line(const QString& line);
  void update_volume_label(const QString& participant, int volume);
  void show_page();

  client::app::CallSession& session_;

  QStackedWidget* pages_ = nullptr;

  // Login.
  QLineEdit* username_ = nullptr;
  QLineEdit* password_ = nullptr;
  QPushButton* connect_button_ = nullptr;
  QLabel* login_error_ = nullptr;

  // Home.
  QLabel* welcome_ = nullptr;
  QLineEdit* room_id_ = nullptr;
  QPushButton* create_button_ = nullptr;
  QPushButton* join_button_ = nullptr;
  /// Shown only to an administrator. See on_open_administration.
  QPushButton* admin_button_ = nullptr;

  // Room.
  QLabel* room_title_ = nullptr;
  QPushButton* copy_room_button_ = nullptr;
  ScreenView* screen_view_ = nullptr;
  QListWidget* participants_ = nullptr;
  QProgressBar* microphone_level_ = nullptr;
  QSlider* volume_ = nullptr;
  QLabel* volume_label_ = nullptr;
  QPushButton* mute_button_ = nullptr;
  QPushButton* share_button_ = nullptr;
  QPushButton* settings_button_ = nullptr;
  QPushButton* leave_button_ = nullptr;
  QLabel* sharing_label_ = nullptr;

  // Chat. A list of plain text items rather than a rich text view: what goes
  // in it is typed by other people, and a widget that renders no markup cannot
  // be made to render theirs.
  QListWidget* chat_view_ = nullptr;
  QLineEdit* chat_input_ = nullptr;
  QPushButton* chat_send_ = nullptr;

  // Administration.
  AdminPanel* admin_panel_ = nullptr;
  /// Which page to go back to when the administration page is closed.
  int previous_page_ = 0;

  // Status bar.
  QLabel* status_ = nullptr;
  QLabel* quality_ = nullptr;
  QLabel* metrics_ = nullptr;

  client::app::CallSession::State state_ = client::app::CallSession::State::Idle;
  /// Whose volume the slider is showing. Empty when nobody is selected.
  QString selected_participant_;
  /// The volume applied to each participant, by user id, as a percentage.
  /// Anyone missing is at 100, which is the volume they were sent at.
  QHash<QString, int> volumes_;
  /// The monitor chosen in the settings dialog, empty for the primary one.
  QString monitor_id_;
};

}  // namespace dv::ui
