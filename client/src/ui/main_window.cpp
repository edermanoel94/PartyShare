#include "ui/main_window.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>
#include <dv/models/chat.hpp>
#include <dv/models/room.hpp>

#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "app/server_address.hpp"
#include "app/smoothing.hpp"
#include "media/media_session.hpp"
#include "ui/admin_panel.hpp"
#include "ui/attention.hpp"
#include "ui/chat_view.hpp"
#include "ui/chimes.hpp"
#include "ui/components/app_dialog.hpp"
#include "ui/components/app_header.hpp"
#include "ui/components/app_sidebar.hpp"
#include "ui/components/avatar.hpp"
#include "ui/components/buttons.hpp"
#include "ui/components/empty_state.hpp"
#include "ui/components/floating_bar.hpp"
#include "ui/components/icon_button.hpp"
#include "ui/components/icons.hpp"
#include "ui/components/search_input.hpp"
#include "ui/components/stat_card.hpp"
#include "ui/components/toast.hpp"
#include "ui/elided_label.hpp"
#include "ui/emoji_picker.hpp"
#include "ui/metrics_dialog.hpp"
#include "ui/participant_delegate.hpp"
#include "ui/password_dialog.hpp"
#include "ui/screen_view.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/update_checker.hpp"
#include "ui/update_dialog.hpp"

namespace dv::ui {
namespace {

/// The pages of the stack, in the order section 19 of SPEC.md walks through
/// them.
constexpr int kLoginPage = 0;
constexpr int kHomePage = 1;
constexpr int kRoomPage = 2;
constexpr int kAdminPage = 3;

/// Where the participant's plain name is kept on a list item, next to the
/// user id in Qt::UserRole. The state is on its own role and drawn as icons
/// by ParticipantDelegate; see ui/participant_delegate.hpp.
constexpr int kNameRole = kParticipantNameRole;

/// "Nothing is being measured" as a value the indicator can remember having
/// drawn. Outside the NetworkQuality range on purpose, so it can never be
/// mistaken for one of the three verdicts.
constexpr int kNoLinkQuality = -2;
/// "The server is not there", likewise: its own value because it is drawn in
/// its own colour, and the indicator only restyles when the value moves.
constexpr int kLinkOffline = -3;

/// The colour of a verdict, from the theme rather than from a literal, so the
/// three of them follow the colour scheme instead of staying at their
/// light-window values.
///
/// Shared by the two indicators. They make different claims - one weighs three
/// measurements, the other one - but the same word has to mean the same colour
/// in both, or the dot changes meaning as somebody walks into a room.
[[nodiscard]] QColor quality_colour(client::app::NetworkQuality quality) {
  switch (quality) {
    case client::app::NetworkQuality::Good:
      return theme::colors().success;
    case client::app::NetworkQuality::Fair:
      return theme::colors().warn;
    case client::app::NetworkQuality::Poor:
      return theme::colors().danger;
    case client::app::NetworkQuality::Unknown:
      break;
  }
  return theme::colors().muted;
}

/// A run of UTF-8 bytes as a QString.
///
/// The core hands out string_view - it may not name a Qt type - and every
/// place that shows one of its words was spelling this conversion out.
[[nodiscard]] QString from_utf8(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// One field of a tab separated row, with anything that could end it early
/// turned into a space.
///
/// The rows this window builds are split back apart by field number on the
/// other side, so a value carrying a tab does not corrupt itself - it moves
/// every field after it, and the reader takes the wrong one. That is how a
/// display name became a way to put somebody else's identifier in the field a
/// moderation menu acts on.
///
/// The server refuses control characters in the names it accepts now, in
/// models::is_valid_display_name and models::is_valid_room_name. This is the
/// same rule applied where the rows are actually built, so that it holds for a
/// value that never passed through either check: a name already in the store
/// from before those existed, a record written straight into the database, or
/// a field that has nothing to do with names at all.
[[nodiscard]] QString as_field(const std::string& text) {
  QString field = QString::fromStdString(text);
  for (QChar& character : field) {
    if (character < QChar(0x20) || character == QChar(0x7F)) {
      character = QLatin1Char(' ');
    }
  }
  return field;
}

/// The places on the navigation rail. Two, where there are four pages: the
/// list of rooms and the room are one place, because which of the two is on
/// screen is decided by whether there is a call and never by a click.
constexpr int kRoomsDestination = 0;
constexpr int kAdminDestination = 1;

/// The two tabs of the room's side panel, in the order they are added.
constexpr int kPeopleTab = 0;
constexpr int kChatTab = 1;

/// The sign-in card: its width, the mark over the form, the button under it.
constexpr int kLoginCardWidth = 400;
constexpr int kLoginMark = 40;
constexpr int kLoginButtonHeight = 42;

/// The badge at the foot of the rail.
constexpr int kAccountAvatar = 34;

/// The lobby. The search box stretches between these; a card is never
/// narrower than its minimum, and the grid never has more columns than this
/// however wide the window is dragged - past four, a row of rooms is a strip
/// of labels too far apart to scan.
constexpr int kSearchMinWidth = 260;
constexpr int kSearchMaxWidth = 320;
constexpr int kRoomCardMinWidth = 240;
constexpr int kMaxRoomColumns = 4;
constexpr int kOccupancyBarHeight = 6;
constexpr int kCreateDialogWidth = 420;

/// The question about a new release. How long after a page arrives it is
/// asked - leaving a call and being handed a dialog in the same frame reads as
/// the dialog having caused it - and how long it waits before looking again
/// when another dialog had the window.
constexpr int kUpdateSettleMs = 400;
constexpr int kUpdateRetryMs = 3000;

/// The room. The code chip in the header, the buttons of the floating
/// controls, how far the controls rest above the bottom of the stage, and how
/// long the pointer has to be still before they leave.
constexpr int kCodeChipHeight = 30;
constexpr int kDockButton = 44;
constexpr int kDockButtonRadius = 12;
constexpr int kDockLift = 6;
constexpr int kDockIdleMs = 3000;

/// The side panel of the room: how narrow it may be dragged, the height of
/// the microphone meter, the row the message field and its buttons share,
/// the faces above the tabs and how many of them fit before "+N".
constexpr int kSidePanelMinWidth = 260;
constexpr int kMeterHeight = 6;
constexpr int kChatRowHeight = 38;
constexpr int kFace = 28;
constexpr int kMaxFaces = 7;

/// How far down from the top of the pages the notifications start: clear of
/// a page's header, which is where its title and its actions are.
constexpr int kToastTop = 64;

/// One line of the conversation as it is shown: when it was said, by whom, and
/// what.
///
/// Built where the message arrives, on the signaling thread, because a QString
/// crosses to the interface thread without a registered metatype and a
/// models::ChatMessage would not.
[[nodiscard]] QString to_chat_line(const models::ChatMessage& message) {
  const QString name =
      QString::fromStdString(message.display_name.empty() ? message.user_id : message.display_name);
  const QString text = QString::fromStdString(message.text);
  if (message.timestamp_seconds <= 0) {
    return QStringLiteral("%1: %2").arg(name, text);
  }
  const QString when =
      QDateTime::fromSecsSinceEpoch(message.timestamp_seconds).toString(QStringLiteral("HH:mm"));
  return QStringLiteral("[%1] %2: %3").arg(when, name, text);
}

[[nodiscard]] QString to_display(client::app::CallSession::State state) {
  switch (state) {
    case client::app::CallSession::State::Idle:
      return QStringLiteral("disconnected");
    case client::app::CallSession::State::Connecting:
      return QStringLiteral("connecting");
    case client::app::CallSession::State::Authenticated:
      return QStringLiteral("connected");
    case client::app::CallSession::State::InCall:
      return QStringLiteral("in call");
    case client::app::CallSession::State::Failed:
      return QStringLiteral("failed");
  }
  return QStringLiteral("unknown");
}

/// Turns an audio level into how much of a meter to fill.
///
/// The level arrives as a linear amplitude from 0 to 1, which is the wrong
/// scale for an eye: ordinary speech sits around a twentieth of full scale and
/// would barely lift a linear bar off the floor. Meters are read in decibels,
/// so the bottom of this one is -60 dBFS and the top is full scale.
/// How many steps the microphone meter has.
///
/// A thousand and not a hundred. The bar is drawn sixty times a second now, and
/// on a hundred steps a bar eight pixels tall crossing its whole width takes a
/// hundred visible stops - which is the staircase the animation exists to get
/// rid of, at a tenth of the size.
constexpr int kMeterSteps = 1000;

/// How long the incoming page takes to fade in.
constexpr int kPageFadeMs = 140;

/// The shake a nudge gives the window: one frame every sixteen milliseconds,
/// thirty of them, so just under half a second - long enough to be seen from
/// the corner of an eye, short enough to be over before it is in the way of
/// the thing on screen. The reach is the first swing in pixels, and it falls
/// to nothing over the thirty frames. See MainWindow::shake.
constexpr int kShakeFrameMs = 16;
constexpr int kShakeSteps = 30;
constexpr int kShakeReach = 12;

/// How wide a window's title may run in the share menu before it is cut. A
/// browser tab's title can be a whole sentence, and a menu as wide as the
/// longest of forty of them is a menu that covers the call.
constexpr int kWindowTitleWidth = 420;

/// How often the microphone meter is redrawn while it is moving. Sixty a
/// second, and stopped the moment it has nothing left to do.
constexpr int kLevelFrameMs = 16;

/// What to put in front of the user for an error the core reported.
///
/// Section 19 of SPEC.md names four of these by hand, and the point of the
/// table is that a person reads a sentence about their situation rather than
/// an identifier from the protocol.
[[nodiscard]] QString describe(const QString& code, const QString& message) {
  static const QHash<QString, QString> kKnown = {
      {QStringLiteral("room_not_found"), QStringLiteral("That room does not exist.")},
      {QStringLiteral("room_full"), QStringLiteral("The room is full.")},
      // The name is left in the field rather than cleared, so that picking a
      // different one is an edit and not a retype. Only a room that was
      // actually created clears it. See apply_room_created.
      {QStringLiteral("room_name_taken"),
       QStringLiteral("Another room already has that name. Pick a different one, or leave the "
                      "field empty to be given the room code.")},
      {QStringLiteral("unauthorized"), QStringLiteral("Wrong username or password.")},
      // Its own code rather than `unauthorized`, because the session asking is
      // perfectly valid and one field of a form is wrong. See
      // Authenticator::change_password.
      {QStringLiteral("invalid_password"),
       QStringLiteral("The current password is not correct. The password was not changed.")},
      {QStringLiteral("not_connected"),
       QStringLiteral("No connection to the server. Trying again.")},
      {QStringLiteral("capture_denied"),
       QStringLiteral("Permission to capture the screen was denied.")},
      {QStringLiteral("capture_unavailable"),
       QStringLiteral("This system has no way to capture the screen.")},
      {QStringLiteral("capture_failed"),
       QStringLiteral("Screen capture stopped. The monitor may have been disconnected.")},
      {QStringLiteral("monitor_not_found"), QStringLiteral("That monitor no longer exists.")},
      {QStringLiteral("window_not_found"), QStringLiteral("That window is no longer open.")},
      {QStringLiteral("window_minimized"),
       QStringLiteral("That window is minimized. Restore it, then share it.")},
      {QStringLiteral("window_closed"),
       QStringLiteral("Screen share stopped: the shared window was closed.")},
      {QStringLiteral("window_capture_failed"),
       QStringLiteral("Screen share stopped. The system stopped drawing that window.")},
      {QStringLiteral("screen_share_busy"),
       QStringLiteral("Someone else is already sharing their screen.")},
      {QStringLiteral("media_unavailable"),
       QStringLiteral("This build was compiled without audio and video.")},
      {QStringLiteral("device_not_found"), QStringLiteral("That device no longer exists.")},
  };

  if (const auto it = kKnown.find(code); it != kKnown.end()) {
    return it.value();
  }
  // Nothing invented for a code nobody wrote a sentence for: the raw message
  // is worse to read but at least it is true.
  return message.isEmpty() ? code : message;
}

}  // namespace

MainWindow::MainWindow(client::app::CallSession& session, QWidget* parent)
    : QMainWindow(parent),
      session_(session),
      shell_(new QWidget(this)),
      pages_(new QStackedWidget(this)),
      // What the session starts with came from the configuration, so the
      // first sign-in to it has nothing to write down.
      remembered_server_(session.signaling_url()),
      level_timer_(new QTimer(this)) {
  setWindowTitle(QStringLiteral("PartyShare"));
  setMinimumSize(780, 560);
  resize(1100, 740);

  // The frame every page stands in: the navigation rail, and the pages beside
  // it. No margin and no gap, so the rail runs the height of the window and
  // each page is free to bring its own header up to the top edge.
  auto* frame = new QHBoxLayout(shell_);
  frame->setContentsMargins(0, 0, 0, 0);
  frame->setSpacing(0);
  build_sidebar();
  frame->addWidget(sidebar_);
  frame->addWidget(pages_, 1);
  setCentralWidget(shell_);
  // Nothing to navigate before there is somebody signed in. See go_to_page.
  sidebar_->setVisible(false);

  build_login_page();
  build_home_page();
  build_room_page();
  build_admin_page();

  // The notification area: the top of whatever page is up, clear of its
  // header.
  toasts_ = new ToastHost(pages_);
  toasts_->set_margins(QMargins(0, kToastTop, 0, 0));

  // The two things here a change of theme does not reach on its own. The
  // network indicator is coloured inline, and only when its verdict moves, so
  // it has to be told that what it believes is on screen no longer is.
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    shown_quality_ = -1;
    shown_link_quality_ = -1;
    if (state_ != client::app::CallSession::State::InCall) {
      show_link_quality();
    }
  });

  status_ = new QLabel(QStringLiteral("disconnected"), this);
  quality_ = new QLabel(QString{}, this);
  metrics_ = new ElidedLabel(this);
  QFont small = metrics_->font();
  small.setPointSize(small.pointSize() - 1);
  metrics_->setFont(small);
  metrics_->setProperty("hint", true);

  // DV_VERSION is the project version, handed to the compiler by
  // shared/CMakeLists.txt, so this and `--version` on the command line cannot
  // drift apart: there is one number and it lives in the top level
  // CMakeLists.txt.
  version_ = new QLabel(QStringLiteral("PartyShare " DV_VERSION), this);
  version_->setFont(small);
  version_->setProperty("hint", true);
  // Selectable, because the first thing a bug report is asked for is the
  // version, and a number that has to be retyped is a number that gets sent
  // wrong.
  version_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // The Qt actually loaded rather than the one this was built against: where
  // Qt is a shared library the two are allowed to differ, and the one that
  // explains a misbehaving window is the one in the process.
  version_->setToolTip(
      QStringLiteral("PartyShare %1, on Qt %2").arg(QStringLiteral(DV_VERSION), qVersion()));

  statusBar()->addWidget(status_);
  statusBar()->addWidget(quality_);
  statusBar()->addPermanentWidget(metrics_);
  // Last, so that it sits against the right hand edge and stays there. The
  // permanent widgets are laid out in the order they are added, and the call
  // metrics beside it change width every second; the other way round, the
  // version would slide back and forth for the length of a call.
  statusBar()->addPermanentWidget(version_);

  // Runs only while the meter has somewhere to go. A timer beating sixty times
  // a second for the whole time a window is open, to redraw a bar that is not
  // moving, is a program that never lets a laptop idle.
  level_timer_->setInterval(kLevelFrameMs);
  connect(level_timer_, &QTimer::timeout, this, &MainWindow::animate_level);
  level_clock_.start();

  wire_session();
  refresh_controls();
  show_page();

  // The indicator says something from the first frame, and the socket that
  // will answer it is opened now rather than at the first sign-in. Somebody
  // looking at the login screen is asking whether the server is there before
  // they are asking anything else, and a form that only finds out by being
  // submitted answers that question with an error.
  show_link_quality();
  if (const auto probed = session_.probe_server(); !probed) {
    // A refused address - empty, or not ws:// - is what the Server field on
    // the login screen is for, and the indicator saying "offline" until it is
    // fixed is the right amount of noise about it.
    link_connected_ = 0;
    show_link_quality();
  }
}

MainWindow::~MainWindow() {
  // Taking the handlers back before any of this window is taken apart.
  //
  // wire_session installs eighteen handlers that capture this, and the session
  // calls them from three places that are not the interface thread: the
  // signaling socket's own threads, the metrics thread, and libwebrtc's decode
  // thread. main() declares the session before the window so that it outlives
  // it, which means every one of those is live for as long as this destructor
  // takes to run - and tearing down four pages of widgets and a tray icon is
  // not instant.
  //
  // Seventeen of the eighteen post a queued invocation and would survive being
  // late, because a QObject drops its own posted events when it goes. The
  // eighteenth does not: on_remote_video calls screen_view_->submit straight
  // from the decode thread, thirty times a second while a share is running, so
  // quitting mid-share is a use-after-free with a stack in a codec.
  //
  // Clearing first, then quiescing, and the order is the point. Clearing alone
  // fixes nothing: every dispatch site copies the callback struct under the
  // mutex and invokes it with the lock released, so a thread already past that
  // copy still holds these lambdas and will run them. What actually closes the
  // window is disconnect(), which joins the metrics thread, closes the media
  // session - whose close() detaches the remote video sink before it returns -
  // and calls SignalingClient::disconnect, which waits for a callback that is
  // running. Clearing first only makes disconnect()'s own closing set_state
  // find nothing to report, which saves posting to a window that is going.
  //
  // When this returns, no thread can reach these widgets, and only then does
  // the widget tree start to come apart.
  session_.on_events({});
  session_.on_room_created(nullptr);
  session_.disconnect();
}

void MainWindow::show_login_error(const QString& text) {
  login_error_->setText(text);
  login_error_->setVisible(!text.isEmpty());
}

void MainWindow::build_sidebar() {
  sidebar_ = new AppSidebar(shell_);

  // One place for both of the first two pages: the list of rooms while
  // nobody is in one, the room while somebody is. They are the same answer to
  // "take me to the call", and the state decides which - see show_page.
  sidebar_->add_destination(kRoomsDestination, Icon::Rooms, QStringLiteral("Rooms"));

  // Accounts, rooms, restrictions and the audit log. Hidden rather than
  // disabled from everybody else - a greyed out button that most people can
  // never use is a permanent reminder of a feature that is not theirs. See
  // refresh_controls.
  admin_button_ =
      sidebar_->add_destination(kAdminDestination, Icon::Shield, QStringLiteral("Administration"));
  admin_button_->setToolTip(
      QStringLiteral("Administration: accounts, rooms, restrictions and the audit log. A call "
                     "carries on behind it."));
  admin_button_->setVisible(false);

  // Settings is reachable from every page the rail is on, so the microphone
  // and the monitor can be sorted out before there is anybody on the other
  // end to hear the result, and changed while there is.
  settings_button_ = sidebar_->add_action(Icon::Settings, QStringLiteral("Settings"));

  // The account, at the bottom with Settings: both are about this machine or
  // this person, and neither is what somebody came to a page to do.
  account_avatar_ = new Avatar(kAccountAvatar, sidebar_);
  account_avatar_->set_interactive(true);
  sidebar_->add_footer(account_avatar_);

  connect(sidebar_, &AppSidebar::destination_chosen, this, [this](int destination) {
    if (destination == kAdminDestination) {
      on_open_administration();
      return;
    }
    // Back to the call, or to the list of rooms: the same way out of the
    // administration panel its own Back button takes.
    on_close_administration();
  });
  connect(settings_button_, &IconButton::clicked, this, &MainWindow::on_open_settings);
  connect(account_avatar_, &Avatar::clicked, this, &MainWindow::on_account_menu);
}

void MainWindow::build_login_page() {
  auto* page = new QWidget(pages_);
  auto* outer = new QVBoxLayout(page);
  outer->addStretch();

  auto* card = new Card(page);
  card->setFixedWidth(kLoginCardWidth);
  card->body()->setContentsMargins(theme::space::kXl, theme::space::kXl, theme::space::kXl,
                                   theme::space::kXl);
  card->body()->setSpacing(theme::space::kLg);

  // The program's own mark and name over the form. This is the first thing
  // anybody sees of it, and a bare form with "Sign in" over it could be the
  // login of anything.
  auto* mark = new QLabel(card);
  mark->setPixmap(QIcon(QStringLiteral(":/partyshare-48.png")).pixmap(kLoginMark, kLoginMark));
  auto* name = new QLabel(QStringLiteral("PartyShare"), card);
  theme::set_role(name, theme::TextRole::Heading);
  auto* tagline = new QLabel(QStringLiteral("Sign in to your server"), card);
  theme::set_role(tagline, theme::TextRole::Caption);
  auto* names = new QVBoxLayout();
  names->setSpacing(0);
  names->addWidget(name);
  names->addWidget(tagline);
  auto* brand = new QHBoxLayout();
  brand->setSpacing(theme::space::kMd);
  brand->addWidget(mark);
  brand->addLayout(names, 1);

  auto* form = new AppForm(card);

  // The server first, above the name, because it is the question the other
  // two depend on: a username is only a username somewhere.
  //
  // It is on the form and not in config.ini alone, and the history is why.
  // The address used to be a row in Settings, reachable from this screen
  // through a button that left in 0.1.55 and came back in 0.1.57 because a
  // wrong address in config.ini had left editing the file by hand as the only
  // way in. Both arrangements asked somebody who had been handed
  // "192.168.1.10" to know that a WebSocket URL was what the program wanted,
  // and to find the row for it. This field takes the address as it was handed
  // over - app::expand_server_address says what it makes of it - and the
  // button beside it asks the server whether it is there, which was otherwise
  // a question answered by a failed sign-in.
  server_ = new QLineEdit(card);
  server_->setPlaceholderText(QStringLiteral("192.168.1.10 or party.example.com"));
  server_->setToolTip(
      QStringLiteral("An IP or a name, with a port after a colon when the server is not on "
                     "8080. ws:// is added; wss://, for a server behind TLS, is typed."));
  server_->setText(
      QString::fromStdString(client::app::display_server_address(session_.signaling_url())));
  test_server_button_ = new QPushButton(QStringLiteral("Test"), card);
  test_server_button_->setToolTip(
      QStringLiteral("Asks the server at this address whether it is there, and how far away."));

  auto* server_row = new QHBoxLayout();
  server_row->setSpacing(theme::space::kSm);
  server_row->addWidget(server_, 1);
  server_row->addWidget(test_server_button_);

  server_hint_ = new QLabel(QString{}, card);
  server_hint_->setWordWrap(true);
  server_hint_->setVisible(false);

  username_ = new QLineEdit(card);
  username_->setPlaceholderText(QStringLiteral("username"));
  password_ = new QLineEdit(card);
  password_->setEchoMode(QLineEdit::Password);
  password_->setPlaceholderText(QStringLiteral("password"));

  form->layout_form()->addRow(QStringLiteral("Server"), server_row);
  form->add_wide(server_hint_);
  form->add_row(QStringLiteral("Username"), username_);
  form->add_row(QStringLiteral("Password"), password_);

  connect_button_ = new PrimaryButton(QStringLiteral("Connect"), card);
  connect_button_->setDefault(true);
  connect_button_->setMinimumHeight(kLoginButtonHeight);

  login_error_ = new QLabel(QString{}, card);
  login_error_->setWordWrap(true);
  // The colour comes from the theme rather than from a literal here. It used to
  // be a hard coded #c62828, which is legible on a light window and very nearly
  // invisible on a dark one.
  login_error_->setProperty("error", true);
  login_error_->setVisible(false);

  card->body()->addLayout(brand);
  card->body()->addWidget(form);
  card->body()->addWidget(connect_button_);
  card->body()->addWidget(login_error_);

  auto* centred = new QHBoxLayout();
  centred->addStretch();
  centred->addWidget(card);
  centred->addStretch();
  outer->addLayout(centred);
  outer->addStretch();

  connect(connect_button_, &QPushButton::clicked, this, &MainWindow::on_connect);
  connect(test_server_button_, &QPushButton::clicked, this, &MainWindow::on_test_server);
  connect(server_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  connect(username_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  connect(password_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  // A sentence about an address that is no longer in the field is a sentence
  // about the wrong address. textEdited and not textChanged: this window
  // writes the field itself, and what it writes is what the sentence is about.
  connect(server_, &QLineEdit::textEdited, this, [this] {
    server_under_test_.clear();
    show_server_hint(QString{}, false);
  });

  pages_->insertWidget(kLoginPage, page);
}

void MainWindow::build_home_page() {
  auto* page = new QWidget(pages_);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);

  home_header_ = new AppHeader(page);
  home_header_->set_title(QStringLiteral("Rooms"));

  // One box for both ways of finding a room. Typing narrows the cards below
  // to the rooms whose name or code has those letters; a code somebody was
  // sent goes in the same box, and Enter or the button beside it walks in.
  // They used to be a table and a separate field, and the field stays in
  // spirit: a list is how you find a room nobody told you about, a code is
  // still how somebody hands you one particular room.
  auto* search = new SearchInput(QStringLiteral("Search rooms or paste a code"), page);
  search->setMinimumWidth(kSearchMinWidth);
  search->setMaximumWidth(kSearchMaxWidth);
  room_id_ = search;

  join_button_ = new SecondaryButton(QStringLiteral("Join"), page);
  join_button_->setToolTip(QStringLiteral("Join the room whose code is in the box."));

  // The one filled button on this page. Creating and joining are both things
  // somebody came here to do, but only one of them works without first having
  // been sent a code, so that is the one the eye should land on.
  auto* create = new PrimaryButton(QStringLiteral("New room"), page);
  create->set_leading_icon(Icon::Plus);
  create_button_ = create;

  home_header_->actions()->addWidget(search);
  home_header_->actions()->addWidget(join_button_);
  home_header_->actions()->addSpacing(theme::space::kSm);
  home_header_->actions()->addWidget(create_button_);

  auto* content = new QWidget(page);
  auto* body = new QVBoxLayout(content);
  body->setContentsMargins(theme::space::kXl, theme::space::kXl, theme::space::kXl,
                           theme::space::kLg);
  body->setSpacing(theme::space::kLg);

  // Three numbers across the top: what there is to walk into, how busy it is,
  // and whether the server is close enough to be worth walking into anything.
  // The third is the status bar's indicator said large, on the one page where
  // "is my connection any good" is the question being asked.
  stat_rooms_ = new StatCard(QStringLiteral("Open rooms"), content);
  stat_people_ = new StatCard(QStringLiteral("People in rooms"), content);
  stat_server_ = new StatCard(QStringLiteral("Server"), content);
  auto* stats = new QHBoxLayout();
  stats->setSpacing(theme::space::kLg);
  stats->addWidget(stat_rooms_, 1);
  stats->addWidget(stat_people_, 1);
  stats->addWidget(stat_server_, 1);

  auto* rooms_label = new QLabel(QStringLiteral("ROOMS"), content);
  rooms_label->setProperty("eyebrow", true);

  // Every room that exists, as a card each, so that arriving here does not
  // require already knowing a six character code. Rooms outlive their
  // participants, so there is always a list to show.
  //
  // Cards in a grid rather than rows in a table: a room is a place to go and
  // not a record to read, and the thing to do with one - walk in - is a
  // button on the card instead of a second act after selecting a row.
  room_scroll_ = new QScrollArea(content);
  room_scroll_->setFrameShape(QFrame::NoFrame);
  room_scroll_->setWidgetResizable(true);
  room_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* grid_host = new QWidget(room_scroll_);
  auto* grid_column = new QVBoxLayout(grid_host);
  grid_column->setContentsMargins(0, 0, 0, 0);
  room_grid_ = new QGridLayout();
  room_grid_->setContentsMargins(0, 0, 0, 0);
  room_grid_->setSpacing(theme::space::kLg);
  grid_column->addLayout(room_grid_);
  // Under the grid, so a single row of cards stays at the top at its own
  // height instead of being stretched down the page.
  grid_column->addStretch();
  room_scroll_->setWidget(grid_host);
  // To lay the cards out again when the width changes by a whole column.
  room_scroll_->viewport()->installEventFilter(this);

  // A page with no cards is indistinguishable from a page that failed to
  // load, so the empty case says which one it is - and offers the way out.
  rooms_empty_ = new EmptyState(
      Icon::Rooms, QStringLiteral("No rooms yet"),
      QStringLiteral("Create the first one. Everybody signed in to this server will see it here."),
      content);
  QPushButton* create_first = rooms_empty_->add_action(QStringLiteral("New room"));
  rooms_empty_->setVisible(false);

  body->addLayout(stats);
  body->addSpacing(theme::space::kSm);
  body->addWidget(rooms_label);
  body->addWidget(room_scroll_, 1);
  body->addWidget(rooms_empty_, 1);

  column->addWidget(home_header_);
  column->addWidget(content, 1);

  // The form a room is made from, in a dialog of its own. It used to be two
  // fields above a button on the page; a name and a size are two answers
  // nobody needs on screen until they have decided to make a room, and the
  // page is for finding the ones that exist.
  //
  // Built once and kept, because the two fields are read by on_create_room
  // whenever it runs, and because what it is told - a name already taken -
  // arrives after the click, while the dialog is still up, and is written
  // into it. See apply_error.
  create_dialog_ = new AppDialog(QStringLiteral("New room"), this);
  create_dialog_->set_description(
      QStringLiteral("Everybody signed in to this server sees the room and can walk in."));
  create_dialog_->setMinimumWidth(kCreateDialogWidth);
  create_form_ = new AppForm(create_dialog_);

  room_name_ = new QLineEdit(create_dialog_);
  room_name_->setPlaceholderText(QStringLiteral("Named after its code if left empty"));
  // No setMaxLength. Qt counts UTF-16 code units and the server counts UTF-8
  // bytes, so a cap of 48 here would still let "Reunião de sexta" through at
  // one length and refuse it at another. The length is checked on the way out
  // instead, with the same predicate the server uses. See on_create_room.

  // How many people the room will hold. The range is what the protocol
  // accepts; the server may allow less and says so when asked for more, which
  // is rarer than the box getting in the way. Kept at what was last chosen
  // rather than reset after a creation: the name is cleared because it now
  // belongs to a room, and a size belongs to nobody.
  room_capacity_ = new QSpinBox(create_dialog_);
  room_capacity_->setRange(models::kMinRoomCapacity, models::kMaxRoomCapacity);
  room_capacity_->setValue(models::kDefaultRoomCapacity);
  room_capacity_->setSuffix(QStringLiteral(" people"));
  room_capacity_->setToolTip(QStringLiteral("How many people the room holds, you included."));

  create_form_->add_row(QStringLiteral("Name"), room_name_);
  create_form_->add_row(QStringLiteral("Size"), room_capacity_);
  create_dialog_->body()->addWidget(create_form_);
  create_dialog_->add_secondary(QStringLiteral("Cancel"));
  QPushButton* confirm = create_dialog_->add_primary(QStringLiteral("Create room"));
  // Not accepted on the click. The room exists when the server says so, and
  // until then the dialog stays, so that a refusal has somewhere to be read
  // and the name is still there to be edited rather than retyped.
  disconnect(confirm, &QPushButton::clicked, create_dialog_, &QDialog::accept);
  connect(confirm, &QPushButton::clicked, this, &MainWindow::on_create_room);

  const auto open_create = [this] {
    create_form_->show_error(QString{});
    create_dialog_->open();
    room_name_->setFocus();
  };
  connect(create_button_, &QPushButton::clicked, this, open_create);
  connect(create_first, &QPushButton::clicked, this, open_create);
  connect(join_button_, &QPushButton::clicked, this, &MainWindow::on_join_room);
  connect(room_id_, &QLineEdit::returnPressed, this, &MainWindow::on_join_room);
  // Every keystroke narrows the cards. textChanged and not textEdited, so a
  // code this window writes into the box itself is filtered on too - which is
  // why the two places that write it and then leave the page do so quietly.
  connect(room_id_, &QLineEdit::textChanged, this, [this] { rebuild_room_cards(); });

  pages_->insertWidget(kHomePage, page);
}

QWidget* MainWindow::make_room_card(const QString& id, const QString& name, int count,
                                    int capacity) {
  auto* card = new Card(room_scroll_->widget());
  card->setProperty("interactive", true);
  card->setMinimumWidth(kRoomCardMinWidth);
  card->body()->setSpacing(theme::space::kMd);

  // A room nobody named carries its own identifier as its name, so showing
  // both would read "8F42A1 / 8F42A1". The code is the title then, and the
  // line under it says why.
  const bool named = !name.isEmpty() && name != id;
  auto* title = new ElidedLabel(card);
  title->setText(named ? name : id);
  theme::set_role(title, theme::TextRole::Title);
  title->setToolTip(named ? name : id);

  // The code stays visible on a named room: it is what somebody reads out to
  // invite a person who is not looking at this page.
  auto* code = new QLabel(named ? id : QStringLiteral("no name"), card);
  if (named) {
    code->setProperty("chip", true);
    code->setTextInteractionFlags(Qt::TextSelectableByMouse);
  } else {
    code->setProperty("hint", true);
  }

  // "3 of 10" rather than "3": how many are in says nothing about whether
  // there is room for one more, and that is the question somebody reading
  // this is asking. A server from before rooms had sizes sends none, and the
  // line is a bare count again rather than "3 of 0".
  const bool full = capacity > 0 && count >= capacity;
  QString people = capacity > 0 ? QStringLiteral("%1 of %2").arg(count).arg(capacity)
                                : QStringLiteral("%1 here").arg(count);
  if (count == 0) {
    people += QStringLiteral(" · nobody yet");
  }
  auto* occupancy = new QLabel(people, card);
  theme::set_role(occupancy, theme::TextRole::Caption);

  auto* header = new QHBoxLayout();
  header->setSpacing(theme::space::kSm);
  header->addWidget(title, 1);
  if (full) {
    // The word, not only the colour: a full bar in amber says nothing to
    // somebody who cannot tell it from the accent.
    auto* chip = new QLabel(QStringLiteral("full"), card);
    chip->setProperty("chip", QStringLiteral("warn"));
    header->addWidget(chip);
  }

  auto* meta = new QHBoxLayout();
  meta->setSpacing(theme::space::kSm);
  meta->addWidget(code);
  meta->addStretch();
  meta->addWidget(occupancy);

  card->body()->addLayout(header);
  card->body()->addLayout(meta);

  if (capacity > 0) {
    auto* bar = new QProgressBar(card);
    bar->setRange(0, capacity);
    bar->setValue(std::min(count, capacity));
    bar->setTextVisible(false);
    bar->setFixedHeight(kOccupancyBarHeight);
    // The meter's own property: filled in the accent rather than the grey a
    // resting microphone meter wears. See QProgressBar in ui/theme.cpp.
    bar->setProperty("speaking", !full);
    card->body()->addWidget(bar);
  }

  // Enabled on a full room too. The server is what decides, the list can be a
  // moment old, and a button that refuses before asking would keep somebody
  // out of a room a person has just left.
  auto* join = new SecondaryButton(QStringLiteral("Join"), card);
  card->body()->addWidget(join);
  connect(join, &QPushButton::clicked, this, [this, id] { join_room(id); });
  return card;
}

void MainWindow::rebuild_room_cards() {
  // Rebuilt wholesale on every answer and every keystroke, because the answer
  // is the whole list and merging it into what is on screen would be a second
  // implementation of the truth. Deleted on the next turn of the loop: one of
  // these cards may own the button whose click is what got us here.
  while (QLayoutItem* item = room_grid_->takeAt(0)) {
    if (QWidget* card = item->widget(); card != nullptr) {
      card->hide();
      card->deleteLater();
    }
    // The layout hands the item over with takeAt, and nothing else frees it.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    delete item;
  }

  const QString wanted = room_id_->text().trimmed();
  const int spacing = room_grid_->spacing();
  const int columns =
      std::clamp((room_scroll_->viewport()->width() + spacing) / (kRoomCardMinWidth + spacing), 1,
                 kMaxRoomColumns);
  room_columns_ = columns;

  int shown = 0;
  for (const QString& row : room_rows_) {
    // Identifier, identifier, name, "3/10", then the size and the count as
    // bare numbers. See the on_room_list handler in wire_session.
    const QStringList fields = row.split(QLatin1Char('\t'));
    const QString id = fields.value(0);
    const QString name = fields.value(2);
    if (!wanted.isEmpty() && !id.contains(wanted, Qt::CaseInsensitive) &&
        !name.contains(wanted, Qt::CaseInsensitive)) {
      continue;
    }
    QWidget* card = make_room_card(id, name, fields.value(5).toInt(), fields.value(4).toInt());
    room_grid_->addWidget(card, shown / columns, shown % columns);
    ++shown;
  }
  // Equal shares for the columns in use and none for the rest, or a column
  // left over from a wider window would go on taking a third of the page with
  // nothing in it.
  for (int column = 0; column < kMaxRoomColumns; ++column) {
    room_grid_->setColumnStretch(column, column < columns ? 1 : 0);
  }

  // Two kinds of nothing, and they are not the same sentence: a server with
  // no rooms, and rooms the letters in the box ruled out.
  const bool none_at_all = room_rows_.isEmpty();
  if (none_at_all) {
    rooms_empty_->set_title(QStringLiteral("No rooms yet"));
    rooms_empty_->set_hint(QStringLiteral(
        "Create the first one. Everybody signed in to this server will see it here."));
  } else if (shown == 0) {
    rooms_empty_->set_title(QStringLiteral("No room matches \"%1\"").arg(wanted));
    rooms_empty_->set_hint(
        QStringLiteral("If it is a code somebody sent you, press Join to walk in."));
  }
  room_scroll_->setVisible(shown > 0);
  rooms_empty_->setVisible(shown == 0);
}

void MainWindow::join_room(const QString& id) {
  {
    // Quietly: the box is about to be left behind with the page, and
    // narrowing the cards to this one room on the way out is a flicker.
    const QSignalBlocker quiet(room_id_);
    room_id_->setText(id);
  }
  on_join_room();
}

void MainWindow::build_room_page() {
  auto* page = new QWidget(pages_);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);

  // The header and the controls are widgets and not bare layouts, because
  // full screen has to hide them and a layout cannot be hidden. See
  // enter_fullscreen.
  room_header_ = new AppHeader(page);

  // The only way to read the room code back out from inside a call, and it is
  // a button: inviting somebody is exactly the thing you do from in here, and
  // a label copies nothing.
  auto* copy = new SecondaryButton(QString{}, page);
  copy->set_leading_icon(Icon::Copy);
  copy->setProperty("code", true);
  copy->setMinimumHeight(kCodeChipHeight);
  copy->setToolTip(QStringLiteral("Copy the room code to the clipboard"));
  copy_room_button_ = copy;
  connect(copy_room_button_, &QPushButton::clicked, this, &MainWindow::on_copy_room_id);

  room_people_label_ = new QLabel(QString{}, page);
  theme::set_role(room_people_label_, theme::TextRole::Caption);

  sharing_label_ = new QLabel(QString{}, page);
  sharing_label_->setProperty("chip", QStringLiteral("accent"));
  sharing_label_->setVisible(false);

  // Each at its own height in the middle of the bar. A layout stretches what
  // it is given to the height of the row unless told otherwise, and a chip
  // sixty pixels tall is a banner.
  room_header_->beside_title()->addWidget(copy_room_button_, 0, Qt::AlignVCenter);
  room_header_->beside_title()->addWidget(room_people_label_, 0, Qt::AlignVCenter);
  room_header_->actions()->addWidget(sharing_label_, 0, Qt::AlignVCenter);

  // The picture, and over it the controls. The stage has no layout for the
  // controls: they float, placed by place_dock, so that the picture gets the
  // whole of the stage rather than the stage minus a strip.
  room_stage_ = new QWidget(page);
  auto* stage_layout = new QVBoxLayout(room_stage_);
  stage_layout->setContentsMargins(0, 0, 0, 0);
  screen_view_ = new ScreenView(room_stage_);
  stage_layout->addWidget(screen_view_);

  auto* dock = new FloatingBar(room_stage_);
  room_controls_ = dock;
  const auto dock_button = [dock](Icon icon) {
    auto* button = new IconButton(icon, dock);
    button->set_variant(IconButton::Variant::Tonal);
    button->set_extent(kDockButton);
    button->set_corner_radius(kDockButtonRadius);
    // The focus by Tab and not by a click. The controls stay up for as long
    // as the keyboard is among them - see the idle timer below - and a click
    // that left the focus on the button it pressed made that for ever: press
    // Mute once while watching a film and the controls never left again.
    // Somebody who tabs to them still holds them there, which is the case
    // the rule is for.
    button->setFocusPolicy(Qt::TabFocus);
    return button;
  };

  // What each toggle looks like when it is on. Sharing is the good state and
  // wears the accent; a muted microphone is a warning and wears amber, so the
  // two are told apart at a glance and not by reading a label.
  mute_button_ = dock_button(Icon::Microphone);
  mute_button_->setCheckable(true);
  mute_button_->set_checked_icon(Icon::MicrophoneOff);
  mute_button_->set_checked_tone(IconButton::CheckedTone::Warn);

  share_button_ = dock_button(Icon::Screen);
  share_button_->setCheckable(true);
  share_button_->set_label(QStringLiteral("Share screen"));

  // Beside the share button, because it is about the same picture. Shown only
  // while somebody else is sharing: see refresh_controls.
  fullscreen_button_ = dock_button(Icon::Fullscreen);
  fullscreen_button_->setToolTip(
      QStringLiteral("Watch the shared screen full screen. Esc, F11 or a double click on the "
                     "picture brings the room back."));
  fullscreen_button_->setAccessibleName(QStringLiteral("Full screen"));
  fullscreen_button_->setVisible(false);

  network_status_button_ = dock_button(Icon::Signal);
  network_status_button_->setToolTip(
      QStringLiteral("Network status: what the call is carrying, the three measurements behind "
                     "the network indicator in the status bar, and how much audio had to be "
                     "concealed."));
  network_status_button_->setAccessibleName(QStringLiteral("Network status"));

  // Apart from the rest, behind a rule, and the only one in red: it is the
  // one control here that ends something.
  auto* rule = new QFrame(dock);
  rule->setObjectName(QStringLiteral("rule"));
  rule->setFixedSize(1, kDockButton - theme::space::kLg);

  leave_button_ = dock_button(Icon::Leave);
  leave_button_->set_variant(IconButton::Variant::Danger);
  leave_button_->set_label(QStringLiteral("Leave"));

  dock->row()->addWidget(mute_button_);
  dock->row()->addWidget(share_button_);
  dock->row()->addWidget(fullscreen_button_);
  dock->row()->addWidget(network_status_button_);
  dock->row()->addWidget(rule);
  dock->row()->addWidget(leave_button_);

  // The controls get out of the way of a picture somebody is watching: three
  // seconds without the pointer moving over the stage and they slide down out
  // of it, and the first movement brings them back. Only while there is a
  // picture - with nothing shared they have nothing to be in the way of.
  dock_idle_ = new QTimer(this);
  dock_idle_->setSingleShot(true);
  dock_idle_->setInterval(kDockIdleMs);
  connect(dock_idle_, &QTimer::timeout, this, [this] {
    // Not from under the pointer, and not from under the keyboard: a control
    // that leaves while it is being aimed at is a control that dodges.
    const QWidget* focused = QApplication::focusWidget();
    const bool in_use = room_controls_->underMouse() ||
                        (focused != nullptr && room_controls_->isAncestorOf(focused));
    if (in_use) {
      dock_idle_->start();
      return;
    }
    set_dock_shown(false);
  });
  dock_slide_ = new QPropertyAnimation(room_controls_, "pos", this);
  // The stage tells us when it is resized and when the pointer moves over it;
  // the picture has to be asked for moves made with no button down.
  screen_view_->setMouseTracking(true);
  room_stage_->installEventFilter(this);
  screen_view_->installEventFilter(this);
  room_controls_->installEventFilter(this);

  // The side panel: who is here and what is being said, on one card.
  auto* side = new Card(page);
  room_sidebar_ = side;
  side->body()->setContentsMargins(0, 0, 0, 0);
  side->body()->setSpacing(0);

  // Two tabs rather than two boxes one above the other. Stacked, the list of
  // people was capped at 180 pixels to leave the conversation any room, and
  // the conversation still got what was left; a tab each gives both the whole
  // height. What must not disappear behind a tab is who is talking - so the
  // faces stay above both, with a ring round whoever is speaking.
  side_tabs_ = new QTabBar(side);
  side_tabs_->setDrawBase(false);
  side_tabs_->setExpanding(true);
  side_tabs_->addTab(QStringLiteral("People"));
  side_tabs_->addTab(QStringLiteral("Chat"));
  auto* tabs_row = new QHBoxLayout();
  tabs_row->setContentsMargins(theme::space::kSm, theme::space::kSm, theme::space::kXs, 0);
  tabs_row->addWidget(side_tabs_);

  auto* faces = new QWidget(side);
  avatar_strip_ = new QHBoxLayout(faces);
  avatar_strip_->setContentsMargins(theme::space::kMd, theme::space::kSm, theme::space::kMd,
                                    theme::space::kXs);
  avatar_strip_->setSpacing(theme::space::kXs);
  avatar_strip_->addStretch();

  // Our own microphone, under the faces and on both tabs: "am I being heard"
  // is asked from the conversation as often as from the list.
  microphone_level_ = new QProgressBar(side);
  microphone_level_->setRange(0, kMeterSteps);
  microphone_level_->setTextVisible(false);
  microphone_level_->setFixedHeight(kMeterHeight);
  microphone_level_->setToolTip(QStringLiteral("Your microphone"));
  auto* meter_row = new QHBoxLayout();
  meter_row->setContentsMargins(theme::space::kMd, 0, theme::space::kMd, theme::space::kMd);
  meter_row->addWidget(microphone_level_);

  const auto make_rule = [side] {
    auto* line = new QFrame(side);
    line->setObjectName(QStringLiteral("rule"));
    line->setFixedHeight(1);
    return line;
  };

  auto* people = new QWidget(side);
  auto* people_column = new QVBoxLayout(people);
  people_column->setContentsMargins(theme::space::kSm, theme::space::kSm, theme::space::kSm,
                                    theme::space::kMd);
  people_column->setSpacing(theme::space::kSm);
  participants_ = new QListWidget(people);
  participants_->setProperty("flat", true);
  participants_->setMinimumHeight(120);
  // The name and, beside it, an icon for each thing known about them:
  // microphone, muted, sharing. Words used to follow the name and read like a
  // log line; the icons say the same at a glance and leave the words to the
  // tooltip. See ui/participant_delegate.hpp.
  participants_->setItemDelegate(new ParticipantDelegate(participants_));
  // The menu is built on demand and is empty for anybody who is not an
  // administrator, so there is nothing to enable or disable here.
  participants_->setContextMenuPolicy(Qt::CustomContextMenu);

  // Stacked and not side by side. The label is a sentence and the panel is
  // 260 pixels wide at its narrowest, so sharing a row left the slider about
  // ninety of them: a control too small to aim at, beside a sentence too long
  // to finish.
  auto* volume_column = new QVBoxLayout();
  volume_column->setContentsMargins(theme::space::kXs, 0, theme::space::kXs, 0);
  volume_column->setSpacing(theme::space::kXs);
  volume_label_ = new QLabel(QStringLiteral("Volume: select a participant"), people);
  volume_label_->setProperty("hint", true);
  volume_ = new QSlider(Qt::Horizontal, people);
  // 0 to 200 percent: above 100 is amplification, which WebRTC allows.
  volume_->setRange(0, 200);
  volume_->setValue(100);
  volume_->setEnabled(false);
  volume_column->addWidget(volume_label_);
  volume_column->addWidget(volume_);

  people_column->addWidget(participants_, 1);
  people_column->addLayout(volume_column);

  auto* chat = new QWidget(side);
  auto* chat_column = new QVBoxLayout(chat);
  chat_column->setContentsMargins(0, 0, 0, 0);
  chat_column->setSpacing(0);
  chat_view_ = new ChatView(chat);
  chat_view_->setProperty("flat", true);
  // A message is a paragraph rather than a cell, and a long one has to be
  // readable without a horizontal scrollbar under it. The wrapping itself is
  // ChatView's delegate, not setWordWrap: a view that draws its own rows does
  // not consult either that or setTextElideMode. This is the guard against a
  // row that asks for more width than there is.
  chat_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Selectable, because copying what somebody pasted into the room is half of
  // what a chat in a screen sharing call is for.
  chat_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);

  auto* chat_row = new QHBoxLayout();
  chat_row->setContentsMargins(theme::space::kSm, theme::space::kSm, theme::space::kSm,
                               theme::space::kSm);
  chat_row->setSpacing(theme::space::kXs);
  chat_input_ = new QLineEdit(chat);
  chat_input_->setPlaceholderText(QStringLiteral("Message"));
  chat_input_->setMinimumHeight(kChatRowHeight);
  // A coarse guard and not the rule. The limit is bytes, and this counts
  // characters, so a line of accented text can still be refused by the server
  // at a length this field allowed. It is here to stop a paste of a whole
  // document dead rather than to decide anything: send_chat checks the real
  // limit, and what it answers reaches the same place every other error does.
  chat_input_->setMaxLength(static_cast<int>(models::kMaxChatTextBytes));

  // Opens a short curated grid. The system picker, which every platform has,
  // covers everything this one leaves out and works in the same field.
  auto* emoji = new IconButton(Icon::Smile, chat);
  emoji->set_extent(kChatRowHeight);
  emoji->setToolTip(QStringLiteral("Insert an emoji"));
  emoji->setAccessibleName(QStringLiteral("Insert an emoji"));
  chat_emoji_ = emoji;

  auto* send = new IconButton(Icon::Send, chat);
  send->set_variant(IconButton::Variant::Accent);
  send->set_extent(kChatRowHeight);
  send->setToolTip(QStringLiteral("Send"));
  send->setAccessibleName(QStringLiteral("Send"));
  chat_send_ = send;

  chat_row->addWidget(chat_input_, 1);
  chat_row->addWidget(chat_emoji_);
  chat_row->addWidget(chat_send_);

  chat_column->addWidget(chat_view_, 1);
  chat_column->addWidget(make_rule());
  chat_column->addLayout(chat_row);

  side_pages_ = new QStackedWidget(side);
  side_pages_->addWidget(people);
  side_pages_->addWidget(chat);

  side->body()->addLayout(tabs_row);
  side->body()->addWidget(faces);
  side->body()->addLayout(meter_row);
  side->body()->addWidget(make_rule());
  side->body()->addWidget(side_pages_, 1);

  // The conversation is what the panel opens on: the faces above it already
  // say who is here, and the list is one click away.
  side_tabs_->setCurrentIndex(kChatTab);
  side_pages_->setCurrentIndex(kChatTab);
  connect(side_tabs_, &QTabBar::currentChanged, this, [this](int tab) {
    side_pages_->setCurrentIndex(tab);
    if (tab == kChatTab) {
      // Read, as far as this window can tell.
      unread_chat_ = 0;
      refresh_side_tabs();
    }
  });

  // A splitter and not a fixed division, because how much room the conversation
  // deserves is not something this code knows. Somebody reading a long paste
  // wants it wide; somebody watching a screen share wants it out of the way.
  auto* body = new QSplitter(Qt::Horizontal, page);
  body->addWidget(room_stage_);
  body->addWidget(side);
  // Kept, because full screen hides the panel and puts it back, and puts the
  // handle back where the person had dragged it.
  room_body_ = body;

  // Neither side may be dragged out of existence. A collapsed pane leaves a
  // handle at the very edge of the window as the only way back, which is a
  // thing to discover rather than a thing to see, and collapsing the panel
  // takes the participants and the chat with it.
  body->setChildrenCollapsible(false);
  room_stage_->setMinimumWidth(320);
  side->setMinimumWidth(kSidePanelMinWidth);

  // Growing the window widens the screen, not the panel: the panel holds
  // names and lines of text, and neither reads better for being stretched.
  // Dragging still overrides this, which is the point of the splitter.
  body->setStretchFactor(0, 1);
  body->setStretchFactor(1, 0);
  body->setSizes({720, 300});

  // The margin round the picture and the panel lives on a layout of its own,
  // because full screen takes it away and puts it back. See enter_fullscreen.
  auto* body_holder = new QWidget(page);
  room_column_ = new QVBoxLayout(body_holder);
  room_column_->setContentsMargins(theme::space::kMd, theme::space::kMd, theme::space::kMd,
                                   theme::space::kXs);
  room_column_->addWidget(body);

  column->addWidget(room_header_);
  column->addWidget(body_holder, 1);

  connect(participants_, &QListWidget::customContextMenuRequested, this,
          &MainWindow::on_participant_menu);
  connect(chat_send_, &QAbstractButton::clicked, this, &MainWindow::on_send_chat);
  connect(chat_emoji_, &QAbstractButton::clicked, this, &MainWindow::on_open_emoji_picker);
  // Return sends, which is what everybody expects of a field next to a Send
  // button and what nobody wants to reach for the mouse for.
  connect(chat_input_, &QLineEdit::returnPressed, this, &MainWindow::on_send_chat);
  // `toggled` and not `clicked`, for the two checkable buttons only.
  //
  // Qt gives a checkable button the accessibility role of a check box, and a
  // check box answers the platform's press action with
  // QAbstractButton::toggle(). That sets the checked state and emits
  // `toggled`; it never emits `clicked`. So a person driving this window with
  // VoiceOver could press "Share screen", watch it light up, and share
  // nothing - the button's appearance said one thing and the call another.
  //
  // The cost of listening to `toggled` is that refresh_controls() also sets
  // these states, and that would come straight back here as a fresh command.
  // The two setChecked calls there are blocked for exactly that reason.
  connect(mute_button_, &QAbstractButton::toggled, this, &MainWindow::on_toggle_mute);
  connect(share_button_, &QAbstractButton::toggled, this, &MainWindow::on_toggle_share);
  connect(fullscreen_button_, &QAbstractButton::clicked, this, &MainWindow::on_toggle_fullscreen);
  // A double click on the picture, which is what every video player has
  // taught a hand to do. Both ways: in full screen there is nothing else on
  // the screen to click.
  connect(screen_view_, &ScreenView::activated, this, &MainWindow::on_toggle_fullscreen);

  // Keys, on the window rather than on a widget, because in full screen the
  // widgets that could hold them are hidden. F11 is what every browser uses;
  // Esc is what every full screen anything uses. The Esc action is enabled
  // only while full screen, so that it never takes the key from a dialog or
  // from the filter line of the administration panel, which has its own use
  // for it.
  fullscreen_action_ = new QAction(QStringLiteral("Full screen"), this);
  fullscreen_action_->setShortcut(QKeySequence(Qt::Key_F11));
  fullscreen_action_->setShortcutContext(Qt::WindowShortcut);
  connect(fullscreen_action_, &QAction::triggered, this, &MainWindow::on_toggle_fullscreen);
  addAction(fullscreen_action_);
  leave_fullscreen_action_ = new QAction(QStringLiteral("Leave full screen"), this);
  leave_fullscreen_action_->setShortcut(QKeySequence(Qt::Key_Escape));
  leave_fullscreen_action_->setShortcutContext(Qt::WindowShortcut);
  leave_fullscreen_action_->setEnabled(false);
  connect(leave_fullscreen_action_, &QAction::triggered, this, &MainWindow::leave_fullscreen);
  addAction(leave_fullscreen_action_);

  // Runs only while a nudge is shaking the window, for the reason the level
  // timer runs only while the meter is moving.
  shake_timer_ = new QTimer(this);
  shake_timer_->setInterval(kShakeFrameMs);
  connect(shake_timer_, &QTimer::timeout, this, &MainWindow::animate_shake);
  connect(network_status_button_, &QAbstractButton::clicked, this, &MainWindow::on_open_metrics);
  connect(leave_button_, &QAbstractButton::clicked, this, &MainWindow::on_leave_room);
  connect(participants_, &QListWidget::itemSelectionChanged, this,
          &MainWindow::on_participant_selected);
  connect(volume_, &QSlider::valueChanged, this, &MainWindow::on_volume_changed);

  pages_->insertWidget(kRoomPage, page);
}

void MainWindow::place_dock(bool animated) {
  if (room_controls_ == nullptr || room_stage_ == nullptr) {
    return;
  }
  // Its own size first: a label changing - "Share screen" to "Stop sharing" -
  // or a button appearing changes how wide the bar is.
  room_controls_->adjustSize();
  const int left = (room_stage_->width() - room_controls_->width()) / 2;
  // Resting a little above the bottom edge, or just below it and out of
  // sight. The stage clips its children, so "below" is simply not drawn.
  const int rest = room_stage_->height() - room_controls_->height() - kDockLift;
  const QPoint target(left, dock_shown_ ? rest : room_stage_->height());

  dock_slide_->stop();
  const int duration = animated ? theme::motion::duration(theme::motion::kBaseMs) : 0;
  if (duration == 0 || !room_controls_->isVisible()) {
    room_controls_->move(target);
    return;
  }
  dock_slide_->setDuration(duration);
  // Arriving slows down and leaving speeds up: the two read as one movement
  // run in both directions.
  dock_slide_->setEasingCurve(dock_shown_ ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
  dock_slide_->setStartValue(room_controls_->pos());
  dock_slide_->setEndValue(target);
  dock_slide_->start();
}

void MainWindow::set_dock_shown(bool shown) {
  if (dock_shown_ == shown) {
    return;
  }
  dock_shown_ = shown;
  room_controls_->raise();
  place_dock(true);
}

void MainWindow::wake_dock() {
  set_dock_shown(true);
  // Only a picture is worth hiding the controls for. With nothing shared the
  // stage is a sentence on a dark card, and controls that left it would be
  // controls somebody had to go looking for with nothing gained.
  if (watching_share()) {
    dock_idle_->start();
  } else {
    dock_idle_->stop();
  }
}

bool MainWindow::watching_share() const {
  const std::string sharer = session_.screen_sharer();
  return state_ == client::app::CallSession::State::InCall && !sharer.empty() &&
         sharer != session_.local_user().id;
}

void MainWindow::notify(ToastKind kind, const QString& text) {
  // Over a picture only when the picture is what is on screen. A share goes
  // on behind the administration panel too, and a notification raised there
  // lands on a page of tables, where waiting to be dismissed costs nothing.
  const bool over_picture = watching_share() && pages_->currentIndex() == kRoomPage;
  toasts_->show_toast(kind, text, over_picture);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == room_stage_ && event->type() == QEvent::Resize) {
    place_dock(false);
  } else if (watched == room_stage_ || watched == screen_view_ || watched == room_controls_) {
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseMove || type == QEvent::Enter || type == QEvent::MouseButtonPress) {
      wake_dock();
    }
  } else if (room_scroll_ != nullptr && watched == room_scroll_->viewport() &&
             event->type() == QEvent::Resize) {
    // Laid out again only when the width has changed by a whole column:
    // every pixel of a drag would otherwise rebuild every card.
    const int spacing = room_grid_->spacing();
    const int columns =
        std::clamp((room_scroll_->viewport()->width() + spacing) / (kRoomCardMinWidth + spacing), 1,
                   kMaxRoomColumns);
    if (columns != room_columns_) {
      rebuild_room_cards();
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::refresh_side_tabs() {
  // Written only when it differs from what the tab already says. This is
  // called for every list of participants, which arrives several times a
  // second with the levels in it, and QTabBar lays itself out and repaints
  // for every setTabText - the same text included.
  const auto write = [this](int tab, const QString& text) {
    if (side_tabs_->tabText(tab) != text) {
      side_tabs_->setTabText(tab, text);
    }
  };
  const int people = participants_->count();
  write(kPeopleTab,
        people > 0 ? QStringLiteral("People · %1").arg(people) : QStringLiteral("People"));
  // How many lines arrived while the list was what was being looked at. A
  // number and not a dot: the tab is text, and "Chat · 3" needs no legend.
  write(kChatTab,
        unread_chat_ > 0 ? QStringLiteral("Chat · %1").arg(unread_chat_) : QStringLiteral("Chat"));
}

void MainWindow::on_account_menu() {
  const models::User user = session_.local_user();
  const bool in_call = state_ == client::app::CallSession::State::InCall;

  QMenu menu(this);
  menu.setToolTipsVisible(true);
  // Who this is, first and not clickable: the badge on the rail is one
  // letter, and this is where the rest of the name is.
  QAction* who = menu.addAction(
      QString::fromStdString(user.display_name.empty() ? user.id : user.display_name));
  who->setEnabled(false);
  menu.addSeparator();

  // Both end the session - a changed password revokes it - and doing that
  // from inside a call would drop the call as a side effect of a
  // settings-shaped action. Leaving the room first is one click, and it is
  // the click that says the call is meant to end.
  QAction* change = menu.addAction(QStringLiteral("Change password"));
  QAction* sign_out = menu.addAction(QStringLiteral("Sign out"));
  for (QAction* action : {change, sign_out}) {
    action->setEnabled(!in_call);
    action->setToolTip(in_call ? QStringLiteral("Leave the room first.") : QString());
  }
  connect(change, &QAction::triggered, this, &MainWindow::on_change_password);
  connect(sign_out, &QAction::triggered, this, &MainWindow::on_sign_out);

  // To the right of the badge and growing upwards from its foot: the badge
  // is in the bottom left corner of the window, and a menu opened at its own
  // corner would cover it.
  const QPoint corner = account_avatar_->mapToGlobal(
      QPoint(account_avatar_->width() + theme::space::kSm, account_avatar_->height()));
  menu.exec(QPoint(corner.x(), corner.y() - menu.sizeHint().height()));
}

void MainWindow::wire_session() {
  // Each callback lands on a networking or media thread. Nothing here touches
  // a widget: it packages the news and posts it to the UI thread.
  session_.on_events({
      .on_state =
          [this](client::app::CallSession::State state, const std::string& detail) {
            QMetaObject::invokeMethod(this, "apply_state", Qt::QueuedConnection,
                                      Q_ARG(int, static_cast<int>(state)),
                                      Q_ARG(QString, QString::fromStdString(detail)));
          },
      .on_link =
          [this](client::app::CallSession::LinkStats link) {
            // Minus one for "no measurement", because a queued invocation
            // carries plain types and an optional is not one of them. Zero is
            // not free for that job: a round trip really can measure zero
            // milliseconds against a server on this machine.
            //
            // Numbered here, on the thread that made the report, and not in
            // apply_link: the number has to say when the report was made,
            // and the queue between the two is exactly what it is for.
            QMetaObject::invokeMethod(
                this, "apply_link", Qt::QueuedConnection,
                Q_ARG(int, link.round_trip ? static_cast<int>(link.round_trip->count()) : -1),
                Q_ARG(bool, link.connected), Q_ARG(bool, link.attempting),
                Q_ARG(int, ++link_sequence_));
          },
      .on_participants =
          [this](const std::vector<client::app::Participant>& list) {
            QStringList names;
            names.reserve(static_cast<qsizetype>(list.size()));
            for (const client::app::Participant& participant : list) {
              // Four tab separated fields: the id, the name, the state as
              // bits, and the level as a fraction. The name is what the row
              // shows and what the volume slider says; the state and the
              // level are what the delegate draws beside it.
              //
              // The id goes first, and that order is the point rather than a
              // matter of taste. It used to sit in the middle, between two
              // fields built out of the display name, and a display name is
              // whatever another participant typed. A name containing a tab
              // pushed a field of its own into the row and moved the id along
              // by one, so "Alice<tab><somebody else's id><tab>Alice" produced
              // a row that read Alice and carried a stranger's identifier -
              // which on_participant_menu then kicked, muted or restricted.
              //
              // First, the id cannot be moved by anything that follows it: it
              // is server generated hex with no tab in it, so it ends where the
              // first tab is and a name can only ever garble itself. The server
              // refuses control characters in a display name as well, in
              // models::is_valid_display_name, but this side does not depend on
              // that being true.
              const QString name =
                  as_field(participant.user.display_name.empty() ? participant.user.id
                                                                 : participant.user.display_name);
              int state = kParticipantNone;
              if (participant.muted) {
                state |= kParticipantMuted;
              }
              if (participant.speaking) {
                state |= kParticipantSpeaking;
              }
              if (participant.audio_active) {
                state |= kParticipantAudioActive;
              }
              if (participant.sharing_screen) {
                state |= kParticipantSharing;
                if (participant.sharing_audio) {
                  state |= kParticipantSharingWithSound;
                }
              }
              // The same curve the local meter uses, so that the microphone
              // beside a name fills the way the bar under our own does.
              const double level = client::app::meter_fraction(participant.level);
              names.push_back(QString::fromStdString(participant.user.id) + QLatin1Char('\t') +
                              name + QLatin1Char('\t') + QString::number(state) +
                              QLatin1Char('\t') + QString::number(level, 'f', 3));
            }
            QMetaObject::invokeMethod(this, "apply_participants", Qt::QueuedConnection,
                                      Q_ARG(QStringList, names));
          },
      .on_metrics =
          [this](client::media::AudioStats stats) {
            // What the last interval sounded like, next to what it lost: the
            // share of audio the jitter buffer had to invent since the previous
            // report. Loss counts the packet whether or not RED or a
            // retransmission made it good; this is what got through to the ear.
            double concealed_percent = 0.0;
            if (stats.concealed_samples >= status_concealed_samples_ &&
                stats.total_samples_received > status_total_samples_) {
              concealed_percent =
                  100.0 * static_cast<double>(stats.concealed_samples - status_concealed_samples_) /
                  static_cast<double>(stats.total_samples_received - status_total_samples_);
            }
            status_concealed_samples_ = stats.concealed_samples;
            status_total_samples_ = stats.total_samples_received;

            // One "kbps" for both directions. The line shares the bar with
            // the verdict on its left and the version on its right, and a
            // share with sound adds two more segments to it; the units it
            // does not repeat are what keep the last of them on screen.
            QString summary = QStringLiteral(
                                  "rtt %1 ms · jitter %2 ms · lost %3 · concealed %4% · "
                                  "↑ %5 ↓ %6 kbps")
                                  .arg(stats.round_trip_time_ms, 0, 'f', 0)
                                  .arg(stats.jitter_ms, 0, 'f', 1)
                                  .arg(stats.packets_lost)
                                  .arg(concealed_percent, 0, 'f', 1)
                                  .arg(stats.send_bitrate_kbps, 0, 'f', 0)
                                  .arg(stats.receive_bitrate_kbps, 0, 'f', 0);

            // The screen share is the part of the call that gives way when the
            // network does, so while there is one its rate is worth seeing:
            // a picture that went soft and a number that fell are the same
            // event, and only one of them is legible.
            const client::media::VideoStats video = session_.video_stats();
            if (video.frames_sent > 0) {
              summary +=
                  QStringLiteral(" · screen ↑ %1 kbps").arg(video.send_bitrate_kbps, 0, 'f', 0);
            } else if (video.frames_received > 0) {
              summary +=
                  QStringLiteral(" · screen ↓ %1 kbps").arg(video.receive_bitrate_kbps, 0, 'f', 0);
            }

            // While the share carries sound, the audio number above stops being
            // about a voice: it is a voice and a film together, in stereo, and
            // it climbs from tens of kbps to near a hundred. Saying so is what
            // keeps somebody from reading their own screen share as a fault.
            //
            // How much of it was silence is the other half. A capture that is
            // delivering nothing but silence looks exactly like a working one
            // from the outside, and this is the only place that difference
            // shows.
            if (stats.screen_audio_active) {
              const auto blocks = stats.screen_audio_blocks;
              const int quiet =
                  blocks == 0 ? 0
                              : static_cast<int>((stats.screen_audio_silent_blocks * 100) / blocks);
              summary += QStringLiteral(" · sound shared (%1% silent)").arg(quiet);
            }
            QMetaObject::invokeMethod(this, "apply_metrics", Qt::QueuedConnection,
                                      Q_ARG(QString, summary),
                                      Q_ARG(int, static_cast<int>(client::app::quality_of(stats))));
          },
      .on_local_level =
          [this](double level, bool speaking) {
            QMetaObject::invokeMethod(this, "apply_local_level", Qt::QueuedConnection,
                                      Q_ARG(double, level), Q_ARG(bool, speaking));
          },
      .on_error =
          [this](const Error& error) {
            QMetaObject::invokeMethod(this, "apply_error", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(error.code)),
                                      Q_ARG(QString, QString::fromStdString(error.message)));
          },
      .on_remote_video =
          [this](client::video::VideoFrame frame) {
            // Handed straight to the widget, which is what decides when the
            // interface thread comes to collect it.
            screen_view_->submit(frame);
          },
      .on_screen_share =
          [this](const std::string& user_id) {
            QMetaObject::invokeMethod(this, "apply_screen_share", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(user_id)));
          },
      .on_chat_message =
          [this](const models::ChatMessage& message) {
            QMetaObject::invokeMethod(this, "apply_chat_message", Qt::QueuedConnection,
                                      Q_ARG(QString, to_chat_line(message)));
          },
      .on_chat_history =
          [this](const std::vector<models::ChatMessage>& messages) {
            QStringList lines;
            lines.reserve(static_cast<qsizetype>(messages.size()));
            for (const models::ChatMessage& message : messages) {
              lines.push_back(to_chat_line(message));
            }
            QMetaObject::invokeMethod(this, "apply_chat_history", Qt::QueuedConnection,
                                      Q_ARG(QStringList, lines));
          },
      .on_nudge =
          [this](const std::string& from_user_id, const std::string& to_user_id) {
            QMetaObject::invokeMethod(this, "apply_nudge", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(from_user_id)),
                                      Q_ARG(QString, QString::fromStdString(to_user_id)));
          },
      .on_password_changed =
          [this] {
            QMetaObject::invokeMethod(this, "apply_password_changed", Qt::QueuedConnection);
          },
      .on_session_ended =
          [this](const std::string& reason) {
            QMetaObject::invokeMethod(this, "apply_session_ended", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(reason)));
          },
      .on_notice =
          [this](const models::Notice& notice) {
            // The name the administrator held when they wrote it, which is
            // what travelled with the notice: this may be read a week later,
            // by which time the account behind it may have been renamed or
            // removed, and "an administrator" is the right thing to say then
            // rather than an identifier.
            const QString from = notice.from_display_name.empty()
                                     ? QStringLiteral("an administrator")
                                     : QString::fromStdString(notice.from_display_name);
            QMetaObject::invokeMethod(this, "apply_notice", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(notice.id)),
                                      Q_ARG(QString, from),
                                      Q_ARG(QString, QString::fromStdString(notice.text)));
          },
      .on_notice_sent =
          [this](const models::Notice& notice) {
            // The identifier and not a name, because turning one into the
            // other means reading the account list the panel is holding, and
            // this runs on the signaling thread. The slot at the other end
            // runs on the interface thread, where that list is safe to touch.
            QMetaObject::invokeMethod(this, "apply_notice_sent", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(notice.user_id)));
          },
      .on_user_list =
          [this](const std::vector<protocol::UserSummary>& users) {
            QStringList rows;
            rows.reserve(static_cast<qsizetype>(users.size()));
            for (const protocol::UserSummary& summary : users) {
              // Identifier first, then one field per column. Tab separated for
              // the same reason the participant list is: it needs no
              // registered metatype to cross to the UI thread.
              const models::Restrictions& taken = summary.user.restrictions;
              QStringList fields;
              fields << as_field(summary.user.id) << as_field(summary.username)
                     << as_field(summary.user.display_name)
                     << from_utf8(models::to_string(summary.user.role))
                     << (summary.created_at > 0 ? QDateTime::fromSecsSinceEpoch(summary.created_at)
                                                      .toString(QStringLiteral("yyyy-MM-dd"))
                                                : QString())
                     << (summary.online ? QStringLiteral("yes") : QString())
                     // Empty for an account with nothing taken away, which is
                     // most of them, so the column reads as an exception list
                     // rather than as a column of "none".
                     << as_field(models::describe(taken))
                     // The same four flags once more, past the last column the
                     // table draws, as one character each in the order
                     // Restrictions declares them: banned, muted, silenced,
                     // screen_share_blocked.
                     //
                     // A second copy rather than the panel reading the column
                     // above, because that column is describe(), whose own
                     // documentation calls it the display form. Reading a
                     // rendering back as though it were protocol is how the
                     // restrictions dialog came to open with every box
                     // unchecked when the format moved under it - and it sends
                     // all four boxes, so opening wrong means lifting three
                     // restrictions nobody asked to lift. This field has one
                     // reader and no other job, so it cannot drift.
                     << QStringLiteral("%1%2%3%4")
                            .arg(taken.banned ? 1 : 0)
                            .arg(taken.muted ? 1 : 0)
                            .arg(taken.silenced ? 1 : 0)
                            .arg(taken.screen_share_blocked ? 1 : 0);
              rows.push_back(fields.join(QLatin1Char('\t')));
            }
            QMetaObject::invokeMethod(admin_panel_, "apply_users", Qt::QueuedConnection,
                                      Q_ARG(QStringList, rows));
          },
      .on_room_list =
          [this](const std::vector<protocol::RoomSummary>& rooms) {
            QStringList rows;
            rows.reserve(static_cast<qsizetype>(rooms.size()));
            for (const protocol::RoomSummary& summary : rooms) {
              QStringList fields;
              // Identifier first and unshown, then the columns in the order
              // both tables declare them. See ui::fill.
              // "3/10" rather than "3": how many are in says nothing about
              // whether there is room for one more, and that is the question
              // somebody reading this column is asking. A server from before
              // rooms had sizes sends none, and the column is a bare count
              // again rather than "3/0".
              const QString people =
                  summary.capacity > 0
                      ? QStringLiteral("%1/%2").arg(summary.participant_count).arg(summary.capacity)
                      : QString::number(summary.participant_count);
              fields << as_field(summary.id) << as_field(summary.id) << as_field(summary.name)
                     << people
                     // Two more than either table has columns, and ui::fill
                     // reads only as many fields as there are columns. They
                     // are for the administrator's resize dialog, which needs
                     // the two numbers and should not have to read them back
                     // out of "3/10".
                     << QString::number(summary.capacity)
                     << QString::number(summary.participant_count);
              rows.push_back(fields.join(QLatin1Char('\t')));
            }
            // Both screens, from the one answer. The administrator's tab is
            // where rooms are closed and the home page is where they are
            // walked into, and neither of them is the owner of the list.
            QMetaObject::invokeMethod(admin_panel_, "apply_rooms", Qt::QueuedConnection,
                                      Q_ARG(QStringList, rows));
            // Worked out here rather than in the window, because this is where
            // the summaries still carry who owns what: the rows are flattened
            // to what the table shows.
            const models::User me = session_.local_user();
            const bool may_create =
                me.role == models::Role::Admin ||
                std::none_of(rooms.begin(), rooms.end(),
                             [&](const protocol::RoomSummary& s) { return s.owner_id == me.id; });
            QMetaObject::invokeMethod(this, "apply_room_list", Qt::QueuedConnection,
                                      Q_ARG(QStringList, rows), Q_ARG(bool, may_create));
          },
      .on_audit_list =
          [this](const std::vector<models::AuditEntry>& entries) {
            QStringList rows;
            rows.reserve(static_cast<qsizetype>(entries.size()));
            for (const models::AuditEntry& entry : entries) {
              QStringList fields;
              fields << as_field(entry.id)
                     << QDateTime::fromSecsSinceEpoch(entry.timestamp_seconds)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                     << as_field(entry.actor_username) << as_field(entry.action)
                     << as_field(entry.target_id)
                     << as_field(entry.room_id)
                     // The one that carries a name most of the time: an audit
                     // detail is built from models::user_label, so it holds
                     // whatever the account it names calls itself.
                     << as_field(entry.detail);
              rows.push_back(fields.join(QLatin1Char('\t')));
            }
            QMetaObject::invokeMethod(admin_panel_, "apply_audit", Qt::QueuedConnection,
                                      Q_ARG(QStringList, rows));
          },
      .on_session_list =
          [this](const std::vector<protocol::SessionSummary>& sessions) {
            // The three times as the server wrote them, formatted here; the
            // account stays an identifier, because the panel holds the account
            // list and is where a name is looked up. Zero, which is a time the
            // server never wrote, travels as an empty field.
            const auto when = [](std::int64_t seconds) {
              return seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds).toString(
                                       QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                 : QString();
            };
            QStringList rows;
            rows.reserve(static_cast<qsizetype>(sessions.size()));
            for (const protocol::SessionSummary& summary : sessions) {
              QStringList fields;
              fields << as_field(summary.id) << as_field(summary.user_id) << as_field(summary.ip)
                     << when(summary.connected_at) << when(summary.last_seen_at)
                     << when(summary.ended_at);
              rows.push_back(fields.join(QLatin1Char('\t')));
            }
            QMetaObject::invokeMethod(admin_panel_, "apply_sessions", Qt::QueuedConnection,
                                      Q_ARG(QStringList, rows));
          },
      .on_kicked =
          [this](const std::string& reason) {
            QMetaObject::invokeMethod(this, "apply_kicked", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(reason)));
          },
      .on_forced_mute =
          [this](const std::string& user_id, const std::string& by_user_id, bool muted) {
            // Resolved to names here, on the session's own snapshot, because
            // the participant list on screen is rebuilt from scratch and may
            // not have caught up yet.
            QString name;
            QString by_name;
            for (const client::app::Participant& participant : session_.participants()) {
              if (participant.user.id == user_id) {
                name = QString::fromStdString(participant.user.display_name);
              }
              if (participant.user.id == by_user_id) {
                by_name = QString::fromStdString(participant.user.display_name);
              }
            }
            // Whether the administrator is the person at this window. By
            // identifier and not by the name resolved above: two accounts are
            // free to carry the same display name.
            const bool by_us = by_user_id == session_.local_user().id;
            QMetaObject::invokeMethod(this, "apply_forced_mute", Qt::QueuedConnection,
                                      Q_ARG(QString, name), Q_ARG(QString, by_name),
                                      Q_ARG(bool, muted), Q_ARG(bool, by_us));
          },
      .on_restrictions_changed =
          [this](const std::string& user_id, const models::Restrictions& restrictions,
                 const std::string& by_user_id, const std::string& reason) {
            // Resolved to names here for the same reason the forced mute is,
            // and the whole line is built here as well: it is the one thing
            // the interface has left to do, and a slot taking four arguments
            // would need three of them registered as metatypes to cross
            // threads.
            QString name;
            QString by_name;
            for (const client::app::Participant& participant : session_.participants()) {
              if (participant.user.id == user_id) {
                name = QString::fromStdString(participant.user.display_name);
              }
              if (participant.user.id == by_user_id) {
                by_name = QString::fromStdString(participant.user.display_name);
              }
            }
            const bool is_us = user_id == session_.local_user().id;
            const bool by_us = by_user_id == session_.local_user().id;
            QMetaObject::invokeMethod(
                this, "apply_restrictions", Qt::QueuedConnection, Q_ARG(QString, name),
                Q_ARG(QString, by_name),
                Q_ARG(QString, QString::fromStdString(models::describe(restrictions))),
                Q_ARG(QString, QString::fromStdString(reason)), Q_ARG(bool, is_us),
                Q_ARG(bool, by_us));
          },
  });

  session_.on_room_created([this](const std::string& room_id) {
    QMetaObject::invokeMethod(this, "apply_room_created", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(room_id)));
  });
}

void MainWindow::on_connect() {
  show_login_error(QString{});

  // Top to bottom, the way the form reads: a server that cannot be right is
  // said before a username that is missing.
  const std::optional<std::string> url = server_address_from_form();
  if (!url) {
    return;
  }
  const QString user = username_->text().trimmed();
  if (user.isEmpty()) {
    show_login_error(QStringLiteral("Enter a username."));
    return;
  }

  // A different server than the socket has been knocking at. set_signaling_url
  // takes the probe's socket there, and connect_and_authenticate below finds
  // it on its way and hands the credentials over the moment it opens. The
  // same server is left alone: closing a socket that is up in order to open
  // the same one again is a reconnect nobody asked for.
  if (*url != session_.signaling_url()) {
    session_.set_signaling_url(*url);
  }
  // And a different one than the file says, whether or not it is the one in
  // use: the Test button can have moved the session there already, and a
  // sign-in is the moment the address stops being an experiment.
  if (*url != remembered_server_) {
    remember_server_address(*url);
  }

  if (const auto connected =
          session_.connect_and_authenticate(user.toStdString(), password_->text().toStdString());
      !connected) {
    show_login_error(describe(QString::fromStdString(connected.error().code),
                              QString::fromStdString(connected.error().message)));
  }
}

void MainWindow::on_test_server() {
  show_login_error(QString{});
  const std::optional<std::string> url = server_address_from_form();
  if (!url) {
    return;
  }

  // Armed before the socket is touched, so that no report about the socket
  // being closed can slip in between: a report numbered at or below this was
  // made before the test began, and apply_link ignores it for the test.
  server_under_test_ = QString::fromStdString(*url);
  server_test_since_ = link_sequence_.load();
  show_server_hint(QStringLiteral("Looking for %1...").arg(server_under_test_), false);

  if (*url != session_.signaling_url()) {
    // Knocks at the new address on its own; see CallSession::set_signaling_url.
    session_.set_signaling_url(*url);
    return;
  }
  if (const auto knocked = session_.retest_server(); !knocked) {
    server_under_test_.clear();
    show_server_hint(describe(QString::fromStdString(knocked.error().code),
                              QString::fromStdString(knocked.error().message)),
                     true);
  }
}

std::optional<std::string> MainWindow::server_address_from_form() {
  const auto url = client::app::expand_server_address(server_->text().toStdString());
  if (!url) {
    server_under_test_.clear();
    show_server_hint(QString::fromStdString(url.error().message), true);
    return std::nullopt;
  }
  // Put back as the URL displays, so that what is on screen is what was
  // used. A space on the end is invisible, and "192.168.1.10" on screen over
  // "ws://192.168.1.10:8080" in use is a port somebody has to know about.
  const QString shown = QString::fromStdString(client::app::display_server_address(url.value()));
  if (shown != server_->text()) {
    server_->setText(shown);
  }
  return url.value();
}

void MainWindow::show_server_hint(const QString& text, bool error) {
  server_hint_->setText(text);
  server_hint_->setVisible(!text.isEmpty());
  // Two properties and not one, because the theme styles them apart: "error"
  // is the colour of a refusal, "hint" the colour of a remark. Restyled on
  // every change, since a property set after the widget was polished is not
  // seen until it is polished again.
  server_hint_->setProperty("error", error);
  server_hint_->setProperty("hint", !error);
  server_hint_->style()->unpolish(server_hint_);
  server_hint_->style()->polish(server_hint_);
}

void MainWindow::remember_server_address(const std::string& url) {
  const std::filesystem::path file = config::user_config_file();
  if (file.empty()) {
    // Nowhere to keep it, which main() has already said at startup. The
    // address is in use; it is only the next start that will not have it.
    return;
  }
  const auto written = config::save_ini_settings(
      file, {config::IniSetting{.section = "network", .key = "signaling_url", .value = url}});
  if (!written) {
    DV_LOG_WARN("Could not save network.signaling_url to {}: {}", file.string(),
                written.error().message);
    return;
  }
  remembered_server_ = url;
  DV_LOG_INFO("Signaling server saved to {}: {}", file.string(), url);
}

void MainWindow::on_sign_out() {
  // Nothing to say: the person asked for this.
  return_to_login(QString{});
}

void MainWindow::return_to_login(const QString& text) {
  // disconnect() drops the identity along with the socket, and the state it
  // publishes is what carries the interface back to the login screen. Nothing
  // here changes the page by hand: two routes to the same page are two routes
  // that can disagree.
  //
  // For a session the server ended, the identity is already gone and the
  // socket is still up; dropping both again costs a reconnect and buys one
  // path. The server kept that socket open so that it could say why, and it
  // has said it by the time this runs.
  session_.disconnect();

  // Nor do the notifications of the session that just ended. Some of them
  // wait to be dismissed - an administrator's "restricted Carla" - and left
  // up they would be the first thing the next person at this machine reads,
  // over the sign-in card, about a room they were never in.
  toasts_->dismiss_all();

  // The password does not stay in the field for whoever sits down next. The
  // username does, because signing back in as yourself is the ordinary case
  // and a server address that has just been changed is the other one.
  password_->clear();
  show_login_error(text);

  // The server field says what the next sign-in will use. Since the row in
  // Settings went, this screen is the only one that moves the address, so the
  // field already says what the socket is on; it is put back from the session
  // anyway, so that the two cannot disagree by a route this file has not
  // thought of. The sentence under the field, if any, was about a test of the
  // old one.
  server_->setText(
      QString::fromStdString(client::app::display_server_address(session_.signaling_url())));
  server_under_test_.clear();
  show_server_hint(QString{}, false);

  // Back on the login screen, the indicator has the same question to answer
  // as when the window opened, and the socket that answered it has just been
  // closed. A new one, to whatever address is set now - which is how a
  // changed address takes effect, and why signing out is the way to it.
  link_round_trip_ms_ = -1;
  link_connected_ = -1;
  show_link_quality();
  (void)session_.probe_server();
}

void MainWindow::on_change_password() {
  PasswordDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  // Nothing is kept from the dialog. If the server refuses - the current
  // password was wrong - the answer arrives after this has returned, as an
  // error on this screen, and the form is opened again from the button rather
  // than reappearing half filled with a password that turned out to be wrong.
  const auto sent = session_.change_password(dialog.current_password().toStdString(),
                                             dialog.new_password().toStdString());
  if (!sent) {
    apply_error(QString::fromStdString(sent.error().code),
                QString::fromStdString(sent.error().message));
  }
}

void MainWindow::apply_password_changed() {
  // The same route out as the sign out button, deliberately: two ways to reach
  // the login screen are two ways that can disagree about what they left
  // behind. The sentence goes on the login screen, where they are about to
  // type it, rather than in a box they dismiss on the way there.
  //
  // This arrives right after the `session_ended` the same change produced, so
  // the screen is already up with that message's reason on it; this one
  // replaces it with the answer to the form they filled in.
  return_to_login(QStringLiteral("Password changed. Sign in with the new one."));
}

void MainWindow::apply_session_ended(const QString& reason) {
  // On the login screen and not in a box, for the reason the password message
  // is: it is the answer to the question they are about to ask of the form in
  // front of them. The reason is the server's, in the room's own words - "the
  // session was ended by an administrator", "the account was removed" - and
  // for a ban the box explaining the restriction has already been shown by
  // apply_restrictions, which the server sends first.
  return_to_login(
      reason.isEmpty()
          ? QStringLiteral("The server ended your session. Sign in again.")
          : QStringLiteral("The server ended your session: %1. Sign in again.").arg(reason));
}

void MainWindow::on_create_room() {
  // Left as the person typed it, accents and all, and trimmed by the server.
  // Trimming here too would only mean two places that have to agree on what
  // whitespace is.
  const std::string name = room_name_->text().toStdString();

  // The same predicate the server applies, so that a name too long is answered
  // here instead of after a round trip. It is still checked there: this window
  // is not the only thing that can send a create_room.
  if (!models::is_valid_room_name(name)) {
    apply_error(QStringLiteral("invalid_value"),
                QStringLiteral("A room name is at most %1 bytes and cannot contain tabs or line "
                               "breaks. Leave it empty to be given the room code.")
                    .arg(static_cast<int>(models::kMaxRoomNameBytes)));
    return;
  }

  if (const auto created = session_.create_room(name, false, room_capacity_->value()); !created) {
    apply_error(QString::fromStdString(created.error().code),
                QString::fromStdString(created.error().message));
  }
}

void MainWindow::on_join_room() {
  const QString room = room_id_->text().trimmed().toUpper();
  if (room.isEmpty()) {
    apply_error(QStringLiteral("invalid_value"), QStringLiteral("Enter the room ID."));
    return;
  }
  room_id_->setText(room);

  // The name the room shows is the one the account carries, not the login
  // typed into the form: a room full of lower case usernames is not what the
  // server was asked for.
  const std::string account_name = session_.local_user().display_name;
  const std::string display_name =
      account_name.empty() ? username_->text().toStdString() : account_name;

  if (const auto joined = session_.join(room.toStdString(), display_name); !joined) {
    apply_error(QString::fromStdString(joined.error().code),
                QString::fromStdString(joined.error().message));
  }
}

void MainWindow::on_leave_room() {
  if (const auto left = session_.leave(); !left) {
    apply_error(QString::fromStdString(left.error().code),
                QString::fromStdString(left.error().message));
  }
  screen_view_->set_receiving(false);
  set_sharing_text(QString{});
  // The conversation belongs to the room that was just left. Leaving it on
  // screen would put one room's messages above the next room's history.
  chat_view_->clear();
  chat_input_->clear();
  clear_metrics();
  forget_participants();
  // Nothing is being captured any more, so nothing will arrive to bring the
  // meter down. Left as it was, it would hold whatever the last syllable of
  // the call measured, on a page nobody is speaking into.
  quiet_level();
  refresh_controls();
  show_page();
}

void MainWindow::clear_metrics() {
  metrics_->clear();
  // And the verdict this window believes is on screen. apply_metrics only
  // restyles the indicator when the verdict moves, so without this the next
  // call would find the label already showing what it is about to report, skip
  // the write, and leave the label cleared for the rest of the call.
  shown_quality_ = -1;
  shown_link_quality_ = -1;

  // The indicator changes hands rather than going out. The signaling link is
  // measured whether or not there is a call, so what goes up here is at most
  // one interval old - and a status bar that empties itself the moment a call
  // ends is what left "is my connection any good" unanswered everywhere except
  // inside a room.
  show_link_quality();
}

void MainWindow::on_send_chat() {
  const QString text = chat_input_->text().trimmed();
  if (text.isEmpty()) {
    return;
  }

  if (const auto sent = session_.send_chat(text.toStdString()); !sent) {
    apply_error(QString::fromStdString(sent.error().code),
                QString::fromStdString(sent.error().message));
    return;
  }

  // Emptied as soon as it is sent rather than when it comes back, so the field
  // is ready for the next line. Nothing is displayed from here either way:
  // what appears in the list is the copy the server broadcast.
  chat_input_->clear();
}

void MainWindow::on_open_emoji_picker() {
  // A new one each time, deleted when it closes. What it shows never changes,
  // but a picker kept around would need to be moved every time the sidebar
  // did, and building forty flat buttons is not the expensive part of a click.
  auto* picker = new EmojiPicker(chat_emoji_);
  connect(picker, &EmojiPicker::picked, this, &MainWindow::insert_emoji);
  picker->open_above(chat_emoji_);
}

void MainWindow::insert_emoji(const QString& emoji) {
  // At the cursor, replacing whatever is selected, which is what typing the
  // character would have done. QLineEdit::insert honours the field's maximum
  // as well, so this cannot get past the guard that typing respects.
  chat_input_->insert(emoji);
  // Back to the field, so the next thing typed continues the message instead
  // of going to a button.
  chat_input_->setFocus();
}

void MainWindow::on_toggle_mute() {
  if (const auto applied = session_.set_muted(mute_button_->isChecked()); !applied) {
    apply_error(QString::fromStdString(applied.error().code),
                QString::fromStdString(applied.error().message));
  }
  refresh_controls();
}

void MainWindow::on_toggle_share() {
  if (session_.sharing_screen()) {
    if (const auto stopped = session_.stop_screen_share(); !stopped) {
      apply_error(QString::fromStdString(stopped.error().code),
                  QString::fromStdString(stopped.error().message));
    }
    refresh_controls();
    return;
  }

  // The whole screen or one window, asked at the moment of sharing. Settings
  // holds the monitor half of the choice, but somebody who never opened it
  // had no way of knowing there was a choice to make, and a list of windows
  // is only true at the moment it is shown.
  if (!choose_source()) {
    refresh_controls();
    return;
  }

  // Whatever the dialog last chose, or what the configuration says for
  // somebody who has never opened it.
  const client::app::ScreenAudio audio =
      screen_audio_.value_or(client::app::ScreenAudio{.mode = session_.screen_audio_mode()});

  if (const auto started = session_.start_screen_share(share_source_.toStdString(), audio);
      !started) {
    apply_error(QString::fromStdString(started.error().code),
                QString::fromStdString(started.error().message));
    refresh_controls();
    return;
  }

  // Sound was asked for and did not start. The share is up either way - see
  // CallSession::start_screen_share - so this is a note and not an error
  // dialog, but it has to be said: a silent share that was meant to have sound
  // is otherwise indistinguishable from one that does.
  if (audio.mode != client::app::ScreenAudio::Mode::None && !session_.screen_audio_active()) {
    const Error why = session_.screen_audio_failure();
    status_->setText(
        why.message.empty()
            ? QStringLiteral("Sharing without sound")
            : QStringLiteral("Sharing without sound: %1").arg(QString::fromStdString(why.message)));
  }
  refresh_controls();
}

bool MainWindow::choose_source() {
  const auto screens = session_.monitors();
  const auto windows = session_.windows();
  if (!screens && !windows) {
    // Nothing this side can name: a build without libwebrtc, or a machine
    // with no display. The capturer sorts out the rest, including saying so
    // when there is nothing to grab.
    return true;
  }

  // The one in use, or the primary monitor for somebody who has never chosen.
  // Marked rather than preselected: a menu has no selection, and the mark
  // says which one "the same as last time" would be.
  QString current = share_source_;
  if (current.isEmpty() && screens) {
    for (const client::video::Monitor& monitor : screens.value()) {
      if (monitor.is_primary) {
        current = QString::fromStdString(monitor.id);
      }
    }
  }

  QMenu menu(this);
  const auto offer = [&current](QMenu& where, const QString& label, const QString& id) {
    QAction* action = where.addAction(label);
    action->setCheckable(true);
    action->setChecked(id == current);
    action->setData(id);
    return action;
  };

  // The whole screen first. With one monitor it is simply the screen; with
  // more than one, each by name, and the primary says so.
  if (screens) {
    const bool several = screens.value().size() > 1;
    for (const client::video::Monitor& monitor : screens.value()) {
      const QString name = QString::fromStdString(monitor.name);
      QString label = QStringLiteral("Entire screen");
      if (several) {
        label = monitor.is_primary ? QStringLiteral("%1, primary").arg(name) : name;
      }
      offer(menu, label, QString::fromStdString(monitor.id));
    }
  }

  // Then one window, in a submenu of its own so that a machine with forty of
  // them open does not push the screens off the bottom of the list. A title
  // is what the user knows a window by, so it is shown whole where it fits
  // and cut in the middle where it does not: the end of a title is usually
  // the program's name, and losing it is losing the one part that tells two
  // documents in two programs apart. The whole title waits in the tooltip.
  QMenu* by_window = menu.addMenu(QStringLiteral("One window"));
  by_window->setToolTipsVisible(true);
  if (windows && !windows.value().empty()) {
    const QFontMetrics metrics = by_window->fontMetrics();
    for (const client::video::Window& window : windows.value()) {
      const QString title = QString::fromStdString(window.title);
      // Cut first, escaped second. An ampersand is a letter in a title and a
      // mnemonic to a menu, and doubling it is what keeps it a letter; cutting
      // after the doubling could leave half of the pair behind.
      QString label = metrics.elidedText(title, Qt::ElideMiddle, kWindowTitleWidth);
      label.replace(QLatin1Char('&'), QStringLiteral("&&"));
      QAction* action = offer(*by_window, label, QString::fromStdString(window.id));
      action->setToolTip(title);
    }
  } else {
    QAction* none = by_window->addAction(QStringLiteral("No window to share"));
    none->setEnabled(false);
  }

  // Under the button, where a menu that a button opened belongs. Nothing
  // chosen - Escape, a click elsewhere - means no share, and the caller puts
  // the button back.
  QAction* chosen = menu.exec(share_button_->mapToGlobal(QPoint(0, share_button_->height())));
  if (chosen == nullptr) {
    return false;
  }
  share_source_ = chosen->data().toString();

  // A monitor is also what the box in Settings shows, so the box follows. A
  // window is not something the box can show, and it keeps the monitor it
  // had: that is what "the monitor" still means for the next time a screen
  // is the choice.
  if (screens) {
    for (const client::video::Monitor& monitor : screens.value()) {
      if (QString::fromStdString(monitor.id) == share_source_) {
        monitor_id_ = share_source_;
      }
    }
  }
  return true;
}

void MainWindow::on_open_settings() {
  SettingsDialog dialog(session_, this);
  // Otherwise every visit to Settings, for whatever reason, would quietly put
  // the monitor back to the first in the list.
  dialog.select_monitor(monitor_id_);
  const QString monitor_before = dialog.selected_monitor();
  dialog.exec();
  monitor_id_ = dialog.selected_monitor();
  // A monitor picked there is the next thing shared. A box left alone leaves
  // the share menu's choice alone too, which matters when that choice was a
  // window: somebody who opened Settings to turn the chime off did not mean
  // to switch from sharing their editor to sharing their whole screen.
  if (monitor_id_ != monitor_before) {
    share_source_ = monitor_id_;
  }
  screen_audio_ = dialog.selected_screen_audio();
}

void MainWindow::on_open_metrics() {
  // Raised rather than opened a second time. Pressing the button while the
  // charts are already up should bring them forward, not start a second window
  // charting the same call from a minute later than the first.
  if (metrics_dialog_.isNull()) {
    metrics_dialog_ = new MetricsDialog(session_, this);
  }
  metrics_dialog_->show();
  metrics_dialog_->raise();
  metrics_dialog_->activateWindow();
}

void MainWindow::on_copy_room_id() {
  // The code alone, not the "Room: " the label reads. Whoever receives it pastes
  // it into the Room ID field, and on_join_room only trims and upper cases what
  // it finds there - a prefix would survive that and fail the join.
  const QString room = QString::fromStdString(session_.room_id());
  if (room.isEmpty()) {
    return;
  }
  QGuiApplication::clipboard()->setText(room);

  // A notification rather than the button changing its own text: a button
  // that renames itself moves what is beside it. It used to be a line in the
  // status bar, which stayed there saying "copied" until the state of the
  // call next changed - long after it had stopped being news.
  notify(ToastKind::Success, QStringLiteral("Room code %1 copied").arg(room));
}

void MainWindow::on_participant_selected() {
  const QListWidgetItem* item = participants_->currentItem();
  if (item == nullptr) {
    selected_participant_.clear();
    volume_->setEnabled(false);
    volume_label_->setText(QStringLiteral("Volume: select a participant"));
    return;
  }

  selected_participant_ = item->data(Qt::UserRole).toString();

  // Nobody plays back their own voice, so there is no volume to set for
  // yourself. Leaving the slider live would let it be dragged with nothing
  // happening at the other end of it.
  if (selected_participant_ == QString::fromStdString(session_.local_user().id)) {
    volume_->setEnabled(false);
    volume_label_->setText(QStringLiteral("Volume: you do not hear yourself"));
    return;
  }

  volume_->setEnabled(true);

  // The slider has to show the volume of whoever is selected now. Left where
  // the last selection put it, it would claim a value that was never applied
  // to this participant, and the first nudge would move their volume from
  // somewhere they had never been.
  const int volume = volumes_.value(selected_participant_, 100);
  {
    const QSignalBlocker blocker(volume_);
    volume_->setValue(volume);
  }
  update_volume_label(item->data(kNameRole).toString(), volume);
}

void MainWindow::update_volume_label(const QString& participant, int volume) {
  volume_label_->setText(QStringLiteral("Volume for %1: %2%").arg(participant).arg(volume));
}

void MainWindow::on_volume_changed(int value) {
  if (selected_participant_.isEmpty()) {
    return;
  }
  const double volume = static_cast<double>(value) / 100.0;
  if (const auto applied =
          session_.set_participant_volume(selected_participant_.toStdString(), volume);
      !applied) {
    apply_error(QString::fromStdString(applied.error().code),
                QString::fromStdString(applied.error().message));
    return;
  }

  volumes_[selected_participant_] = value;
  if (const QListWidgetItem* item = participants_->currentItem(); item != nullptr) {
    update_volume_label(item->data(kNameRole).toString(), value);
  }
}

void MainWindow::apply_state(int state, const QString& detail) {
  state_ = static_cast<client::app::CallSession::State>(state);

  QString text = to_display(state_);
  const models::User user = session_.local_user();
  if (!user.display_name.empty() && state_ != client::app::CallSession::State::Idle) {
    text += QStringLiteral(" · ") + QString::fromStdString(user.display_name);
  }
  // The detail is machine talk most of the time. It only earns a place when
  // something went wrong or is being retried, which is when it is the answer
  // to the question the user is asking.
  if ((state_ == client::app::CallSession::State::Failed ||
       state_ == client::app::CallSession::State::Connecting) &&
      !detail.isEmpty()) {
    text += QStringLiteral(" · ") + detail;
  }
  status_->setText(text);

  if (state_ == client::app::CallSession::State::Failed) {
    show_login_error(describe(QString{}, detail));
  }

  const QString name = QString::fromStdString(user.display_name);
  home_header_->set_subtitle(name.isEmpty() ? QString{} : QStringLiteral("Hello, %1").arg(name));
  // The badge on the rail is the same person. The identifier when the account
  // has no name of its own, which is still a better letter than none.
  account_avatar_->set_name(name.isEmpty() ? QString::fromStdString(user.id) : name);
  account_avatar_->setToolTip(
      QStringLiteral("Account: %1").arg(name.isEmpty() ? QString::fromStdString(user.id) : name));
  account_avatar_->setAccessibleName(QStringLiteral("Account"));

  // Again here, and not only where the button was pressed. The metrics arrive
  // through a queued call, so one measured a moment before the call ended can
  // be delivered after on_leave_room has already emptied the labels, and it
  // would write the last numbers of a finished call back onto the lobby. This
  // runs once the state itself says there is no call, which is after that.
  if (state_ != client::app::CallSession::State::InCall) {
    clear_metrics();
    forget_participants();
  }

  refresh_controls();
  show_page();
}

void MainWindow::apply_room_list(const QStringList& rows, bool may_create) {
  // Read off the rows before they are handed to the table, which keeps only
  // what it shows. The row is identifier, identifier, name, people, and then
  // the size and the count as bare numbers for the administrator's panel; the
  // first field is the hidden key every table here carries. See ui::fill.
  room_names_.clear();
  room_names_.reserve(static_cast<int>(rows.size()));
  room_people_.clear();
  room_people_.reserve(static_cast<int>(rows.size()));
  for (const QString& row : rows) {
    const QStringList fields = row.split(QLatin1Char('\t'));
    if (fields.size() >= 3) {
      room_names_.insert(fields.at(0), fields.at(2));
    }
    if (fields.size() >= 4) {
      room_people_.insert(fields.at(0), fields.at(3));
    }
  }
  // The list is what carries the name, and it arrives after the join as often
  // as before it.
  refresh_room_title();

  room_rows_ = rows;
  rebuild_room_cards();

  // The two numbers over the cards, from the same answer: how many rooms
  // there are, and how many people are in them altogether. The count is the
  // sixth field, a bare number; see the on_room_list handler.
  int people = 0;
  for (const QString& row : rows) {
    people += row.section(QLatin1Char('\t'), 5, 5).toInt();
  }
  stat_rooms_->set_value(QString::number(rows.size()));
  stat_people_->set_value(QString::number(people));

  // Disabled rather than left to be refused on the click. The server answers
  // room_limit_reached either way, and a button that always fails is a worse
  // way to learn a rule than a button that is plainly not available.
  create_button_->setEnabled(may_create);
  create_button_->setToolTip(may_create
                                 ? QString()
                                 : QStringLiteral("You already have a room. An administrator has "
                                                  "to close it before you can make another."));
}

void MainWindow::announce_update(const QString& version, const QUrl& page, const QUrl& installer) {
  // The whole line is rebuilt rather than appended to, so that a second and
  // newer release arriving while this window is open replaces the first rather
  // than queueing behind it.
  //
  // Escaped, both halves. Neither can carry a bracket today - one is three
  // numbers this build itself parsed, the other a URL already refused unless it
  // is https on github.com - but both came off the network, and the place to
  // apply that rule is where the string becomes markup, not four functions
  // upstream where somebody can later stop applying it.
  version_->setTextFormat(Qt::RichText);
  version_->setText(
      QStringLiteral("PartyShare " DV_VERSION " &middot; <a href=\"%1\">%2 available</a>")
          .arg(page.toString().toHtmlEscaped(), version.toHtmlEscaped()));
  // Clickable *and* still selectable. Dropping the second flag would take away
  // the reason the version is written here at all: a bug report with the number
  // copied rather than retyped.
  version_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  // Qt opens it with QDesktopServices, which is the platform's own "open this"
  // and lands in the browser the person already uses. Nothing is downloaded
  // here and nothing is run: this is a link, and the decision stays theirs.
  version_->setOpenExternalLinks(true);
  version_->setToolTip(QStringLiteral("Running PartyShare %1. Version %2 has been published: %3")
                           .arg(QStringLiteral(DV_VERSION), version, page.toString()));

  // And the question. A newer release arriving while the one before it is
  // still being asked about takes its place: two dialogs about two versions
  // is one more than anybody can answer.
  if (update_dialog_ != nullptr) {
    update_dialog_->reject();
  }
  update_version_ = version;
  update_page_ = page;
  update_installer_ = installer;
  update_pending_ = true;

  if (pages_->currentIndex() == kRoomPage) {
    // In a call the question waits, and this says that it is waiting: a line
    // that goes away on its own, where the dialog would have had to be
    // answered by somebody who is talking.
    notify(ToastKind::Info,
           QStringLiteral("PartyShare %1 is available. You will be asked about it when you "
                          "leave the room.")
               .arg(version));
    return;
  }
  offer_update();
}

void MainWindow::offer_update() {
  if (!update_pending_ || pages_->currentIndex() == kRoomPage) {
    return;
  }
  // Somebody is in the middle of another dialog - the settings, a new room, a
  // new password. A second modal window over the first leaves two things
  // waiting for an answer and only one of them reachable, so this one comes
  // back when the first has been dealt with.
  if (QApplication::activeModalWidget() != nullptr) {
    QTimer::singleShot(kUpdateRetryMs, this, &MainWindow::offer_update);
    return;
  }
  update_pending_ = false;

  const bool direct = update_installer_.isValid();
  const QUrl target = direct ? update_installer_ : update_page_;

  auto* dialog = new UpdateDialog(update_version_, QStringLiteral(DV_VERSION), direct, this);
  // Answered once and then gone: the next release builds its own.
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, target, direct] {
    // The platform's "open this" on an address ui::UpdateChecker has already
    // refused unless it is https on github.com. For the installer that is the
    // browser starting a download; this program fetches nothing and runs
    // nothing, and the file is the person's to open.
    if (!QDesktopServices::openUrl(target)) {
      notify(ToastKind::Error,
             QStringLiteral("The browser could not be opened. The link in the status bar leads "
                            "to the same release."));
      return;
    }
    if (direct) {
      // The one thing the installer needs and cannot say before it is run:
      // it replaces the files this window is running from.
      notify(ToastKind::Info,
             QStringLiteral("The installer is downloading in your browser. Close PartyShare "
                            "before you run it."));
    }
  });
  update_dialog_ = dialog;
  // open() and not exec(): this is reached from a network reply and from a
  // timer, and a second event loop started inside either would run the call
  // underneath it from within a slot that has not returned.
  dialog->open();
}

void MainWindow::apply_participants(const QStringList& names) {
  const QString selected = selected_participant_;
  // Ourselves, so that our own arrival is not announced back to us: the list
  // the server sends includes the person reading it.
  const QString us = QString::fromStdString(session_.local_user().id);

  QSet<QString> present;
  present.reserve(static_cast<int>(names.size()));
  QStringList arrived;

  // Rebuilt wholesale, so the selection has to be restored by identity rather
  // than by row: the order changes as people join and leave.
  const QSignalBlocker blocker(participants_);
  participants_->clear();
  for (const QString& entry : names) {
    // Field 0 is the identifier, field 1 the name, field 2 the state bits and
    // field 3 the level. See on_participants for why the identifier is first
    // and not in the middle.
    const QStringList parts = entry.split(QLatin1Char('\t'));
    const int state = parts.value(2).toInt();
    auto* item = new QListWidgetItem(parts.value(1), participants_);
    item->setData(kParticipantIdRole, parts.value(0));
    item->setData(kNameRole, parts.value(1));
    item->setData(kParticipantStateRole, state);
    item->setData(kParticipantLevelRole, parts.value(3).toDouble());
    // The words the icons stand for, for whoever hovers to ask and for a
    // screen reader, which cannot see a struck-through microphone.
    const QString words = describe_participant_state(state);
    item->setToolTip(words);
    item->setData(Qt::AccessibleDescriptionRole, words);
    if (!selected.isEmpty() && parts.value(0) == selected) {
      participants_->setCurrentItem(item);
    }

    // An arrival is a difference between this list and the last one. The
    // server broadcasts the whole membership whenever it changes rather than
    // a join event, so there is nothing else to compare against.
    const QString id = parts.value(0);
    present.insert(id);
    if (participants_seeded_ && id != us && !known_participants_.contains(id)) {
      arrived.push_back(parts.value(1));
    }
  }

  // Before the set is replaced, and only over what was already there: a
  // departure is the half of the difference that this list cannot show,
  // because whoever left is not in it.
  bool departed = false;
  for (const QString& id : known_participants_) {
    if (id != us && !present.contains(id)) {
      departed = true;
      break;
    }
  }

  known_participants_ = present;
  participants_seeded_ = true;

  // The notification goes first, because whether it went up is what decides
  // whether the chime should follow.
  //
  // Only arrivals get one. Somebody leaving is news to the room and not to the
  // operating system: the chime says it, and a balloon for every departure
  // would be a queue of them at the end of each call.
  const bool announced = !arrived.isEmpty() && announce_arrivals(arrived);

  // The chime plays whether or not this window has the focus, which is the
  // difference between it and the notification above - except when the
  // notification is what just happened. A balloon carries the system's own
  // sound and cannot be asked not to, so ringing as well would be one arrival
  // and two sounds over the top of each other. See Notifier::notify.
  //
  // One sound per update rather than one per person, and an update carrying an
  // arrival and a departure at once gets the arrival's: the platform has one
  // voice, so a second call cuts the first off, and two cues talking over each
  // other say less than one.
  if (!announced) {
    if (!arrived.isEmpty()) {
      play_chime(Chime::Joined);
    } else if (departed) {
      play_chime(Chime::Left);
    }
  }

  // The signals were blocked through the rebuild, so the volume controls have
  // not heard about any of it. This is also what clears them when whoever was
  // selected has left the room.
  on_participant_selected();

  refresh_faces(names);
  refresh_side_tabs();
}

void MainWindow::refresh_faces(const QStringList& entries) {
  // Updated in place rather than rebuilt. This list arrives several times a
  // second while somebody is speaking - it carries the levels - and tearing
  // down and making a row of widgets at that rate is a flicker and a cost for
  // what is, nearly every time, one ring moving.
  QSet<QString> present;
  int placed = 0;
  int overflow = 0;
  for (const QString& entry : entries) {
    const QStringList parts = entry.split(QLatin1Char('\t'));
    const QString id = parts.value(0);
    const QString name = parts.value(1);
    const int state = parts.value(2).toInt();
    if (placed >= kMaxFaces) {
      ++overflow;
      continue;
    }
    present.insert(id);

    Avatar* face = faces_.value(id);
    if (face == nullptr) {
      face = new Avatar(kFace, room_sidebar_);
      faces_.insert(id, face);
    }
    // In the order of the list, which is the order of the People tab. Before
    // the stretch and the "+N" that close the row.
    if (avatar_strip_->indexOf(face) != placed) {
      avatar_strip_->removeWidget(face);
      avatar_strip_->insertWidget(placed, face);
      face->show();
    }
    face->set_name(name);
    const bool muted = (state & kParticipantMuted) != 0;
    face->set_ring(!muted && (state & kParticipantSpeaking) != 0);
    // The ring is a colour and a shape; the words are here for whoever hovers
    // to ask, the same words the list's own tooltip uses.
    const QString words = describe_participant_state(state);
    const QString tip = words.isEmpty() ? name : QStringLiteral("%1 · %2").arg(name, words);
    // Only when it says something new: this runs several times a second, and
    // setting a tooltip is an event sent to the widget whether or not the
    // words changed.
    if (face->toolTip() != tip) {
      face->setToolTip(tip);
    }
    ++placed;
  }

  for (auto it = faces_.begin(); it != faces_.end();) {
    if (present.contains(it.key())) {
      ++it;
      continue;
    }
    avatar_strip_->removeWidget(it.value());
    it.value()->deleteLater();
    it = faces_.erase(it);
  }

  if (more_faces_ == nullptr) {
    more_faces_ = new QLabel(room_sidebar_);
    theme::set_role(more_faces_, theme::TextRole::Caption);
    avatar_strip_->addWidget(more_faces_);
  }
  more_faces_->setText(QStringLiteral("+%1").arg(overflow));
  more_faces_->setVisible(overflow > 0);
}

bool MainWindow::announce_arrivals(const QStringList& names) {
  // Nothing to say to somebody who is looking at the list this happened in.
  // They watched the row appear, and a balloon over the top of it is a
  // notification about something already seen.
  if (notifier_.window_has_attention()) {
    return false;
  }

  // One name is the whole message and belongs in the title, which is the line
  // a balloon shows largest. Several are a crowd, and the crowd is the news.
  if (names.size() == 1) {
    return notifier_.notify(names.front(), QStringLiteral("joined the room"));
  }

  const QString body =
      names.size() == 2 ? QStringLiteral("%1 and %2 joined the room").arg(names.at(0), names.at(1))
                        : QStringLiteral("%1, %2 and %3 others joined the room")
                              .arg(names.at(0), names.at(1))
                              .arg(names.size() - 2);
  return notifier_.notify(QStringLiteral("New participants"), body);
}

void MainWindow::forget_participants() {
  known_participants_.clear();
  participants_seeded_ = false;
}

void MainWindow::apply_metrics(const QString& summary, int quality) {
  // A measurement taken a moment before the call ended can be delivered after
  // it, and writing it here would put a finished call's verdict over the
  // lobby's own indicator - which is now a live measurement of something else
  // rather than an empty label.
  if (state_ != client::app::CallSession::State::InCall) {
    return;
  }

  metrics_->setText(summary);

  // Only when the verdict has actually moved. Setting a stylesheet makes Qt
  // re-resolve the widget's whole style, and this arrives on a timer whether
  // anything changed or not, so most of the time it was that work to arrive at
  // the colour the label already had.
  if (quality == shown_quality_) {
    return;
  }
  shown_quality_ = quality;
  // The link indicator does not own the label while a call does, so what it
  // believes is on screen is no longer true.
  shown_link_quality_ = -1;

  const auto measured = static_cast<client::app::NetworkQuality>(quality);
  if (measured == client::app::NetworkQuality::Unknown) {
    quality_->clear();
    return;
  }
  quality_->setText(QStringLiteral("%1 network %2")
                        .arg(from_utf8(client::app::glyph(measured)),
                             from_utf8(client::app::to_string(measured))));
  quality_->setStyleSheet(
      QStringLiteral("color: %1; font-weight: bold;").arg(quality_colour(measured).name()));
}

void MainWindow::apply_link(int round_trip_ms, bool connected, bool attempting, int sequence) {
  link_round_trip_ms_ = round_trip_ms;
  // A socket still on its first attempt is "not known yet", which is drawn as
  // a wait, and not "not there", which is drawn as the answer.
  if (connected) {
    link_connected_ = 1;
  } else if (attempting) {
    link_connected_ = -1;
  } else {
    link_connected_ = 0;
  }

  // The Test button's answer, if one is owed and this report is about the
  // socket it opened. The address is in the sentence because the answer can
  // arrive after the field has been changed to something else.
  if (!server_under_test_.isEmpty() && sequence > server_test_since_) {
    if (!connected && !attempting) {
      show_server_hint(QStringLiteral("Could not reach %1. Check the address, and that the "
                                      "server is running there.")
                           .arg(server_under_test_),
                       true);
      server_under_test_.clear();
    } else if (connected && round_trip_ms >= 0) {
      show_server_hint(
          QStringLiteral("%1 answered in %2 ms.").arg(server_under_test_).arg(round_trip_ms),
          false);
      server_under_test_.clear();
    } else if (connected) {
      show_server_hint(QStringLiteral("Connected to %1. Measuring...").arg(server_under_test_),
                       false);
    }
  }

  // While there is a call, the verdict weighing all three measurements is the
  // better answer and keeps the label. This one fills the rest of the time,
  // which is most of it, and is kept up to date underneath so that leaving a
  // room has something current to fall back to rather than a blank wait.
  if (state_ == client::app::CallSession::State::InCall) {
    return;
  }
  show_link_quality();
}

void MainWindow::refresh_server_stat() {
  // The same three answers the status bar's indicator gives, said large on
  // the lobby's card. Worked out from the same two members rather than from
  // the indicator's text, so the two cannot drift into saying different
  // things about one measurement.
  if (link_connected_ == 0) {
    stat_server_->set_value(QStringLiteral("offline"));
    stat_server_->set_status(QStringLiteral("not answering"), StatCard::Tone::Poor);
    return;
  }
  if (link_round_trip_ms_ < 0) {
    stat_server_->set_value(QStringLiteral("–"));
    stat_server_->set_status(link_connected_ == 1 ? QStringLiteral("online, measuring")
                                                  : QStringLiteral("looking for it"));
    return;
  }
  const auto measured = client::app::quality_of(std::chrono::milliseconds(link_round_trip_ms_));
  StatCard::Tone tone = StatCard::Tone::Neutral;
  switch (measured) {
    case client::app::NetworkQuality::Good:
      tone = StatCard::Tone::Good;
      break;
    case client::app::NetworkQuality::Fair:
      tone = StatCard::Tone::Fair;
      break;
    case client::app::NetworkQuality::Poor:
      tone = StatCard::Tone::Poor;
      break;
    case client::app::NetworkQuality::Unknown:
      break;
  }
  stat_server_->set_value(QStringLiteral("%1 ms").arg(link_round_trip_ms_));
  stat_server_->set_status(from_utf8(client::app::to_string(measured)), tone);
}

void MainWindow::show_link_quality() {
  refresh_server_stat();
  if (link_connected_ == 0) {
    // The server is not there, or has gone: the socket was refused, or
    // dropped and is being retried. Red, and the filled dot, because this is
    // the one answer the indicator exists to give somebody at the login
    // screen - signing in cannot work until it changes - and a grey "no
    // server" read as the indicator having nothing to say.
    if (shown_link_quality_ != kLinkOffline) {
      shown_link_quality_ = kLinkOffline;
      shown_quality_ = -1;
      quality_->setStyleSheet(
          QStringLiteral("color: %1; font-weight: bold;").arg(theme::colors().danger.name()));
    }
    quality_->setText(QStringLiteral("%1 server offline")
                          .arg(from_utf8(client::app::glyph(client::app::NetworkQuality::Good))));
    return;
  }

  if (link_round_trip_ms_ < 0) {
    // Not blank. "Nothing is being measured" is itself the answer to whether
    // the connection is any good, and an empty status bar says it so quietly
    // that it reads as the indicator being broken.
    //
    // Two wordings: the socket is up and the first probe has not come back,
    // which lasts a few seconds, or nothing is known yet, which is the moment
    // the window opens and the probe has not been answered either way.
    if (shown_link_quality_ != kNoLinkQuality) {
      shown_link_quality_ = kNoLinkQuality;
      shown_quality_ = -1;
      quality_->setStyleSheet(
          QStringLiteral("color: %1; font-weight: bold;").arg(theme::colors().muted.name()));
    }
    const QString glyph = from_utf8(client::app::glyph(client::app::NetworkQuality::Unknown));
    quality_->setText(link_connected_ == 1
                          ? QStringLiteral("%1 server online").arg(glyph)
                          : QStringLiteral("%1 looking for the server").arg(glyph));
    return;
  }

  const auto measured = client::app::quality_of(std::chrono::milliseconds(link_round_trip_ms_));
  // The number rather than the word, unlike the call indicator, because there
  // is only one measurement here and hiding it behind an adjective would leave
  // "good" doing work three numbers do during a call.
  //
  // Which is why the glyph matters more here than there: during a call the
  // verdict is written out beside the dot, and here it is not. Without a shape
  // the reading is a bare number, and knowing whether 180 ms is fair or poor
  // means remembering where the thresholds are.
  quality_->setText(QStringLiteral("%1 server %2 ms")
                        .arg(from_utf8(client::app::glyph(measured)))
                        .arg(link_round_trip_ms_));

  if (const int verdict = static_cast<int>(measured); verdict != shown_link_quality_) {
    shown_link_quality_ = verdict;
    shown_quality_ = -1;
    quality_->setStyleSheet(
        QStringLiteral("color: %1; font-weight: bold;").arg(quality_colour(measured).name()));
  }
}

void MainWindow::apply_local_level(double level, bool speaking) {
  // Aimed at, not written to. The bar travels there over the next few frames,
  // fast on the way up and slowly on the way down, which is what makes a
  // syllable readable instead of a flicker. See app::LevelMeter.
  level_.observe(client::app::meter_fraction(level));
  if (!level_.at_rest() && !level_timer_->isActive()) {
    level_drawn_at_ms_ = level_clock_.elapsed();
    level_timer_->start();
  }

  // The meter turns the accent colour while somebody is actually speaking into
  // it, which the stylesheet does off this property. Only on a change: this
  // runs several times a second, and re-resolving a stylesheet at that rate to
  // arrive at the colour it already had is work nobody sees.
  if (microphone_level_->property("speaking").toBool() == speaking) {
    return;
  }
  microphone_level_->setProperty("speaking", speaking);
  microphone_level_->style()->unpolish(microphone_level_);
  microphone_level_->style()->polish(microphone_level_);
}

void MainWindow::animate_level() {
  const qint64 now = level_clock_.elapsed();
  const auto elapsed = static_cast<double>(now - level_drawn_at_ms_);
  level_drawn_at_ms_ = now;

  const double filled = level_.advance(elapsed);
  microphone_level_->setValue(static_cast<int>(std::lround(filled * kMeterSteps)));

  if (level_.at_rest()) {
    // Arrived. The next measurement is what starts this again, and between two
    // of them there is nothing here to draw that is not already drawn.
    level_timer_->stop();
  }
}

void MainWindow::quiet_level() {
  level_timer_->stop();
  level_ = client::app::LevelMeter{};
  microphone_level_->setValue(0);
}

void MainWindow::apply_error(const QString& code, const QString& message) {
  DV_LOG_WARN("UI: {} ({})", message.toStdString(), code.toStdString());

  const QString text = describe(code, message);
  if (pages_->currentIndex() == kLoginPage) {
    show_login_error(text);
    return;
  }
  // While the form for a new room is up, whatever went wrong is about what is
  // in it, and it is said there - where the name can be changed - rather than
  // on the page behind a dialog that is in the way of it.
  if (create_dialog_->isVisible()) {
    create_form_->show_error(text);
    return;
  }
  // A notification, where this used to be a box with an OK button. A box
  // stops whatever was being done until it is dismissed, which is a lot to
  // ask of somebody mid-call for "someone else is already sharing"; a
  // notification says the same and keeps out of the way. How long it stays is
  // toast_lifetime_ms's to decide.
  notify(ToastKind::Error, text);
}

void MainWindow::apply_room_created(const QString& room_id) {
  // The name belongs to the room that now exists. Left in the field, it would
  // quietly become the name of the next room created from this window.
  room_name_->clear();
  // The form has its answer, which is what it was kept open for.
  create_dialog_->accept();

  // Not from the administration panel. A persistent room is pre-created there
  // for other people to use later, and joining it would take the administrator
  // out of the panel and into a call they did not ask for.
  if (pages_->currentIndex() == kAdminPage) {
    admin_panel_->refresh();
    return;
  }

  // Everywhere else, created and entered in one movement. Asking someone to
  // press a second button to walk into the room they just made is a step with
  // no decision in it.
  join_room(room_id);
}

void MainWindow::apply_screen_share(const QString& user_id) {
  if (user_id.isEmpty()) {
    set_sharing_text(QString{});
    screen_view_->set_placeholder(QStringLiteral("nobody is sharing a screen"));
    // Closed rather than cleared. Whoever was sharing has stopped, or has left
    // the room, and the frames already inside the decoder are still on their
    // way here - see ScreenView::set_receiving. A clear alone let the last of
    // them land a moment later and freeze on screen for the rest of the call.
    screen_view_->set_receiving(false);
    refresh_controls();
    return;
  }

  const QString name = participant_name(user_id);

  const bool is_me = user_id == QString::fromStdString(session_.local_user().id);
  set_sharing_text(is_me ? QStringLiteral("You are sharing")
                         : QStringLiteral("%1 is sharing").arg(name));

  // Nobody receives their own screen back, so while you are the one sharing
  // this panel stays empty. Saying that nobody is sharing would be wrong in
  // exactly the moment it matters most.
  if (is_me) {
    screen_view_->set_placeholder(QStringLiteral("you are sharing this screen with the room"));
    screen_view_->set_receiving(false);
  } else {
    screen_view_->set_receiving(true);
  }
  refresh_controls();
}

void MainWindow::set_sharing_text(const QString& text) {
  sharing_label_->setText(text);
  // Hidden when there is nothing to say. A chip is a filled shape, and an
  // empty one is a small coloured lozenge in the header about nothing.
  sharing_label_->setVisible(!text.isEmpty());
}

QString MainWindow::participant_name(const QString& user_id) const {
  for (int row = 0; row < participants_->count(); ++row) {
    const QListWidgetItem* item = participants_->item(row);
    if (item->data(Qt::UserRole).toString() == user_id) {
      return item->data(kNameRole).toString();
    }
  }
  // Somebody who has just left, or an identifier the list never had. The
  // identifier is a worse name than a name and a better one than a blank.
  return user_id;
}

void MainWindow::on_toggle_fullscreen() {
  if (fullscreen_) {
    leave_fullscreen();
    return;
  }
  enter_fullscreen();
}

void MainWindow::enter_fullscreen() {
  if (fullscreen_) {
    return;
  }
  // Only a picture worth filling the screen with: somebody else's share, in a
  // call, on the room page. F11 and the double click arrive here from anywhere
  // the window is active, and outside that state they do nothing rather than
  // turning the window into a black rectangle with a sentence in it.
  const std::string sharer = session_.screen_sharer();
  const bool someone_else_is_sharing = state_ == client::app::CallSession::State::InCall &&
                                       !sharer.empty() && sharer != session_.local_user().id;
  if (!someone_else_is_sharing || pages_->currentIndex() != kRoomPage) {
    return;
  }

  fullscreen_ = true;
  // Everything that leave_fullscreen puts back, taken down in one place.
  // The window state is what decides between "back to normal" and "back to
  // maximized", and the splitter's sizes are where the person had dragged
  // the handle to: a sidebar that comes back at its default width has lost
  // an adjustment nobody asked to lose.
  before_fullscreen_ = windowState();
  splitter_before_fullscreen_ = room_body_->sizes();
  margins_before_fullscreen_ = room_column_->contentsMargins();

  room_header_->hide();
  room_sidebar_->hide();
  sidebar_->hide();
  statusBar()->hide();
  room_column_->setContentsMargins(0, 0, 0, 0);
  screen_view_->set_edge_to_edge(true);
  // The call controls stay. They used to go with everything else, which left
  // Esc as the only thing a person in full screen could do; floating, they
  // cost the picture nothing - they slide away when the pointer is still and
  // come back when it moves. See wake_dock.
  fullscreen_button_->set_icon(Icon::ExitFullscreen);
  fullscreen_button_->setToolTip(QStringLiteral("Leave full screen (Esc)"));
  // The focus was in the message field, which has just been hidden, and Qt
  // hands a hidden widget's focus to the next thing in the tab order - which
  // is the first of the call controls. Parked on the stage instead: the
  // controls only stay up while the keyboard is among them on purpose.
  room_stage_->setFocus(Qt::OtherFocusReason);
  showFullScreen();
  wake_dock();

  leave_fullscreen_action_->setEnabled(true);
  // The one thing that has to be said, said the only way left to say it: the
  // header with the button is gone, and the status bar with it.
  screen_view_->show_notice(QStringLiteral("Press Esc to leave full screen"));
}

void MainWindow::leave_fullscreen() {
  if (!fullscreen_) {
    return;
  }
  fullscreen_ = false;
  leave_fullscreen_action_->setEnabled(false);

  // The state it had, and not showNormal(): a window that was maximized when
  // full screen was entered goes back to maximized, which is what "back" means
  // to the person who maximized it.
  setWindowState(before_fullscreen_ & ~Qt::WindowFullScreen);
  statusBar()->show();
  sidebar_->show();
  room_header_->show();
  room_sidebar_->show();
  room_column_->setContentsMargins(margins_before_fullscreen_);
  room_body_->setSizes(splitter_before_fullscreen_);
  screen_view_->set_edge_to_edge(false);
  fullscreen_button_->set_icon(Icon::Fullscreen);
  fullscreen_button_->setToolTip(
      QStringLiteral("Watch the shared screen full screen. Esc, F11 or a double click on the "
                     "picture brings the room back."));
  // Back where typing lands in a room, which is where it was taken from.
  chat_input_->setFocus(Qt::OtherFocusReason);
  wake_dock();
}

void MainWindow::shake() {
  // One at a time. A second nudge inside the first's half second joins it
  // rather than restarting it, so nobody's timing can keep the window moving;
  // the server spaces them further apart than this anyway.
  if (shake_timer_->isActive()) {
    return;
  }
  // The window when it can move, and its contents when it cannot. Moving a
  // maximized window takes it out of maximized on Windows, and moving a full
  // screen one is undefined: a nudge that rearranged the desktop would be a
  // worse surprise than the nudge.
  const bool pinned = isMaximized() || isFullScreen();
  shake_target_ = pinned ? static_cast<QWidget*>(pages_) : static_cast<QWidget*>(this);
  shake_origin_ = shake_target_->pos();
  shake_step_ = 0;
  shake_timer_->start();
}

void MainWindow::animate_shake() {
  ++shake_step_;
  if (shake_step_ >= kShakeSteps) {
    shake_timer_->stop();
    shake_target_->move(shake_origin_);
    return;
  }
  // Side to side, with the swing falling to nothing by the end, so that it
  // stops rather than being stopped. A little up and down as well, at half the
  // reach and on its own rhythm, which is what keeps it from reading as the
  // window sliding.
  const double left = 1.0 - (static_cast<double>(shake_step_) / kShakeSteps);
  const int reach = static_cast<int>(std::lround(kShakeReach * left));
  const int dx = shake_step_ % 2 == 0 ? reach : -reach;
  const int dy = shake_step_ % 4 < 2 ? reach / 2 : -reach / 2;
  shake_target_->move(shake_origin_ + QPoint(dx, dy));
}

void MainWindow::apply_nudge(const QString& from_user_id, const QString& to_user_id) {
  const QString me = QString::fromStdString(session_.local_user().id);
  const QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));

  // Ours, back from the server: one line, the way a chat message we sent is
  // one line. The window that asked does not shake.
  if (from_user_id == me) {
    append_chat_line(QStringLiteral("[%1] You nudged %2").arg(when, participant_name(to_user_id)));
    return;
  }
  // The server sends this to the two people it concerns and nobody else, so
  // anything else here is a server that changed, and there is nothing to show
  // for it.
  if (to_user_id != me) {
    return;
  }

  const QString from = participant_name(from_user_id);
  append_chat_line(QStringLiteral("[%1] %2 nudged you").arg(when, from));

  // The balloon is decided first because its answer decides the sound, the
  // same rule apply_participants follows for an arrival: a balloon brings the
  // system's own sound and cannot be asked not to, and one nudge is one sound.
  // No balloon while the window is being looked at - the person is about to
  // watch it shake.
  const bool announced =
      !notifier_.window_has_attention() && notifier_.notify(from, QStringLiteral("nudged you"));
  if (!announced) {
    play_chime(Chime::Nudge);
  }

  // The operating system's own way of saying a window wants somebody: native
  // where there is one - the title bar and the taskbar button flashing until
  // this window is brought to the front - and Qt's portable version where
  // there is not. See ui/attention.hpp.
  if (!flash_window(static_cast<std::uintptr_t>(winId()))) {
    QApplication::alert(this, 0);
  }

  shake();
}

void MainWindow::build_admin_page() {
  admin_panel_ = new AdminPanel(session_, pages_);

  connect(admin_panel_, &AdminPanel::closed, this, &MainWindow::on_close_administration);
  connect(admin_panel_, &AdminPanel::failed, this, &MainWindow::apply_error);

  pages_->insertWidget(kAdminPage, admin_panel_);
}

void MainWindow::on_open_administration() {
  go_to_page(kAdminPage);
  // Asked for on the way in, never cached from last time: the accounts and the
  // rooms belong to the server and other people change them.
  admin_panel_->refresh();
}

void MainWindow::on_close_administration() {
  // Back to the call, or to the home page if the call ended while the panel
  // was up: show_page() reads that off the state, so nothing here has to
  // remember where the panel was opened from.
  show_page();
}

void MainWindow::on_participant_menu(const QPoint& where) {
  const QListWidgetItem* item = participants_->itemAt(where);
  if (item == nullptr) {
    return;
  }

  const QString user_id = item->data(Qt::UserRole).toString();
  const QString name = item->data(kNameRole).toString();
  if (user_id.isEmpty() || user_id == QString::fromStdString(session_.local_user().id)) {
    // Nothing here applies to yourself: the mute button and the leave button
    // are what those are for, the server refuses a self kick anyway, and your
    // own attention is the one thing you already have.
    return;
  }

  // Read from the session rather than from the label, so that the menu offers
  // the action that matches the state the server last reported.
  bool muted = false;
  models::Restrictions restrictions;
  for (const client::app::Participant& participant : session_.participants()) {
    if (participant.user.id == user_id.toStdString()) {
      muted = participant.muted;
      restrictions = participant.user.restrictions;
      break;
    }
  }

  QMenu menu(this);
  // For everybody: the one thing a participant may do to another participant.
  // A shaken window, a buzz and the taskbar lit up, for the person who has the
  // shared screen on another monitor and the room out of sight. See
  // apply_nudge for what arrives, and protocol::Nudge for what the server
  // allows.
  QAction* nudge = menu.addAction(QStringLiteral("Nudge %1").arg(name));

  // The rest is administration, and the menu used not to open at all for
  // anybody else. The pointers stay null for an ordinary user, and a null
  // never matches the action that was chosen.
  QAction* toggle_mute = nullptr;
  QAction* toggle_silence = nullptr;
  QAction* toggle_share_block = nullptr;
  QAction* kick = nullptr;
  if (session_.is_admin()) {
    menu.addSeparator();
    toggle_mute = menu.addAction(muted ? QStringLiteral("Unmute %1").arg(name)
                                       : QStringLiteral("Mute %1").arg(name));

    // The two below are account restrictions and outlive this room, which is
    // why they sit in their own section: "mute for now" and "may not speak
    // until somebody says otherwise" are different decisions, and a menu that
    // ran them together would make the second one easy to take by accident.
    // The administration panel is where all four are managed together; this
    // is the shortcut for the two that are usually reached for while a call
    // is going on.
    menu.addSeparator();
    toggle_silence =
        menu.addAction(restrictions.silenced ? QStringLiteral("Let %1 use the chat again").arg(name)
                                             : QStringLiteral("Silence %1 in the chat").arg(name));
    toggle_share_block =
        menu.addAction(restrictions.screen_share_blocked
                           ? QStringLiteral("Let %1 share their screen again").arg(name)
                           : QStringLiteral("Stop %1 from sharing their screen").arg(name));

    menu.addSeparator();
    kick = menu.addAction(QStringLiteral("Remove %1 from the room").arg(name));
  }

  const QAction* chosen = menu.exec(participants_->mapToGlobal(where));
  if (chosen == nullptr) {
    return;
  }

  if (chosen == nudge) {
    // Nothing is shown here. The server's copy comes back through apply_nudge
    // and writes the line, the way a chat message does; what a refusal has to
    // say - too soon, silenced - comes back as an error and is said there.
    if (const auto sent = session_.nudge(user_id.toStdString()); !sent) {
      apply_error(QString::fromStdString(sent.error().code),
                  QString::fromStdString(sent.error().message));
    }
    return;
  }

  if (chosen == toggle_mute) {
    if (const auto sent = session_.force_mute(user_id.toStdString(), !muted); !sent) {
      apply_error(QString::fromStdString(sent.error().code),
                  QString::fromStdString(sent.error().message));
    }
    return;
  }

  if (chosen == toggle_silence || chosen == toggle_share_block) {
    // One flag per request, and the other three left absent so this cannot
    // undo a restriction somebody else applied. See protocol::RestrictUser.
    protocol::RestrictUser change;
    change.user_id = user_id.toStdString();
    if (chosen == toggle_silence) {
      change.silenced = !restrictions.silenced;
    } else {
      change.screen_share_blocked = !restrictions.screen_share_blocked;
    }
    if (const auto sent = session_.restrict_user(change); !sent) {
      apply_error(QString::fromStdString(sent.error().code),
                  QString::fromStdString(sent.error().message));
    }
    return;
  }

  if (chosen == kick) {
    bool accepted = false;
    const QString reason =
        QInputDialog::getText(this, QStringLiteral("Remove participant"),
                              QStringLiteral("Reason, shown to %1 (optional)").arg(name),
                              QLineEdit::Normal, QString(), &accepted);
    if (!accepted) {
      return;
    }
    if (const auto sent = session_.kick(user_id.toStdString(), reason.toStdString()); !sent) {
      apply_error(QString::fromStdString(sent.error().code),
                  QString::fromStdString(sent.error().message));
    }
  }
}

void MainWindow::apply_chat_message(const QString& line) {
  append_chat_line(line);
}

void MainWindow::apply_chat_history(const QStringList& lines) {
  // Replaced and not appended: this arrives once per join, and a reconnection
  // that rejoins the same room would otherwise show every message twice.
  chat_view_->set_lines(lines);
  chat_view_->scrollToBottom();
}

void MainWindow::append_chat_line(const QString& line) {
  // Follow the conversation only when it was already being followed. Somebody
  // who has scrolled up to read something keeps their place, which is the
  // difference between a chat pane and one that snatches the page away every
  // time anybody says anything.
  const QScrollBar* scroll = chat_view_->verticalScrollBar();
  const bool was_at_bottom = scroll->value() >= scroll->maximum();

  chat_view_->append_line(line);
  if (was_at_bottom) {
    chat_view_->scrollToBottom();
  }

  // Said on the tab when the conversation is not the tab that is up, so that
  // looking at the list of people is not a way to miss being spoken to.
  if (side_tabs_->currentIndex() != kChatTab) {
    ++unread_chat_;
    refresh_side_tabs();
  }
}

void MainWindow::apply_kicked(const QString& reason) {
  // The session has already left the room and cleared its media by the time
  // this runs, so there is nothing to undo here: only to say what happened and
  // to go back.
  chat_view_->clear();
  chat_input_->clear();
  forget_participants();
  refresh_controls();
  show_page();

  const QString text =
      reason.isEmpty()
          ? QStringLiteral("An administrator removed you from the room.")
          : QStringLiteral("An administrator removed you from the room:\n\n%1").arg(reason);
  QMessageBox::information(this, QStringLiteral("Removed from the room"), text);
}

void MainWindow::apply_restrictions(const QString& name, const QString& by_name,
                                    const QString& summary, const QString& reason, bool is_us,
                                    bool by_us) {
  // The controls first. Whether the microphone, the share button and the chat
  // box are usable is read off the session, and the session is already in step
  // with the server by the time this runs.
  refresh_controls();

  const QString who = name.isEmpty() ? QStringLiteral("Somebody") : name;
  const QString by = by_name.isEmpty() ? QStringLiteral("an administrator") : by_name;

  if (!is_us) {
    // See apply_forced_mute for why the administrator's own copy is plain
    // news and everybody else's waits.
    notify(by_us ? ToastKind::Info : ToastKind::Moderation,
           summary.isEmpty() ? QStringLiteral("%1 lifted the restrictions on %2").arg(by, who)
                             : QStringLiteral("%1 restricted %2: %3").arg(by, who, summary));
    return;
  }

  // Being told about somebody else is a notification. Being told
  // about yourself is a window, because it is the answer to the question the
  // person is about to ask about a control that stopped working.
  if (summary.isEmpty()) {
    status_->setText(QStringLiteral("%1 lifted the restrictions on your account").arg(by));
    QMessageBox::information(this, QStringLiteral("Restrictions lifted"),
                             QStringLiteral("%1 lifted the restrictions on your account.").arg(by));
    return;
  }

  QString text =
      QStringLiteral("%1 changed what your account may do.\n\nIn force: %2").arg(by, summary);
  if (!reason.isEmpty()) {
    text += QStringLiteral("\n\nReason: %1").arg(reason);
  }
  status_->setText(QStringLiteral("%1 restricted your account: %2").arg(by, summary));
  QMessageBox::information(this, QStringLiteral("Your account was restricted"), text);
}

void MainWindow::apply_notice(const QString& notice_id, const QString& from, const QString& text) {
  // One button, and it means what it says. Dismissing the box any way at all
  // counts as having read it, because the distinction between pressing OK and
  // pressing Escape on a box somebody has just looked at is not one anybody
  // could act on. What is not treated as read is the box never being answered:
  // closing the client with it open, or losing the connection before the
  // answer goes out, both leave the notice outstanding and the server hands it
  // over again at the next sign-in. That is the failure this feature has to
  // survive, and it survives it by doing nothing.
  QMessageBox box(this);
  box.setIcon(QMessageBox::Information);
  box.setWindowTitle(QStringLiteral("Message from %1").arg(from));
  box.setText(QStringLiteral("%1 sent you a message:").arg(from));
  box.setInformativeText(text);
  // Plain text, always. A notice is somebody's typing, and Qt reads a string
  // containing tags as rich text unless it is told otherwise - which would let
  // an administrator's message carry markup, and a message carry a link.
  box.setTextFormat(Qt::PlainText);
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();

  status_->setText(QStringLiteral("Message from %1 marked as read").arg(from));
  const auto sent = session_.acknowledge_notice(notice_id.toStdString());
  if (!sent) {
    // Reported, and nothing else to do: the notice is still outstanding on the
    // server, so it arrives again next time rather than being lost.
    apply_error(QString::fromStdString(sent.error().code),
                QString::fromStdString(sent.error().message));
  }
}

void MainWindow::apply_notice_sent(const QString& user_id) {
  // On the interface thread, which is what makes the panel's account list safe
  // to read here. See the callback that queued this.
  const QString to = admin_panel_ != nullptr ? admin_panel_->label_for(user_id) : user_id;
  notify(ToastKind::Success, QStringLiteral("Message delivered to %1").arg(to));
}

void MainWindow::apply_forced_mute(const QString& name, const QString& by_name, bool muted,
                                   bool by_us) {
  refresh_controls();

  const QString who = name.isEmpty() ? QStringLiteral("Somebody") : name;
  const QString by = by_name.isEmpty() ? QStringLiteral("an administrator") : by_name;
  // A notification of the kind that waits to be dismissed. It used to be a
  // line in the status bar, which nobody watching a shared screen was looking
  // at; and it is not allowed to come and go in four seconds either, because
  // a voice that stops with no explanation on screen reads as a fault in the
  // call. See toast_lifetime_ms.
  //
  // Except on the window of whoever did it. There it is not news, it is the
  // answer to a click made a moment ago, and an administrator working down a
  // list would be dismissing one of these for every person on it. Theirs
  // leaves by itself, like any other confirmation.
  notify(by_us ? ToastKind::Info : ToastKind::Moderation,
         muted ? QStringLiteral("%1 was muted by %2").arg(who, by)
               : QStringLiteral("%1 was unmuted by %2").arg(who, by));
}

void MainWindow::refresh_room_title() {
  const QString id = QString::fromStdString(session_.room_id());
  if (id.isEmpty()) {
    return;
  }
  const QString name = room_names_.value(id);

  // A room nobody named carries its own identifier as its name, so showing
  // both would read "8F42A1 (8F42A1)". The same line covers a name we have not
  // been told yet, which is what the first moments after a join look like.
  room_header_->set_title(name.isEmpty() || name == id ? QStringLiteral("Room %1").arg(id) : name);
  // The code on its own, on the button that copies it: what is on the button
  // is exactly what lands on the clipboard.
  copy_room_button_->setText(id);
  // How many are in and how many fit, from the same list. The list is
  // broadcast on every arrival, so this is as current as the People tab, and
  // it is the number to read out to somebody asking whether they can still
  // join.
  const QString people = room_people_.value(id);
  room_people_label_->setText(people.isEmpty() ? QString{}
                                               : QStringLiteral("%1 people").arg(people));
}

void MainWindow::show_page() {
  switch (state_) {
    // Until there is a call, the page depends only on whether anyone is signed
    // in: connecting and authenticated look the same to a person waiting.
    case client::app::CallSession::State::Idle:
    case client::app::CallSession::State::Failed:
    case client::app::CallSession::State::Connecting:
    case client::app::CallSession::State::Authenticated:
      go_to_page(session_.local_user().id.empty() ? kLoginPage : kHomePage);
      break;
    case client::app::CallSession::State::InCall:
      go_to_page(kRoomPage);
      refresh_room_title();
      break;
  }
}

void MainWindow::go_to_page(int index) {
  // Nothing to cross to. show_page() is called from every state change, and
  // most of them land on the page that is already up; fading that in would be
  // the interface blinking at news that changed nothing.
  if (pages_->currentIndex() == index) {
    return;
  }
  // Full screen belongs to the room page. Leaving it for any other - the
  // administration panel, the home screen after being removed - puts the
  // window back first, or the next page would come up with no status bar and
  // no title bar and no way to tell why.
  if (index != kRoomPage) {
    leave_fullscreen();
  }
  pages_->setCurrentIndex(index);

  // The rail belongs to somebody who is signed in: on the sign-in page there
  // is nowhere to go and nobody to show a badge for. And it says where the
  // window is now - told here, the one place every change of page passes
  // through, rather than by the rail's own buttons when they are pressed.
  sidebar_->setVisible(index != kLoginPage && !fullscreen_);
  sidebar_->set_current(index == kAdminPage ? kAdminDestination : kRoomsDestination);

  // Arriving at the home page asks for the list, which covers both ways of
  // getting here: signing in, and coming back out of a call. The server pushes
  // it after that, whenever a room appears or is closed, so this is the only
  // place that has to ask.
  if (index == kHomePage) {
    (void)session_.list_rooms();
    // And the box starts empty. It doubles as the filter over the cards, and
    // the code of the room that was just left would otherwise still be in it,
    // narrowing the lobby to the one room somebody has just walked out of.
    room_id_->clear();
    // And it is where typing lands. Coming out of a room, the focus was in a
    // message field that has just been hidden, and Qt hands it to whatever
    // is next in the tab order - which can be a notification still waiting
    // to be dismissed, one press of the space bar from being sent away
    // unread.
    room_id_->setFocus();
  }
  if (index == kRoomPage) {
    // A room is entered on the conversation, with nothing marked unread: the
    // history that arrives with the join is not news.
    unread_chat_ = 0;
    refresh_side_tabs();
    wake_dock();
    // Where typing should land in a room is the conversation. Left to Qt the
    // focus goes to the first thing in the tab order, which is the button
    // that copies the room code - lit up as though it were the point of the
    // page, and one press of the space bar from doing something.
    chat_input_->setFocus();
  }

  // A notification that waits to be dismissed is still up when the page under
  // it is replaced, and the page that arrives is drawn after it: what was left
  // of an administrator's notice from the room was its top edge, showing
  // between the lobby's header and the cards that covered the rest. Put back
  // on top here, and again once the fade below has finished with the page.
  toasts_->bring_to_front();

  // A release that was announced during a call has been waiting for this:
  // any page that is not the room is a moment to ask. Shortly after, so that
  // the page somebody was sent to is seen before the dialog over it.
  if (update_pending_ && index != kRoomPage) {
    QTimer::singleShot(kUpdateSettleMs, this, &MainWindow::offer_update);
  }

  QWidget* page = pages_->widget(index);
  // With animations off the page is simply there, which the stack has already
  // seen to: no effect is made, so none has to be taken off again.
  const int duration = theme::motion::duration(kPageFadeMs);
  if (page == nullptr || duration == 0) {
    return;
  }

  auto* fade = new QGraphicsOpacityEffect(page);
  fade->setOpacity(0.0);
  page->setGraphicsEffect(fade);

  auto* animation = new QPropertyAnimation(fade, "opacity", page);
  animation->setDuration(duration);
  animation->setStartValue(0.0);
  animation->setEndValue(1.0);
  animation->setEasingCurve(QEasingCurve::OutCubic);

  // Taken off again when it is over, and never left in place. A graphics
  // effect makes Qt render the whole page into an offscreen pixmap on every
  // repaint, and the room page repaints thirty times a second for as long as
  // somebody is sharing a screen.
  //
  // Queued, so the effect is deleted on the next turn of the event loop rather
  // than from inside the signal of the animation that is driving it.
  connect(
      animation, &QPropertyAnimation::finished, this,
      [this, page] {
        page->setGraphicsEffect(nullptr);
        // The effect drew the page from an offscreen copy of itself, over
        // whatever was in front of it.
        toasts_->bring_to_front();
      },
      Qt::QueuedConnection);

  animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::refresh_controls() {
  const bool authenticated = state_ == client::app::CallSession::State::Authenticated ||
                             state_ == client::app::CallSession::State::InCall;
  const bool in_call = state_ == client::app::CallSession::State::InCall;

  connect_button_->setEnabled(!authenticated);
  username_->setEnabled(!authenticated);
  password_->setEnabled(!authenticated);
  // The server goes with them: a session is on one, and CallSession::
  // retest_server refuses to close the socket carrying it.
  server_->setEnabled(!authenticated);
  test_server_button_->setEnabled(!authenticated);
  create_button_->setEnabled(authenticated);
  join_button_->setEnabled(authenticated);
  // The server is what actually decides, and it re-reads the role on every
  // administrative message. This only decides whether the door is visible.
  // The door is on the rail, so it is there from the list of rooms as well as
  // from inside one: closing a room nobody is in should not need walking into
  // another first.
  admin_button_->setVisible(authenticated && session_.is_admin());

  // What an administrator has taken away from this account, which the session
  // keeps in step with the server. The server refuses each of these anyway;
  // what these three do is say so before somebody presses a control and is
  // told no, and explain the refusal in the tooltip rather than in an error
  // box that appears a second later.
  const models::Restrictions& restrictions = session_.local_user().restrictions;

  {
    // Reporting the session's state, not asking for a change. See the
    // `toggled` connection: without the blocker this would call
    // on_toggle_mute, which would call refresh_controls again.
    const QSignalBlocker blocker(mute_button_);
    mute_button_->setChecked(session_.muted());
  }
  // The button is an icon, so its name is what the tooltip and a screen
  // reader say - and the icon itself changes with the state: a microphone,
  // or a microphone struck through on amber.
  const QString mute_name =
      session_.muted() ? QStringLiteral("Unmute microphone") : QStringLiteral("Mute microphone");
  mute_button_->setAccessibleName(mute_name);
  // A forced mute is not a button that turned itself off: it is one that will
  // not turn back on. Disabled rather than left clickable and refused, which
  // would leave the microphone looking like it had failed.
  //
  // And nothing to mute outside a call: a room is always entered with the
  // microphone open (CallSession::set_muted says why), so a button that could
  // be pressed in the lobby would promise a mute the join throws away.
  mute_button_->setEnabled(in_call && !restrictions.muted);
  mute_button_->setToolTip(
      restrictions.muted ? QStringLiteral("An administrator has muted this account.") : mute_name);

  const bool sharing = session_.sharing_screen();
  const std::string sharer = session_.screen_sharer();
  const bool someone_else_is_sharing = !sharer.empty() && sharer != session_.local_user().id;

  // Section 5.2 of SPEC.md allows one screen at a time, and the server refuses
  // a second one. Saying so with a disabled button beats letting the click
  // through and answering with an error.
  //
  // Stopping is never taken away, so a share still running when the block
  // lands can be ended from here as well as by the server.
  const bool may_share = !restrictions.screen_share_blocked || sharing;
  share_button_->setEnabled(in_call && !someone_else_is_sharing && may_share);
  {
    const QSignalBlocker blocker(share_button_);
    share_button_->setChecked(sharing);
  }
  share_button_->set_label(sharing ? QStringLiteral("Stop sharing")
                                   : QStringLiteral("Share screen"));
  share_button_->setToolTip(
      restrictions.screen_share_blocked
          ? QStringLiteral("An administrator has blocked screen sharing for this account.")
          : QString());

  // Only while there is somebody else's picture to fill the screen with.
  // Hidden rather than disabled, beside a sentence that is itself empty when
  // nobody is sharing: a greyed "Full screen" next to nothing is a button
  // about nothing.
  fullscreen_button_->setVisible(in_call && someone_else_is_sharing);
  // And out of full screen the moment there is not, whichever way that came
  // about: the share ending, the sharer leaving, this side being removed, the
  // call dropping. Every one of those reaches here. A black screen with
  // nothing on it and no visible way out is not a state to leave anybody in.
  if (fullscreen_ && !(in_call && someone_else_is_sharing)) {
    leave_fullscreen();
  }

  // There is nobody to say anything to outside a room, and a field that
  // accepts a line it cannot send is a field that loses it. Being silenced is
  // the same problem with a different cause and gets the same answer.
  const bool may_speak = in_call && !restrictions.silenced;
  chat_input_->setEnabled(may_speak);
  chat_emoji_->setEnabled(may_speak);
  chat_send_->setEnabled(may_speak);
  chat_input_->setPlaceholderText(restrictions.silenced
                                      ? QStringLiteral("An administrator has silenced you")
                                      : QStringLiteral("Message"));
  // The whole sentence for whoever hovers: the field is as narrow as the
  // panel, and a placeholder that does not fit is cut at "silenc...".
  chat_input_->setToolTip(restrictions.silenced
                              ? QStringLiteral("An administrator has silenced you in the chat.")
                              : QString());

  // Last, because everything above can change how wide the controls are - a
  // label, a button appearing - and whether there is a picture for them to
  // get out of the way of.
  place_dock(false);
  wake_dock();
}

}  // namespace dv::ui
