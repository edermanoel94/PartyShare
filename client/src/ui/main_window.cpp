#include "ui/main_window.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <dv/logging/logger.hpp>
#include <dv/models/chat.hpp>

#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QEasingCurve>
#include <QFont>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
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
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "app/smoothing.hpp"
#include "media/media_session.hpp"
#include "ui/admin_panel.hpp"
#include "ui/metrics_dialog.hpp"
#include "ui/screen_view.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/table.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The pages of the stack, in the order section 19 of SPEC.md walks through
/// them.
constexpr int kLoginPage = 0;
constexpr int kHomePage = 1;
constexpr int kRoomPage = 2;
constexpr int kAdminPage = 3;

/// Where the participant's plain name is kept on a list item, next to the
/// user id in Qt::UserRole. The visible text carries the state as well.
constexpr int kNameRole = Qt::UserRole + 1;

/// "Nothing is being measured" as a value the indicator can remember having
/// drawn. Outside the NetworkQuality range on purpose, so it can never be
/// mistaken for one of the three verdicts.
constexpr int kNoLinkQuality = -2;

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

/// How many emoji the picker puts on a row. Eight of them at 34 pixels plus
/// the spacing is about 290 wide, which fits under a sidebar that is 260 to
/// 320 and does not hang off the side of the window.
constexpr int kEmojiColumns = 8;

/// What the picker offers.
///
/// Forty, chosen and not generated. The system picker already does the whole
/// of Unicode with a search box, skin tones and a list of recents, and it will
/// know about emoji that do not exist yet; competing with it would be a worse
/// copy that goes out of date. What it cannot do is put the handful somebody
/// reaches for during a call one click away, and that is what this is.
///
/// Ctrl+Cmd+Space on macOS and Win+. on Windows open the system one in this
/// same field, so nothing here is a ceiling on what can be sent.
[[nodiscard]] const QStringList& quick_emoji() {
  // A function local static: a QStringList has a non-trivial constructor and
  // cannot be constexpr, which is the case client/src/ui/.clang-tidy's naming
  // rules call out.
  // clang-format off
  //
  // Laid out eight to a line because that is how many go on a row of the
  // picker, so the source has the shape of the thing on screen, and written as
  // the characters themselves so that what is on offer can be read here rather
  // than decoded. clang-format measures a line in bytes and an emoji is four
  // of them, so left alone it reflows this to one per line and the grid goes.
  static const QStringList kQuickEmoji = {
      // Answers, which is most of what a chat during a call is for.
      QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👌"), QStringLiteral("🙌"),
      QStringLiteral("👏"), QStringLiteral("🙏"), QStringLiteral("💪"), QStringLiteral("🤝"),
      // Faces.
      QStringLiteral("😀"), QStringLiteral("😄"), QStringLiteral("😅"), QStringLiteral("😂"),
      QStringLiteral("🙂"), QStringLiteral("😉"), QStringLiteral("😍"), QStringLiteral("🤔"),
      QStringLiteral("😐"), QStringLiteral("😴"), QStringLiteral("😭"), QStringLiteral("😱"),
      QStringLiteral("😡"), QStringLiteral("🤯"), QStringLiteral("🤦"), QStringLiteral("🤷"),
      // How it went.
      QStringLiteral("🎉"), QStringLiteral("🎊"), QStringLiteral("🔥"), QStringLiteral("⭐"),
      QStringLiteral("✨"), QStringLiteral("💡"), QStringLiteral("✅"), QStringLiteral("❌"),
      // The work itself.
      QStringLiteral("🚀"), QStringLiteral("🐛"), QStringLiteral("🔧"), QStringLiteral("📌"),
      QStringLiteral("⏰"), QStringLiteral("☕"), QStringLiteral("👀"), QStringLiteral("❤️"),
  };
  // clang-format on

  return kQuickEmoji;
}

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
      {QStringLiteral("unauthorized"), QStringLiteral("Wrong username or password.")},
      {QStringLiteral("not_connected"),
       QStringLiteral("No connection to the server. Trying again.")},
      {QStringLiteral("capture_denied"),
       QStringLiteral("Permission to capture the screen was denied.")},
      {QStringLiteral("capture_unavailable"),
       QStringLiteral("This system has no way to capture the screen.")},
      {QStringLiteral("capture_failed"),
       QStringLiteral("Screen capture stopped. The monitor may have been disconnected.")},
      {QStringLiteral("monitor_not_found"), QStringLiteral("That monitor no longer exists.")},
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
      pages_(new QStackedWidget(this)),
      level_timer_(new QTimer(this)) {
  setWindowTitle(QStringLiteral("PartyShare"));
  setMinimumSize(720, 560);
  resize(960, 760);

  setCentralWidget(pages_);

  build_login_page();
  build_home_page();
  build_room_page();
  build_admin_page();

  status_ = new QLabel(QStringLiteral("disconnected"), this);
  quality_ = new QLabel(QString{}, this);
  metrics_ = new QLabel(QString{}, this);
  QFont small = metrics_->font();
  small.setPointSize(small.pointSize() - 1);
  metrics_->setFont(small);
  metrics_->setProperty("hint", true);

  statusBar()->addWidget(status_);
  statusBar()->addWidget(quality_);
  statusBar()->addPermanentWidget(metrics_);

  // Runs only while the meter has somewhere to go. A timer beating sixty times
  // a second for the whole time a window is open, to redraw a bar that is not
  // moving, is a program that never lets a laptop idle.
  level_timer_->setInterval(kLevelFrameMs);
  connect(level_timer_, &QTimer::timeout, this, &MainWindow::animate_level);
  level_clock_.start();

  wire_session();
  refresh_controls();
  show_page();
}

MainWindow::~MainWindow() = default;

void MainWindow::show_login_error(const QString& text) {
  login_error_->setText(text);
  login_error_->setVisible(!text.isEmpty());
}

void MainWindow::build_login_page() {
  auto* page = new QWidget(pages_);
  auto* outer = new QVBoxLayout(page);
  outer->addStretch();

  auto* box = new QGroupBox(QStringLiteral("Sign in"), page);
  box->setMaximumWidth(420);
  auto* form = new QFormLayout(box);

  username_ = new QLineEdit(box);
  username_->setPlaceholderText(QStringLiteral("username"));
  password_ = new QLineEdit(box);
  password_->setEchoMode(QLineEdit::Password);
  password_->setPlaceholderText(QStringLiteral("password"));
  connect_button_ = new QPushButton(QStringLiteral("Connect"), box);
  connect_button_->setDefault(true);
  connect_button_->setProperty("accent", true);
  connect_button_->setMinimumHeight(40);

  login_error_ = new QLabel(QString{}, box);
  login_error_->setWordWrap(true);
  // The colour comes from the theme rather than from a literal here. It used to
  // be a hard coded #c62828, which is legible on a light window and very nearly
  // invisible on a dark one.
  login_error_->setProperty("error", true);
  login_error_->setVisible(false);

  // Settings is reachable from here and not only from inside a room, and the
  // reason is the one setting that lives in this dialog: the server address.
  // Every other setting can wait until somebody is signed in, because it is
  // about this machine. The address is what signing in depends on, so a
  // dialog that opens only after a successful sign-in is a dialog nobody can
  // reach at the one moment they need it - a wrong address in config.ini used
  // to leave editing the file by hand as the only way back.
  auto* login_settings = new QPushButton(QStringLiteral("Settings"), box);
  login_settings->setMinimumHeight(40);

  auto* actions = new QHBoxLayout();
  actions->addWidget(connect_button_, 1);
  actions->addWidget(login_settings);

  form->addRow(QStringLiteral("Username"), username_);
  form->addRow(QStringLiteral("Password"), password_);
  form->addRow(actions);
  form->addRow(login_error_);

  auto* centred = new QHBoxLayout();
  centred->addStretch();
  centred->addWidget(box);
  centred->addStretch();
  outer->addLayout(centred);
  outer->addStretch();

  connect(connect_button_, &QPushButton::clicked, this, &MainWindow::on_connect);
  connect(login_settings, &QPushButton::clicked, this, &MainWindow::on_open_settings);
  connect(username_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  connect(password_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);

  pages_->insertWidget(kLoginPage, page);
}

void MainWindow::build_home_page() {
  auto* page = new QWidget(pages_);
  auto* outer = new QVBoxLayout(page);
  outer->addStretch();

  welcome_ = new QLabel(QString{}, page);
  QFont big = welcome_->font();
  big.setPointSize(big.pointSize() + 4);
  welcome_->setFont(big);
  welcome_->setAlignment(Qt::AlignCenter);

  auto* box = new QWidget(page);
  // Wider than it was, because a list of rooms needs room to be a list. The
  // page held two buttons and a six character field before, and 420 was
  // generous for that.
  box->setMaximumWidth(560);
  auto* column = new QVBoxLayout(box);

  // Somebody arriving here used to need a code before they could go anywhere:
  // the only way to learn that a room existed was to be told its six
  // characters. Rooms outlive their participants now, so there is a list to
  // show, and this is where it belongs.
  auto* rooms_label = new QLabel(QStringLiteral("Rooms"), box);
  QFont label_font = rooms_label->font();
  label_font.setBold(true);
  rooms_label->setFont(label_font);

  // The same column order as the administrator's tab, because both are filled
  // from the same row: identifier, then name, then how many are inside. The
  // code is a column rather than something hidden because it is what somebody
  // reads out to invite a person who is not looking at this list.
  room_list_ =
      make_table({QStringLiteral("Room"), QStringLiteral("Name"), QStringLiteral("People")}, box);
  room_list_->setMinimumHeight(160);

  // A table with no rows is indistinguishable from a table that failed to
  // load, so the empty case says which one it is.
  rooms_empty_ = new QLabel(QStringLiteral("No rooms yet. Create the first one."), box);
  rooms_empty_->setAlignment(Qt::AlignCenter);
  rooms_empty_->setProperty("hint", true);

  create_button_ = new QPushButton(QStringLiteral("Create room"), box);
  create_button_->setMinimumHeight(44);
  // The one filled button on this page. Creating and joining are both things
  // somebody came here to do, but only one of them works without first having
  // been sent a code, so that is the one the eye should land on.
  create_button_->setProperty("accent", true);

  room_id_ = new QLineEdit(box);
  room_id_->setPlaceholderText(QStringLiteral("Room ID, for example 8F42A1"));
  room_id_->setAlignment(Qt::AlignCenter);
  room_id_->setMinimumHeight(44);

  join_button_ = new QPushButton(QStringLiteral("Join room"), box);
  join_button_->setMinimumHeight(44);

  // Hidden rather than disabled until somebody signs in as an administrator.
  // A greyed out button that most people can never use is a permanent
  // reminder of a feature that is not theirs.
  admin_button_ = new QPushButton(QStringLiteral("Administration"), box);
  admin_button_->setMinimumHeight(36);
  admin_button_->setVisible(false);

  // The way back to the login screen, and the reason it exists is the server
  // address in the settings dialog. A new address is adopted at the next
  // sign-in, and without a way to sign out, "the next sign-in" would mean
  // closing and reopening the program - which is the thing the setting was
  // added to avoid. It is also the only way to hand the machine to somebody
  // else without doing that.
  auto* sign_out = new QPushButton(QStringLiteral("Sign out"), box);
  sign_out->setMinimumHeight(36);

  column->addWidget(rooms_label);
  column->addWidget(room_list_, 1);
  column->addWidget(rooms_empty_);
  column->addSpacing(12);
  column->addWidget(create_button_);
  column->addSpacing(16);
  // The field stays. A list is how you find a room nobody told you about; a
  // code is still how somebody hands you one particular room, and that is not
  // the same errand.
  column->addWidget(room_id_);
  column->addWidget(join_button_);
  column->addSpacing(16);
  column->addWidget(admin_button_);
  column->addWidget(sign_out);

  auto* centred = new QHBoxLayout();
  centred->addStretch();
  centred->addWidget(box);
  centred->addStretch();

  outer->addWidget(welcome_);
  outer->addSpacing(24);
  outer->addLayout(centred);
  outer->addStretch();

  // Selecting writes the code into the field rather than joining, so that the
  // row you are reading and the room you are about to enter are the same one
  // even if the list is refreshed underneath you. Entering is the deliberate
  // second act: a double click, or the button below.
  connect(room_list_, &QTableWidget::itemSelectionChanged, this, [this] {
    const QString id = selected_id(room_list_);
    if (!id.isEmpty()) {
      room_id_->setText(id);
    }
  });
  connect(room_list_, &QTableWidget::itemDoubleClicked, this, [this](const QTableWidgetItem* item) {
    if (item == nullptr) {
      return;
    }
    const QString id = selected_id(room_list_);
    if (!id.isEmpty()) {
      room_id_->setText(id);
      on_join_room();
    }
  });

  connect(create_button_, &QPushButton::clicked, this, &MainWindow::on_create_room);
  connect(join_button_, &QPushButton::clicked, this, &MainWindow::on_join_room);
  connect(room_id_, &QLineEdit::returnPressed, this, &MainWindow::on_join_room);
  connect(admin_button_, &QPushButton::clicked, this, &MainWindow::on_open_administration);
  connect(sign_out, &QPushButton::clicked, this, &MainWindow::on_sign_out);

  pages_->insertWidget(kHomePage, page);
}

void MainWindow::build_room_page() {
  auto* page = new QWidget(pages_);
  auto* column = new QVBoxLayout(page);

  auto* header = new QHBoxLayout();
  room_title_ = new QLabel(QString{}, page);
  QFont bold = room_title_->font();
  bold.setBold(true);
  room_title_->setFont(bold);
  // The only way to read the room code back out from inside a call. The home
  // page shows it in a QLineEdit, which a person can select and copy; this is a
  // QLabel, and a label copies nothing. Inviting somebody is exactly the thing
  // you do from in here, so the code has to be reachable from in here.
  copy_room_button_ = new QPushButton(QStringLiteral("Copy"), page);
  copy_room_button_->setToolTip(QStringLiteral("Copy the room code to the clipboard"));
  connect(copy_room_button_, &QPushButton::clicked, this, &MainWindow::on_copy_room_id);

  sharing_label_ = new QLabel(QString{}, page);
  sharing_label_->setProperty("accent", true);
  header->addWidget(room_title_);
  header->addWidget(copy_room_button_);
  header->addStretch();
  header->addWidget(sharing_label_);

  screen_view_ = new ScreenView(page);

  auto* people = new QGroupBox(QStringLiteral("Participants"), page);
  auto* people_column = new QVBoxLayout(people);
  participants_ = new QListWidget(people);
  participants_->setMinimumHeight(120);
  participants_->setMaximumHeight(180);
  // The menu is built on demand and is empty for anybody who is not an
  // administrator, so there is nothing to enable or disable here.
  participants_->setContextMenuPolicy(Qt::CustomContextMenu);

  microphone_level_ = new QProgressBar(people);
  microphone_level_->setRange(0, kMeterSteps);
  microphone_level_->setTextVisible(false);
  microphone_level_->setFixedHeight(8);

  // Stacked and not side by side. The label is a sentence and the sidebar is
  // 240 pixels wide at its narrowest, so sharing a row left the slider about
  // ninety of them: a control too small to aim at, beside a sentence too long
  // to finish.
  auto* volume_column = new QVBoxLayout();
  volume_column->setSpacing(4);
  volume_label_ = new QLabel(QStringLiteral("Volume: select a participant"), people);
  volume_label_->setProperty("hint", true);
  volume_ = new QSlider(Qt::Horizontal, people);
  // 0 to 200 percent: above 100 is amplification, which WebRTC allows.
  volume_->setRange(0, 200);
  volume_->setValue(100);
  volume_->setEnabled(false);
  volume_column->addWidget(volume_label_);
  volume_column->addWidget(volume_);

  people_column->addWidget(microphone_level_);
  people_column->addWidget(participants_);
  people_column->addLayout(volume_column);

  auto* chat = new QGroupBox(QStringLiteral("Chat"), page);
  auto* chat_column = new QVBoxLayout(chat);
  chat_view_ = new QListWidget(chat);
  // Wrapped, and with no alternating rows: a message is a paragraph rather
  // than a cell, and a long one has to be readable without a horizontal
  // scrollbar under it.
  chat_view_->setWordWrap(true);
  chat_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Selectable, because copying what somebody pasted into the room is half of
  // what a chat in a screen sharing call is for.
  chat_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  chat_view_->setTextElideMode(Qt::ElideNone);

  auto* chat_row = new QHBoxLayout();
  chat_input_ = new QLineEdit(chat);
  chat_input_->setPlaceholderText(QStringLiteral("Message"));
  // A coarse guard and not the rule. The limit is bytes, and this counts
  // characters, so a line of accented text can still be refused by the server
  // at a length this field allowed. It is here to stop a paste of a whole
  // document dead rather than to decide anything: send_chat checks the real
  // limit, and what it answers reaches the same place every other error does.
  chat_input_->setMaxLength(static_cast<int>(models::kMaxChatTextBytes));

  chat_emoji_ = new QPushButton(QStringLiteral("🙂"), chat);
  chat_emoji_->setToolTip(QStringLiteral("Insert an emoji"));
  // Square and small: it sits in a sidebar that is 260 pixels wide at its
  // narrowest, and every pixel it takes is one the message field does not get.
  chat_emoji_->setFixedWidth(34);
  // The stylesheet gives every button 18 pixels of side padding, which on a
  // button pinned to 34 wide leaves nothing for the emoji itself.
  chat_emoji_->setStyleSheet(QStringLiteral("padding: 0px;"));

  chat_send_ = new QPushButton(QStringLiteral("Send"), chat);
  chat_send_->setProperty("accent", true);
  chat_row->addWidget(chat_input_, 1);
  chat_row->addWidget(chat_emoji_);
  chat_row->addWidget(chat_send_);

  chat_column->addWidget(chat_view_, 1);
  chat_column->addLayout(chat_row);

  // A splitter and not a fixed division, because how much room the conversation
  // deserves is not something this code knows. Somebody reading a long paste
  // wants it wide; somebody watching a screen share wants it out of the way.
  //
  // It also fixes a real problem with the fixed version: the sidebar was capped
  // at 320, so the message field stayed about 150 pixels wide no matter how
  // large the window got.
  auto* sidebar_widget = new QWidget(page);
  auto* sidebar = new QVBoxLayout(sidebar_widget);
  sidebar->setContentsMargins(0, 0, 0, 0);
  sidebar->addWidget(people);
  sidebar->addWidget(chat, 1);

  auto* body = new QSplitter(Qt::Horizontal, page);
  body->addWidget(screen_view_);
  body->addWidget(sidebar_widget);

  // Neither side may be dragged out of existence. A collapsed pane leaves a
  // handle at the very edge of the window as the only way back, which is a
  // thing to discover rather than a thing to see, and collapsing the sidebar
  // takes the participants and the chat with it.
  body->setChildrenCollapsible(false);
  screen_view_->setMinimumWidth(320);
  sidebar_widget->setMinimumWidth(240);

  // Growing the window widens the screen, not the sidebar: the sidebar holds
  // names and lines of text, and neither reads better for being stretched.
  // Dragging still overrides this, which is the point of the splitter.
  body->setStretchFactor(0, 1);
  body->setStretchFactor(1, 0);
  body->setSizes({640, 300});

  auto* controls = new QHBoxLayout();
  mute_button_ = new QPushButton(QStringLiteral("Mute microphone"), page);
  mute_button_->setCheckable(true);
  share_button_ = new QPushButton(QStringLiteral("Share screen"), page);
  share_button_->setCheckable(true);
  settings_button_ = new QPushButton(QStringLiteral("Settings"), page);
  network_status_button_ = new QPushButton(QStringLiteral("Network status"), page);
  network_status_button_->setToolTip(
      QStringLiteral("What the call is carrying, and the three measurements behind the network "
                     "indicator in the status bar."));
  leave_button_ = new QPushButton(QStringLiteral("Leave"), page);
  for (QPushButton* button :
       {mute_button_, share_button_, settings_button_, network_status_button_, leave_button_}) {
    button->setMinimumHeight(38);
  }
  // What each toggle looks like when it is on. Sharing is the good state and
  // wears the accent; a muted microphone is a warning and wears amber.
  share_button_->setProperty("toggle", QStringLiteral("accent"));
  mute_button_->setProperty("toggle", QStringLiteral("warn"));
  leave_button_->setProperty("danger", true);
  controls->addWidget(mute_button_);
  controls->addWidget(share_button_);
  controls->addWidget(settings_button_);
  controls->addWidget(network_status_button_);
  controls->addStretch();
  controls->addWidget(leave_button_);

  column->addLayout(header);
  column->addWidget(body, 1);
  column->addLayout(controls);

  connect(participants_, &QListWidget::customContextMenuRequested, this,
          &MainWindow::on_participant_menu);
  connect(chat_send_, &QPushButton::clicked, this, &MainWindow::on_send_chat);
  connect(chat_emoji_, &QPushButton::clicked, this, &MainWindow::on_open_emoji_picker);
  // Return sends, which is what everybody expects of a field next to a Send
  // button and what nobody wants to reach for the mouse for.
  connect(chat_input_, &QLineEdit::returnPressed, this, &MainWindow::on_send_chat);
  connect(mute_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_mute);
  connect(share_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_share);
  connect(settings_button_, &QPushButton::clicked, this, &MainWindow::on_open_settings);
  connect(network_status_button_, &QPushButton::clicked, this, &MainWindow::on_open_metrics);
  connect(leave_button_, &QPushButton::clicked, this, &MainWindow::on_leave_room);
  connect(participants_, &QListWidget::itemSelectionChanged, this,
          &MainWindow::on_participant_selected);
  connect(volume_, &QSlider::valueChanged, this, &MainWindow::on_volume_changed);

  pages_->insertWidget(kRoomPage, page);
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
            QMetaObject::invokeMethod(
                this, "apply_link", Qt::QueuedConnection,
                Q_ARG(int, link.round_trip ? static_cast<int>(link.round_trip->count()) : -1));
          },
      .on_participants =
          [this](const std::vector<client::app::Participant>& list) {
            QStringList names;
            names.reserve(static_cast<qsizetype>(list.size()));
            for (const client::app::Participant& participant : list) {
              // The id and the bare name travel after tabs, so that the volume
              // slider knows whose volume it is changing and can say the name
              // without the state that got appended to it. The list only shows
              // what comes before the first tab.
              const QString name = QString::fromStdString(participant.user.display_name.empty()
                                                              ? participant.user.id
                                                              : participant.user.display_name);
              QString label = name;
              if (participant.muted) {
                label += QStringLiteral("  (muted)");
              } else if (participant.speaking) {
                label += QStringLiteral("  (speaking)");
              } else if (participant.audio_active) {
                label += QStringLiteral("  (connected)");
              }
              if (participant.sharing_screen) {
                // Two words apart, because they answer different questions:
                // whose picture is on screen, and why this person's volume
                // slider is now also the volume of a film.
                label += participant.sharing_audio ? QStringLiteral("  (sharing with sound)")
                                                   : QStringLiteral("  (sharing)");
              }
              label += QStringLiteral("\t") + QString::fromStdString(participant.user.id);
              label += QStringLiteral("\t") + name;
              names.push_back(label);
            }
            QMetaObject::invokeMethod(this, "apply_participants", Qt::QueuedConnection,
                                      Q_ARG(QStringList, names));
          },
      .on_metrics =
          [this](client::media::AudioStats stats) {
            QString summary =
                QStringLiteral("rtt %1 ms · jitter %2 ms · lost %3 · %4 kbps ↑ · %5 kbps ↓")
                    .arg(stats.round_trip_time_ms, 0, 'f', 0)
                    .arg(stats.jitter_ms, 0, 'f', 1)
                    .arg(stats.packets_lost)
                    .arg(stats.send_bitrate_kbps, 0, 'f', 0)
                    .arg(stats.receive_bitrate_kbps, 0, 'f', 0);

            // The screen share is the part of the call that gives way when the
            // network does, so while there is one its rate is worth seeing:
            // a picture that went soft and a number that fell are the same
            // event, and only one of them is legible.
            const client::media::VideoStats video = session_.video_stats();
            if (video.frames_sent > 0) {
              summary +=
                  QStringLiteral(" · screen %1 kbps ↑").arg(video.send_bitrate_kbps, 0, 'f', 0);
            } else if (video.frames_received > 0) {
              summary +=
                  QStringLiteral(" · screen %1 kbps ↓").arg(video.receive_bitrate_kbps, 0, 'f', 0);
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
      .on_user_list =
          [this](const std::vector<protocol::UserSummary>& users) {
            QStringList rows;
            rows.reserve(static_cast<qsizetype>(users.size()));
            for (const protocol::UserSummary& summary : users) {
              // Identifier first, then one field per column. Tab separated for
              // the same reason the participant list is: it needs no
              // registered metatype to cross to the UI thread.
              QStringList fields;
              fields << QString::fromStdString(summary.user.id)
                     << QString::fromStdString(summary.username)
                     << QString::fromStdString(summary.user.display_name)
                     << QString::fromStdString(std::string(models::to_string(summary.user.role)))
                     << (summary.created_at > 0 ? QDateTime::fromSecsSinceEpoch(summary.created_at)
                                                      .toString(QStringLiteral("yyyy-MM-dd"))
                                                : QString())
                     << (summary.online ? QStringLiteral("yes") : QString())
                     // Empty for an account with nothing taken away, which is
                     // most of them, so the column reads as an exception list
                     // rather than as a column of "none".
                     << QString::fromStdString(models::describe(summary.user.restrictions));
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
              fields << QString::fromStdString(summary.id) << QString::fromStdString(summary.id)
                     << QString::fromStdString(summary.name)
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
              fields << QString::fromStdString(entry.id)
                     << QDateTime::fromSecsSinceEpoch(entry.timestamp_seconds)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                     << QString::fromStdString(entry.actor_username)
                     << QString::fromStdString(entry.action)
                     << QString::fromStdString(entry.target_id)
                     << QString::fromStdString(entry.room_id)
                     << QString::fromStdString(entry.detail);
              rows.push_back(fields.join(QLatin1Char('\t')));
            }
            QMetaObject::invokeMethod(admin_panel_, "apply_audit", Qt::QueuedConnection,
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
            QMetaObject::invokeMethod(this, "apply_forced_mute", Qt::QueuedConnection,
                                      Q_ARG(QString, name), Q_ARG(QString, by_name),
                                      Q_ARG(bool, muted));
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
            QMetaObject::invokeMethod(
                this, "apply_restrictions", Qt::QueuedConnection, Q_ARG(QString, name),
                Q_ARG(QString, by_name),
                Q_ARG(QString, QString::fromStdString(models::describe(restrictions))),
                Q_ARG(QString, QString::fromStdString(reason)), Q_ARG(bool, is_us));
          },
  });

  session_.on_room_created([this](const std::string& room_id) {
    QMetaObject::invokeMethod(this, "apply_room_created", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(room_id)));
  });
}

void MainWindow::on_connect() {
  show_login_error(QString{});
  const QString user = username_->text().trimmed();
  if (user.isEmpty()) {
    show_login_error(QStringLiteral("Enter a username."));
    return;
  }

  if (const auto connected =
          session_.connect_and_authenticate(user.toStdString(), password_->text().toStdString());
      !connected) {
    show_login_error(describe(QString::fromStdString(connected.error().code),
                              QString::fromStdString(connected.error().message)));
  }
}

void MainWindow::on_sign_out() {
  // disconnect() drops the identity along with the socket, and the state it
  // publishes is what carries the interface back to the login screen. Nothing
  // here changes the page by hand: two routes to the same page are two routes
  // that can disagree.
  session_.disconnect();

  // The password does not stay in the field for whoever sits down next. The
  // username does, because signing back in as yourself is the ordinary case
  // and a server address that has just been changed is the other one.
  password_->clear();
  show_login_error(QString{});
}

void MainWindow::on_create_room() {
  if (const auto created = session_.create_room("room"); !created) {
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
  screen_view_->clear();
  sharing_label_->clear();
  // The conversation belongs to the room that was just left. Leaving it on
  // screen would put one room's messages above the next room's history.
  chat_view_->clear();
  chat_input_->clear();
  clear_metrics();
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
  // A window of its own with Qt::Popup, and not a QMenu holding one widget.
  //
  // The QMenu route was written first and looks right: it draws the grid, it
  // closes on a click outside, and it needs no lifetime handling. It also
  // never delivers a click to a single one of those buttons on macOS. The menu
  // keeps the mouse grab for itself and the presses die inside it, so the
  // picker opened, showed forty emoji, and did nothing at all.
  //
  // Qt::Popup gives the same behaviour a picker needs, closing on a click
  // outside and on escape, and what is inside it stays an ordinary widget that
  // sees ordinary events. WA_DeleteOnClose is what disposes of it, because
  // every one of those ways out ends in close().
  auto* popup = new QWidget(chat_emoji_, Qt::Popup);
  popup->setAttribute(Qt::WA_DeleteOnClose);

  auto* grid = new QGridLayout(popup);
  grid->setContentsMargins(4, 4, 4, 4);
  grid->setSpacing(2);

  int row = 0;
  int column = 0;
  for (const QString& emoji : quick_emoji()) {
    auto* button = new QPushButton(emoji, popup);
    button->setFlat(true);
    button->setFixedSize(34, 34);
    connect(button, &QPushButton::clicked, popup, [this, emoji, popup] {
      insert_emoji(emoji);
      // Closed after one pick. Somebody who wants a second one presses the
      // button again, which is a click either way, and a picker that stays
      // open over the field it is typing into is one that has to be dismissed.
      popup->close();
    });
    grid->addWidget(button, row, column);
    if (++column == kEmojiColumns) {
      column = 0;
      ++row;
    }
  }

  // Above the button and lined up with its right edge. The row it sits on is
  // at the bottom of the window, so a picker dropped downwards would open off
  // the screen and be moved back over the field it is meant to fill.
  popup->adjustSize();
  popup->move(
      chat_emoji_->mapToGlobal(QPoint(chat_emoji_->width() - popup->width(), -popup->height())));
  popup->show();
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

  // Whatever the dialog last chose, or what the configuration says for
  // somebody who has never opened it.
  const client::app::ScreenAudio audio =
      screen_audio_.value_or(client::app::ScreenAudio{.mode = session_.screen_audio_mode()});

  if (const auto started = session_.start_screen_share(monitor_id_.toStdString(), audio);
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

void MainWindow::on_open_settings() {
  SettingsDialog dialog(session_, this);
  dialog.exec();
  monitor_id_ = dialog.selected_monitor();
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

  // The status bar rather than the button, for the same reason the mute notice
  // goes there: a button that renames itself moves what is beside it, and the
  // status bar is already where this application says what just happened.
  status_->setText(QStringLiteral("Room code %1 copied").arg(room));
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

  welcome_->setText(
      user.display_name.empty()
          ? QStringLiteral("PartyShare")
          : QStringLiteral("Hello, %1").arg(QString::fromStdString(user.display_name)));

  // Again here, and not only where the button was pressed. The metrics arrive
  // through a queued call, so one measured a moment before the call ended can
  // be delivered after on_leave_room has already emptied the labels, and it
  // would write the last numbers of a finished call back onto the lobby. This
  // runs once the state itself says there is no call, which is after that.
  if (state_ != client::app::CallSession::State::InCall) {
    clear_metrics();
  }

  refresh_controls();
  show_page();
}

void MainWindow::apply_room_list(const QStringList& rows, bool may_create) {
  fill(room_list_, rows);
  const bool empty = rows.isEmpty();
  room_list_->setVisible(!empty);
  rooms_empty_->setVisible(empty);

  // Disabled rather than left to be refused on the click. The server answers
  // room_limit_reached either way, and a button that always fails is a worse
  // way to learn a rule than a button that is plainly not available.
  create_button_->setEnabled(may_create);
  create_button_->setToolTip(may_create
                                 ? QString()
                                 : QStringLiteral("You already have a room. An administrator has "
                                                  "to close it before you can make another."));
}

void MainWindow::apply_participants(const QStringList& names) {
  const QString selected = selected_participant_;

  // Rebuilt wholesale, so the selection has to be restored by identity rather
  // than by row: the order changes as people join and leave.
  const QSignalBlocker blocker(participants_);
  participants_->clear();
  for (const QString& entry : names) {
    const QStringList parts = entry.split(QLatin1Char('\t'));
    auto* item = new QListWidgetItem(parts.value(0), participants_);
    item->setData(Qt::UserRole, parts.value(1));
    item->setData(kNameRole, parts.value(2));
    if (!selected.isEmpty() && parts.value(1) == selected) {
      participants_->setCurrentItem(item);
    }
  }

  // The signals were blocked through the rebuild, so the volume controls have
  // not heard about any of it. This is also what clears them when whoever was
  // selected has left the room.
  on_participant_selected();
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
  quality_->setText(
      QStringLiteral("● network %1")
          .arg(QString::fromUtf8(client::app::to_string(measured).data(),
                                 static_cast<qsizetype>(client::app::to_string(measured).size()))));
  quality_->setStyleSheet(
      QStringLiteral("color: %1; font-weight: bold;").arg(quality_colour(measured).name()));
}

void MainWindow::apply_link(int round_trip_ms) {
  link_round_trip_ms_ = round_trip_ms;

  // While there is a call, the verdict weighing all three measurements is the
  // better answer and keeps the label. This one fills the rest of the time,
  // which is most of it, and is kept up to date underneath so that leaving a
  // room has something current to fall back to rather than a blank wait.
  if (state_ == client::app::CallSession::State::InCall) {
    return;
  }
  show_link_quality();
}

void MainWindow::show_link_quality() {
  if (link_round_trip_ms_ < 0) {
    // Not blank. "Nothing is being measured" is itself the answer to whether
    // the connection is any good, and an empty status bar says it so quietly
    // that it reads as the indicator being broken.
    if (shown_link_quality_ != kNoLinkQuality) {
      shown_link_quality_ = kNoLinkQuality;
      quality_->setStyleSheet(
          QStringLiteral("color: %1; font-weight: bold;").arg(theme::colors().muted.name()));
    }
    quality_->setText(QStringLiteral("● no server"));
    return;
  }

  const auto measured = client::app::quality_of(std::chrono::milliseconds(link_round_trip_ms_));
  // The number as well as the word, unlike the call indicator, because there
  // is only one measurement here and hiding it behind an adjective would leave
  // "good" doing work three numbers do during a call.
  quality_->setText(QStringLiteral("● server %1 ms").arg(link_round_trip_ms_));

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
  QMessageBox::warning(this, QStringLiteral("PartyShare"), text);
}

void MainWindow::apply_room_created(const QString& room_id) {
  room_id_->setText(room_id);

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
  on_join_room();
}

void MainWindow::apply_screen_share(const QString& user_id) {
  if (user_id.isEmpty()) {
    sharing_label_->clear();
    screen_view_->set_placeholder(QStringLiteral("nobody is sharing a screen"));
    screen_view_->clear();
    refresh_controls();
    return;
  }

  QString name = user_id;
  for (int row = 0; row < participants_->count(); ++row) {
    const QListWidgetItem* item = participants_->item(row);
    if (item->data(Qt::UserRole).toString() == user_id) {
      name = item->data(kNameRole).toString();
      break;
    }
  }

  const bool is_me = user_id == QString::fromStdString(session_.local_user().id);
  sharing_label_->setText(is_me ? QStringLiteral("you are sharing")
                                : QStringLiteral("%1 is sharing").arg(name));

  // Nobody receives their own screen back, so while you are the one sharing
  // this panel stays empty. Saying that nobody is sharing would be wrong in
  // exactly the moment it matters most.
  if (is_me) {
    screen_view_->set_placeholder(QStringLiteral("you are sharing this screen with the room"));
    screen_view_->clear();
  }
  refresh_controls();
}

void MainWindow::build_admin_page() {
  admin_panel_ = new AdminPanel(session_, pages_);

  connect(admin_panel_, &AdminPanel::closed, this, &MainWindow::on_close_administration);
  connect(admin_panel_, &AdminPanel::failed, this, &MainWindow::apply_error);

  pages_->insertWidget(kAdminPage, admin_panel_);
}

void MainWindow::on_open_administration() {
  previous_page_ = pages_->currentIndex();
  go_to_page(kAdminPage);
  // Asked for on the way in, never cached from last time: the accounts and the
  // rooms belong to the server and other people change them.
  admin_panel_->refresh();
}

void MainWindow::on_close_administration() {
  // Back where they came from, which is the home page unless they opened the
  // panel during a call.
  go_to_page(previous_page_ == kAdminPage ? kHomePage : previous_page_);
}

void MainWindow::on_participant_menu(const QPoint& where) {
  // Nothing to offer somebody who is not an administrator, and an empty menu
  // that appears on right click is worse than no menu at all.
  if (!session_.is_admin()) {
    return;
  }

  const QListWidgetItem* item = participants_->itemAt(where);
  if (item == nullptr) {
    return;
  }

  const QString user_id = item->data(Qt::UserRole).toString();
  const QString name = item->data(kNameRole).toString();
  if (user_id.isEmpty() || user_id == QString::fromStdString(session_.local_user().id)) {
    // Nothing here applies to yourself: the mute button and the leave button
    // are what those are for, and the server refuses a self kick anyway.
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
  QAction* toggle_mute = menu.addAction(muted ? QStringLiteral("Unmute %1").arg(name)
                                              : QStringLiteral("Mute %1").arg(name));

  // The two below are account restrictions and outlive this room, which is why
  // they sit in their own section: "mute for now" and "may not speak until
  // somebody says otherwise" are different decisions, and a menu that ran them
  // together would make the second one easy to take by accident. The
  // administration panel is where all four are managed together; this is the
  // shortcut for the two that are usually reached for while a call is going on.
  menu.addSeparator();
  QAction* toggle_silence =
      menu.addAction(restrictions.silenced ? QStringLiteral("Let %1 use the chat again").arg(name)
                                           : QStringLiteral("Silence %1 in the chat").arg(name));
  QAction* toggle_share_block =
      menu.addAction(restrictions.screen_share_blocked
                         ? QStringLiteral("Let %1 share their screen again").arg(name)
                         : QStringLiteral("Stop %1 from sharing their screen").arg(name));

  menu.addSeparator();
  QAction* kick = menu.addAction(QStringLiteral("Remove %1 from the room").arg(name));

  const QAction* chosen = menu.exec(participants_->mapToGlobal(where));
  if (chosen == nullptr) {
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
  chat_view_->clear();
  for (const QString& line : lines) {
    chat_view_->addItem(line);
  }
  chat_view_->scrollToBottom();
}

void MainWindow::append_chat_line(const QString& line) {
  // Follow the conversation only when it was already being followed. Somebody
  // who has scrolled up to read something keeps their place, which is the
  // difference between a chat pane and one that snatches the page away every
  // time anybody says anything.
  const QScrollBar* scroll = chat_view_->verticalScrollBar();
  const bool was_at_bottom = scroll->value() >= scroll->maximum();

  chat_view_->addItem(line);
  if (was_at_bottom) {
    chat_view_->scrollToBottom();
  }
}

void MainWindow::apply_kicked(const QString& reason) {
  // The session has already left the room and cleared its media by the time
  // this runs, so there is nothing to undo here: only to say what happened and
  // to go back.
  chat_view_->clear();
  chat_input_->clear();
  refresh_controls();
  show_page();

  const QString text =
      reason.isEmpty()
          ? QStringLiteral("An administrator removed you from the room.")
          : QStringLiteral("An administrator removed you from the room:\n\n%1").arg(reason);
  QMessageBox::information(this, QStringLiteral("Removed from the room"), text);
}

void MainWindow::apply_restrictions(const QString& name, const QString& by_name,
                                    const QString& summary, const QString& reason, bool is_us) {
  // The controls first. Whether the microphone, the share button and the chat
  // box are usable is read off the session, and the session is already in step
  // with the server by the time this runs.
  refresh_controls();

  const QString who = name.isEmpty() ? QStringLiteral("Somebody") : name;
  const QString by = by_name.isEmpty() ? QStringLiteral("an administrator") : by_name;

  if (!is_us) {
    status_->setText(summary.isEmpty()
                         ? QStringLiteral("%1 lifted the restrictions on %2").arg(by, who)
                         : QStringLiteral("%1 restricted %2: %3").arg(by, who, summary));
    return;
  }

  // Being told about somebody else is a line on the status bar. Being told
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

void MainWindow::apply_forced_mute(const QString& name, const QString& by_name, bool muted) {
  refresh_controls();

  const QString who = name.isEmpty() ? QStringLiteral("Somebody") : name;
  const QString by = by_name.isEmpty() ? QStringLiteral("an administrator") : by_name;
  status_->setText(muted ? QStringLiteral("%1 was muted by %2").arg(who, by)
                         : QStringLiteral("%1 was unmuted by %2").arg(who, by));
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
      room_title_->setText(
          QStringLiteral("Room: %1").arg(QString::fromStdString(session_.room_id())));
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
  pages_->setCurrentIndex(index);

  // Arriving at the home page asks for the list, which covers both ways of
  // getting here: signing in, and coming back out of a call. The server pushes
  // it after that, whenever a room appears or is closed, so this is the only
  // place that has to ask.
  if (index == kHomePage) {
    (void)session_.list_rooms();
  }

  QWidget* page = pages_->widget(index);
  if (page == nullptr) {
    return;
  }

  auto* fade = new QGraphicsOpacityEffect(page);
  fade->setOpacity(0.0);
  page->setGraphicsEffect(fade);

  auto* animation = new QPropertyAnimation(fade, "opacity", page);
  animation->setDuration(kPageFadeMs);
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
      animation, &QPropertyAnimation::finished, this, [page] { page->setGraphicsEffect(nullptr); },
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
  create_button_->setEnabled(authenticated);
  join_button_->setEnabled(authenticated);
  // The server is what actually decides, and it re-reads the role on every
  // administrative message. This only decides whether the door is visible.
  admin_button_->setVisible(authenticated && session_.is_admin());

  // What an administrator has taken away from this account, which the session
  // keeps in step with the server. The server refuses each of these anyway;
  // what these three do is say so before somebody presses a control and is
  // told no, and explain the refusal in the tooltip rather than in an error
  // box that appears a second later.
  const models::Restrictions& restrictions = session_.local_user().restrictions;

  mute_button_->setChecked(session_.muted());
  mute_button_->setText(session_.muted() ? QStringLiteral("Unmute microphone")
                                         : QStringLiteral("Mute microphone"));
  // A forced mute is not a button that turned itself off: it is one that will
  // not turn back on. Disabled rather than left clickable and refused, which
  // would leave the microphone looking like it had failed.
  mute_button_->setEnabled(!restrictions.muted);
  mute_button_->setToolTip(
      restrictions.muted ? QStringLiteral("An administrator has muted this account.") : QString());

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
  share_button_->setChecked(sharing);
  share_button_->setText(sharing ? QStringLiteral("Stop sharing") : QStringLiteral("Share screen"));
  share_button_->setToolTip(
      restrictions.screen_share_blocked
          ? QStringLiteral("An administrator has blocked screen sharing for this account.")
          : QString());

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
}

}  // namespace dv::ui
