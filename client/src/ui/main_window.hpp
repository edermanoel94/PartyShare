#pragma once

#include <memory>

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QString>

#include "app/call_session.hpp"
#include "app/network_quality.hpp"
#include "app/smoothing.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace dv::ui {

class AdminPanel;
class ChatView;
class MetricsDialog;
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

 public slots:
  /// Rows of tab separated fields, identifier first, as ui::fill expects.
  /// Invoked from the signalling thread through a queued connection, which is
  /// why it is a slot and not a plain member.
  ///
  /// `may_create` is worked out from the same answer: an ordinary user gets
  /// one room, and the button that would be refused is disabled rather than
  /// left to fail on the click.
  void apply_room_list(const QStringList& rows, bool may_create);

 private slots:
  void on_connect();
  /// Drops the connection and the identity with it, which puts the login
  /// screen back. What it is for is in build_home_page.
  void on_sign_out();
  void on_create_room();
  void on_join_room();
  void on_leave_room();
  void on_toggle_mute();
  void on_toggle_share();
  void on_open_settings();
  void on_open_metrics();
  void on_open_administration();
  void on_close_administration();
  void on_copy_room_id();
  void on_participant_selected();
  void on_participant_menu(const QPoint& where);
  void on_volume_changed(int value);
  void on_send_chat();
  void on_open_emoji_picker();

  // Called on the UI thread, from the session's callbacks.
  void apply_state(int state, const QString& detail);
  void apply_participants(const QStringList& names);
  void apply_metrics(const QString& summary, int quality);
  /// The round trip to the signaling server in milliseconds, or -1 when there
  /// is no current measurement.
  ///
  /// This is what keeps the network indicator alive outside a call, which is
  /// where it was blank before: the call metrics stop when the call does, and
  /// "is my connection any good" is asked hardest on the lobby screen.
  void apply_link(int round_trip_ms);
  void apply_local_level(double level, bool speaking);
  /// Moves the microphone meter one frame closer to the last measurement.
  ///
  /// The measurements arrive five times a second and the bar is drawn sixty,
  /// because five positions a second is not movement, it is a slideshow. See
  /// app::LevelMeter for what decides how fast it is allowed to travel.
  void animate_level();
  void apply_error(const QString& code, const QString& message);
  void apply_room_created(const QString& room_id);
  void apply_screen_share(const QString& user_id);
  void apply_kicked(const QString& reason);
  void apply_forced_mute(const QString& name, const QString& by_name, bool muted);
  /// `summary` is models::describe of what is in force now, empty when nothing
  /// is. Strings rather than the struct, for the reason the participant rows
  /// travel as strings: crossing threads with a Qt connection needs a
  /// registered metatype, and this needs none.
  void apply_restrictions(const QString& name, const QString& by_name, const QString& summary,
                          const QString& reason, bool is_us);
  void apply_chat_message(const QString& line);
  /// Replaces what is on screen rather than adding to it. See
  /// CallSession::Callbacks::on_chat_history for why.
  void apply_chat_history(const QStringList& lines);

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  /// Puts a sentence under the sign in button, or takes the row away when
  /// there is nothing to say.
  ///
  /// Shown but empty, the label still takes its height: a band of nothing under
  /// the button on every login that has not failed yet, and a card that jumps
  /// as soon as one does.
  void show_login_error(const QString& text);

  void build_login_page();
  void build_home_page();
  void build_room_page();
  void build_admin_page();
  void wire_session();
  void refresh_controls();
  /// Adds one line and keeps the view at the bottom, which is where a
  /// conversation is read from.
  void append_chat_line(const QString& line);
  /// Puts one emoji into the message field at the cursor and gives the field
  /// the focus back.
  void insert_emoji(const QString& emoji);
  void update_volume_label(const QString& participant, int volume);
  /// Empties the status bar numbers a call fills, and hands the network
  /// indicator back to the signaling link.
  ///
  /// The call's numbers belong to one call, and nothing outside a call writes
  /// them again. Left where they are, they do not go stale by a second but by
  /// however long the window stays open, and the green "network good" is the
  /// half that misleads: it reads as a live measurement of a connection that
  /// is no longer carrying anything.
  ///
  /// The indicator itself is not emptied, it changes hands. What replaces the
  /// call's verdict is the round trip to the signaling server, which is being
  /// measured the whole time and is at most one interval old.
  void clear_metrics();

  /// Draws the network indicator from the signaling link.
  ///
  /// Says `server` rather than `network`, and the difference is not decorative:
  /// this is one round trip over a WebSocket, while the version a call
  /// produces weighs latency, jitter and loss together. Both colour the same
  /// dot, so the word is the only thing telling somebody which claim they are
  /// being given.
  void show_link_quality();
  void show_page();

  /// Puts a page on screen, fading it in.
  ///
  /// Short - under a fifth of a second - and only on the page that is
  /// arriving. Long enough that the eye follows one screen into the next
  /// instead of being handed a different window, short enough that nobody
  /// waiting to type a room code is kept waiting for it.
  void go_to_page(int index);

  /// Puts the microphone meter back to nothing and stops animating it.
  void quiet_level();

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
  /// Every room that exists, so that arriving here does not require already
  /// knowing a six character code. See build_home_page.
  QTableWidget* room_list_ = nullptr;
  QLabel* rooms_empty_ = nullptr;
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
  QPushButton* network_status_button_ = nullptr;
  QPushButton* leave_button_ = nullptr;
  QLabel* sharing_label_ = nullptr;

  /// The charts, while they are open. A QPointer and not a raw one because the
  /// window deletes itself when it is closed - by its own Close button, by the
  /// title bar, or by noticing that the call it belongs to has ended - and a
  /// raw pointer would be left aimed at the space where it was.
  QPointer<MetricsDialog> metrics_dialog_ = nullptr;

  /// What the microphone meter is drawing, and the clock it moves against.
  ///
  /// A clock of its own rather than counting frames: a timer set to sixteen
  /// milliseconds fires when the event loop gets to it, and a bar that falls
  /// at one speed on an idle machine and half of it under load reads as the
  /// interface stuttering.
  client::app::LevelMeter level_;
  QTimer* level_timer_ = nullptr;
  QElapsedTimer level_clock_;
  qint64 level_drawn_at_ms_ = 0;

  /// The quality on screen, so the indicator is only restyled when it changes.
  /// Setting a stylesheet re-resolves the widget's whole style, and the
  /// measurement behind this arrives on a timer whether it moved or not.
  int shown_quality_ = -1;

  /// The last round trip to the signaling server, in milliseconds, or -1 for
  /// no current measurement. Kept because the indicator is repainted from it
  /// on leaving a room, rather than left blank until the next probe.
  int link_round_trip_ms_ = -1;
  /// The link verdict on screen, for the reason `shown_quality_` exists. Its
  /// own, because the two take turns owning the same label and one resetting
  /// the other's memory would skip a restyle that was needed.
  int shown_link_quality_ = -1;

  // Chat. Still a list of lines rather than a document, and still one that
  // renders none of the markup somebody types into it: ChatView escapes every
  // byte it did not itself recognise as a URL. See ui/chat_view.hpp.
  ChatView* chat_view_ = nullptr;
  QLineEdit* chat_input_ = nullptr;
  /// Opens a short curated grid. The system picker, which every platform has,
  /// covers everything this one leaves out and works in the same field.
  QPushButton* chat_emoji_ = nullptr;
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
  /// What the next share should carry besides the picture, as the settings
  /// dialog last left it. Read from the configuration on the first share, so
  /// that somebody who never opens the dialog still gets what their file says.
  std::optional<client::app::ScreenAudio> screen_audio_;
};

}  // namespace dv::ui
