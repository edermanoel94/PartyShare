#pragma once

#include <cstdint>
#include <memory>

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>

#include "app/call_session.hpp"
#include "app/network_quality.hpp"
#include "app/smoothing.hpp"
#include "ui/notifier.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace dv::ui {

class AdminPanel;
class ChatView;
class ElidedLabel;
class MetricsDialog;
class ScreenView;
class UpdateChecker;

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
// The destructor exists to take the session's handlers back and stop the
// threads that call them before any of this window is taken apart - see its
// body for why clearing them is not on its own enough - and asking for the
// other four special members here would be asking for members that must not
// exist.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  /// `updates` has to outlive this window. It is not started here and not read
  /// here: the window passes it to the settings dialog, which is where it can
  /// be switched on and off, and hears about a new release through the
  /// announce_update slot below.
  MainWindow(client::app::CallSession& session, UpdateChecker& updates, QWidget* parent = nullptr);
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

  /// A release newer than this build has been published.
  ///
  /// Turns the version already in the status bar into the sentence "0.1.41 -
  /// 0.1.42 available", where the second half is a link to the download page.
  /// Nothing is interrupted and nothing has to be dismissed: this arrives while
  /// somebody may be in a call, and a call is not the moment to be handed a
  /// modal window about housekeeping.
  ///
  /// Idempotent. ui::UpdateChecker raises this once per version, but calling it
  /// twice with the same one leaves the same label.
  void announce_update(const QString& version, const QUrl& page);

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
  /// Asks which monitor to share when there is more than one, in a menu under
  /// the share button, and leaves the answer in `monitor_id_`. With a single
  /// monitor there is nothing to ask and it answers true at once. False means
  /// the menu was dismissed, and no share should start.
  bool choose_monitor();
  /// Opens the change-password form, and sends what it collected.
  ///
  /// Home screen only, which is where it is offered. Succeeding ends the
  /// session, and doing that from inside a call would drop the call as a side
  /// effect of a settings-shaped action.
  void on_change_password();
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
  /// is no current measurement, and whether the socket is open at all.
  ///
  /// This is what keeps the network indicator alive outside a call, which is
  /// where it was blank before: the call metrics stop when the call does, and
  /// "is my connection any good" is asked hardest on the lobby screen. Before
  /// that it is asked on the login screen as "is the server there", which is
  /// what `connected` answers.
  void apply_link(int round_trip_ms, bool connected);
  void apply_local_level(double level, bool speaking);
  /// Moves the microphone meter one frame closer to the last measurement.
  ///
  /// The measurements arrive five times a second and the bar is drawn sixty,
  /// because five positions a second is not movement, it is a slideshow. See
  /// app::LevelMeter for what decides how fast it is allowed to travel.
  void animate_level();
  void apply_error(const QString& code, const QString& message);
  /// The server changed the password and revoked every session of the account.
  ///
  /// Signs out and says why on the login screen. The session is already dead
  /// on the server's side by the time this runs; disconnecting here is what
  /// makes this side agree with it.
  void apply_password_changed();
  /// The server ended this session - a ban, a deleted account, an operator
  /// signing the person out from the terminal, a changed password - and said
  /// why in `reason`.
  ///
  /// The session has already forgotten its identity and its room by the time
  /// this runs, so the login page is up; what is left is to say why, and to
  /// leave the window in exactly the state the sign out button leaves it.
  void apply_session_ended(const QString& reason);
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
  /// An administrator's message to this account: a box with one button.
  ///
  /// Modal on purpose, which nothing else the server announces is. A
  /// restriction explains a control that stopped working and can wait for the
  /// person to look; this is somebody addressing them directly and expecting
  /// an answer, and the answer is the whole feature - until it is sent, the
  /// server hands the same notice over again at every sign-in.
  ///
  /// Strings and not the model, for the reason the restriction line above is:
  /// crossing threads with a queued connection needs a registered metatype and
  /// this needs none.
  void apply_notice(const QString& notice_id, const QString& from, const QString& text);
  /// A notice this administrator sent was written down. One line, not a box:
  /// they are the one who just typed it.
  void apply_notice_sent(const QString& user_id);
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

  /// The one way back to the login screen, with `text` under the sign in
  /// button: the identity and the socket dropped, the password field emptied,
  /// the link indicator reset and a fresh probe sent so that it describes the
  /// server the form is about to talk to.
  ///
  /// One function because there are three ways to arrive here - the sign out
  /// button, a changed password, a session the server ended - and three
  /// transcriptions of the same steps is how one of them stops sending the
  /// probe and leaves a stale indicator over an empty form.
  void return_to_login(const QString& text);

  void build_login_page();
  void build_home_page();
  void build_room_page();
  void build_admin_page();
  void wire_session();
  void refresh_controls();
  /// Adds one line and keeps the view at the bottom, which is where a
  /// conversation is read from.
  void append_chat_line(const QString& line);

  /// Says that `names` have just walked in, through the operating system, and
  /// returns whether a balloon went up.
  ///
  /// Only when the window is not the one being looked at: see
  /// Notifier::window_has_attention. The answer is what decides whether the
  /// chime plays too, because a balloon brings its own sound - see
  /// Notifier::notify.
  [[nodiscard]] bool announce_arrivals(const QStringList& names);

  /// Forgets who was in the room, so that the next room's first list seeds
  /// rather than announces.
  void forget_participants();
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

  /// Writes the current room's name into the title above the call. Called both
  /// when the room is entered and whenever a room list arrives, because the
  /// name may only be known on the second of those.
  void refresh_room_title();

  client::app::CallSession& session_;
  /// Held only to hand to the settings dialog, which is where the switch for
  /// it lives. This window neither starts it nor asks it anything; what it
  /// hears from it arrives through announce_update.
  UpdateChecker& updates_;

  QStackedWidget* pages_ = nullptr;

  // Login.
  QLineEdit* username_ = nullptr;
  QLineEdit* password_ = nullptr;
  QPushButton* connect_button_ = nullptr;
  QLabel* login_error_ = nullptr;

  // Home.
  QLabel* welcome_ = nullptr;
  QLineEdit* room_id_ = nullptr;
  /// What the room about to be created is called. Empty is the ordinary case
  /// and not an omission: the server names such a room after its identifier.
  QLineEdit* room_name_ = nullptr;
  /// How many people the next room will hold. Kept at what was last chosen
  /// rather than reset after a creation: the name is cleared because it now
  /// belongs to a room, and a size belongs to nobody.
  QSpinBox* room_capacity_ = nullptr;
  QPushButton* create_button_ = nullptr;
  QPushButton* join_button_ = nullptr;
  /// Every room that exists, so that arriving here does not require already
  /// knowing a six character code. See build_home_page.
  QTableWidget* room_list_ = nullptr;
  QLabel* rooms_empty_ = nullptr;
  /// Identifier to name, kept from the last room list.
  ///
  /// The title above a call wants the name, and nothing in the join exchange
  /// carries it: the server answers a join with who is in the room, not with
  /// what it is called. The list is broadcast to every signed in client on
  /// every arrival and departure, so this is never long out of date, and a
  /// room missing from it falls back to its identifier.
  QHash<QString, QString> room_names_;
  /// "3/10" by room identifier, from the same list, for the title above a
  /// call. Kept as the text the column shows rather than as two numbers,
  /// because that is the only form anything reads it in.
  QHash<QString, QString> room_people_;

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
  /// Shown only to an administrator. See on_open_administration.
  QPushButton* admin_button_ = nullptr;
  QPushButton* leave_button_ = nullptr;
  QLabel* sharing_label_ = nullptr;

  /// The charts, while they are open. A QPointer and not a raw one because the
  /// window deletes itself when it is closed - by its own Close button, by the
  /// title bar, or by noticing that the call it belongs to has ended - and a
  /// raw pointer would be left aimed at the space where it was.
  QPointer<MetricsDialog> metrics_dialog_ = nullptr;

  /// The jitter buffer's totals at the last metrics report, so that the status
  /// bar can say what share of the last interval was invented rather than a
  /// figure for the whole call. docs/16-audio-plan.md, step 9.
  std::uint64_t status_concealed_samples_ = 0;
  std::uint64_t status_total_samples_ = 0;

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
  /// Whether the signaling socket is open: 1 when it is, 0 when the server
  /// refused it or it dropped, and -1 before the first attempt has been
  /// answered either way. Three values because "not known yet" and "not
  /// there" are drawn differently: one is a wait, the other is the answer.
  int link_connected_ = -1;
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

  // Status bar.
  QLabel* status_ = nullptr;
  QLabel* quality_ = nullptr;
  /// Ends in an ellipsis when the bar runs short, rather than mid-word.
  ElidedLabel* metrics_ = nullptr;
  /// Which build this is, at the right hand end of the status bar.
  ///
  /// Written in the constructor from a number decided when the binary was
  /// compiled. It is on the window rather than behind an About dialog because
  /// the question it answers - "is the person I am in a room with running the
  /// same build as me" - is asked while looking at the window, and an answer
  /// two clicks away gets guessed at instead of looked up.
  ///
  /// announce_update is the one thing that changes it afterwards, and it is the
  /// same question from the other side: a version is only interesting next to
  /// the version it should be.
  QLabel* version_ = nullptr;

  client::app::CallSession::State state_ = client::app::CallSession::State::Idle;
  /// Whose volume the slider is showing. Empty when nobody is selected.
  QString selected_participant_;
  /// The volume applied to each participant, by user id, as a percentage.
  /// Anyone missing is at 100, which is the volume they were sent at.
  QHash<QString, int> volumes_;
  /// The monitor to share, as the share button's menu or the settings dialog
  /// last chose it. Empty for the primary one, which is what the capturer
  /// makes of an empty id as well.
  QString monitor_id_;
  /// What the next share should carry besides the picture, as the settings
  /// dialog last left it. Read from the configuration on the first share, so
  /// that somebody who never opens the dialog still gets what their file says.
  std::optional<client::app::ScreenAudio> screen_audio_;

  /// Raises the notification when somebody joins a room this window is not
  /// showing, and plays the chime whether it is showing or not.
  Notifier notifier_{this};
  /// Who was in the room the last time the list was rebuilt, by user id.
  ///
  /// The server sends the whole membership every time it changes rather than a
  /// join event, so an arrival is a difference between two of those lists and
  /// there is nowhere else to keep the first one.
  QSet<QString> known_participants_;
  /// Whether `known_participants_` has seen a list yet.
  ///
  /// The first list after joining is everybody who was already there, and
  /// announcing it would greet somebody with five balloons and five chimes for
  /// walking into a room of five people. It seeds the set and says nothing.
  bool participants_seeded_ = false;
};

}  // namespace dv::ui
