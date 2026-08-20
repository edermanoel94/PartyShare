#include "ui/main_window.hpp"

#include <algorithm>
#include <cmath>

#include <dv/logging/logger.hpp>

#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "media/media_session.hpp"
#include "ui/screen_view.hpp"
#include "ui/settings_dialog.hpp"

namespace dv::ui {
namespace {

/// The pages of the stack, in the order section 19 of SPEC.md walks through
/// them.
constexpr int kLoginPage = 0;
constexpr int kHomePage = 1;
constexpr int kRoomPage = 2;

/// Where the participant's plain name is kept on a list item, next to the
/// user id in Qt::UserRole. The visible text carries the state as well.
constexpr int kNameRole = Qt::UserRole + 1;

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
[[nodiscard]] int bar_percentage(double level) {
  constexpr double kFloorDb = -60.0;
  if (level <= 0.0) {
    return 0;
  }
  const double decibels = 20.0 * std::log10(level);
  if (decibels <= kFloorDb) {
    return 0;
  }
  const double filled = (decibels - kFloorDb) / -kFloorDb;
  return static_cast<int>(std::clamp(filled, 0.0, 1.0) * 100.0);
}

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
    : QMainWindow(parent), session_(session), pages_(new QStackedWidget(this)) {
  setWindowTitle(QStringLiteral("PartyShare"));
  setMinimumSize(720, 560);
  resize(960, 760);

  setCentralWidget(pages_);

  build_login_page();
  build_home_page();
  build_room_page();

  status_ = new QLabel(QStringLiteral("disconnected"), this);
  quality_ = new QLabel(QString{}, this);
  metrics_ = new QLabel(QString{}, this);
  QFont small = metrics_->font();
  small.setPointSize(small.pointSize() - 1);
  metrics_->setFont(small);
  metrics_->setStyleSheet(QStringLiteral("color: palette(mid);"));

  statusBar()->addWidget(status_);
  statusBar()->addWidget(quality_);
  statusBar()->addPermanentWidget(metrics_);

  wire_session();
  refresh_controls();
  show_page();
}

MainWindow::~MainWindow() = default;

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

  login_error_ = new QLabel(QString{}, box);
  login_error_->setWordWrap(true);
  // A fixed red rather than a palette role. bright-text is white on a light
  // theme, which is an error message nobody can read.
  login_error_->setStyleSheet(QStringLiteral("color: #c62828;"));

  form->addRow(QStringLiteral("Username"), username_);
  form->addRow(QStringLiteral("Password"), password_);
  form->addRow(connect_button_);
  form->addRow(login_error_);

  auto* centred = new QHBoxLayout();
  centred->addStretch();
  centred->addWidget(box);
  centred->addStretch();
  outer->addLayout(centred);
  outer->addStretch();

  connect(connect_button_, &QPushButton::clicked, this, &MainWindow::on_connect);
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
  box->setMaximumWidth(420);
  auto* column = new QVBoxLayout(box);

  create_button_ = new QPushButton(QStringLiteral("Create room"), box);
  create_button_->setMinimumHeight(44);

  room_id_ = new QLineEdit(box);
  room_id_->setPlaceholderText(QStringLiteral("Room ID, for example 8F42A1"));
  room_id_->setAlignment(Qt::AlignCenter);

  join_button_ = new QPushButton(QStringLiteral("Join room"), box);
  join_button_->setMinimumHeight(44);

  column->addWidget(create_button_);
  column->addSpacing(16);
  column->addWidget(room_id_);
  column->addWidget(join_button_);

  auto* centred = new QHBoxLayout();
  centred->addStretch();
  centred->addWidget(box);
  centred->addStretch();

  outer->addWidget(welcome_);
  outer->addSpacing(24);
  outer->addLayout(centred);
  outer->addStretch();

  connect(create_button_, &QPushButton::clicked, this, &MainWindow::on_create_room);
  connect(join_button_, &QPushButton::clicked, this, &MainWindow::on_join_room);
  connect(room_id_, &QLineEdit::returnPressed, this, &MainWindow::on_join_room);

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
  sharing_label_ = new QLabel(QString{}, page);
  sharing_label_->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: bold;"));
  header->addWidget(room_title_);
  header->addStretch();
  header->addWidget(sharing_label_);

  screen_view_ = new ScreenView(page);

  auto* people = new QGroupBox(QStringLiteral("Participants"), page);
  auto* people_column = new QVBoxLayout(people);
  participants_ = new QListWidget(people);
  participants_->setMinimumHeight(120);
  participants_->setMaximumHeight(180);

  microphone_level_ = new QProgressBar(people);
  microphone_level_->setRange(0, 100);
  microphone_level_->setTextVisible(false);
  microphone_level_->setFixedHeight(8);

  auto* volume_row = new QHBoxLayout();
  volume_label_ = new QLabel(QStringLiteral("Volume: select a participant"), people);
  volume_ = new QSlider(Qt::Horizontal, people);
  // 0 to 200 percent: above 100 is amplification, which WebRTC allows.
  volume_->setRange(0, 200);
  volume_->setValue(100);
  volume_->setEnabled(false);
  volume_row->addWidget(volume_label_);
  volume_row->addWidget(volume_, 1);

  people_column->addWidget(microphone_level_);
  people_column->addWidget(participants_);
  people_column->addLayout(volume_row);

  auto* controls = new QHBoxLayout();
  mute_button_ = new QPushButton(QStringLiteral("Mute microphone"), page);
  mute_button_->setCheckable(true);
  share_button_ = new QPushButton(QStringLiteral("Share screen"), page);
  share_button_->setCheckable(true);
  settings_button_ = new QPushButton(QStringLiteral("Settings"), page);
  leave_button_ = new QPushButton(QStringLiteral("Leave"), page);
  for (QPushButton* button : {mute_button_, share_button_, settings_button_, leave_button_}) {
    button->setMinimumHeight(38);
  }
  controls->addWidget(mute_button_);
  controls->addWidget(share_button_);
  controls->addWidget(settings_button_);
  controls->addStretch();
  controls->addWidget(leave_button_);

  column->addLayout(header);
  column->addWidget(screen_view_, 1);
  column->addWidget(people);
  column->addLayout(controls);

  connect(mute_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_mute);
  connect(share_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_share);
  connect(settings_button_, &QPushButton::clicked, this, &MainWindow::on_open_settings);
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
                label += QStringLiteral("  (sharing)");
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
  });

  session_.on_room_created([this](const std::string& room_id) {
    QMetaObject::invokeMethod(this, "apply_room_created", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(room_id)));
  });
}

void MainWindow::on_connect() {
  login_error_->clear();
  const QString user = username_->text().trimmed();
  if (user.isEmpty()) {
    login_error_->setText(QStringLiteral("Enter a username."));
    return;
  }

  if (const auto connected =
          session_.connect_and_authenticate(user.toStdString(), password_->text().toStdString());
      !connected) {
    login_error_->setText(describe(QString::fromStdString(connected.error().code),
                                   QString::fromStdString(connected.error().message)));
  }
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
  refresh_controls();
  show_page();
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

  if (const auto started = session_.start_screen_share(monitor_id_.toStdString()); !started) {
    apply_error(QString::fromStdString(started.error().code),
                QString::fromStdString(started.error().message));
  }
  refresh_controls();
}

void MainWindow::on_open_settings() {
  SettingsDialog dialog(session_, this);
  dialog.exec();
  monitor_id_ = dialog.selected_monitor();
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
    login_error_->setText(describe(QString{}, detail));
  }

  welcome_->setText(
      user.display_name.empty()
          ? QStringLiteral("PartyShare")
          : QStringLiteral("Hello, %1").arg(QString::fromStdString(user.display_name)));

  refresh_controls();
  show_page();
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
  metrics_->setText(summary);

  const auto measured = static_cast<client::app::NetworkQuality>(quality);
  static const QHash<int, QString> kColours = {
      {static_cast<int>(client::app::NetworkQuality::Good), QStringLiteral("#2e7d32")},
      {static_cast<int>(client::app::NetworkQuality::Fair), QStringLiteral("#ef6c00")},
      {static_cast<int>(client::app::NetworkQuality::Poor), QStringLiteral("#c62828")},
  };

  if (measured == client::app::NetworkQuality::Unknown) {
    quality_->clear();
    return;
  }
  quality_->setText(
      QStringLiteral("● network %1")
          .arg(QString::fromUtf8(client::app::to_string(measured).data(),
                                 static_cast<qsizetype>(client::app::to_string(measured).size()))));
  quality_->setStyleSheet(
      QStringLiteral("color: %1; font-weight: bold;").arg(kColours.value(quality)));
}

void MainWindow::apply_local_level(double level, bool speaking) {
  microphone_level_->setValue(bar_percentage(level));
  microphone_level_->setStyleSheet(
      speaking ? QStringLiteral("QProgressBar::chunk { background-color: palette(highlight); }")
               : QString());
}

void MainWindow::apply_error(const QString& code, const QString& message) {
  DV_LOG_WARN("UI: {} ({})", message.toStdString(), code.toStdString());

  const QString text = describe(code, message);
  if (pages_->currentIndex() == kLoginPage) {
    login_error_->setText(text);
    return;
  }
  QMessageBox::warning(this, QStringLiteral("PartyShare"), text);
}

void MainWindow::apply_room_created(const QString& room_id) {
  room_id_->setText(room_id);
  // Created and entered in one movement. Asking someone to press a second
  // button to walk into the room they just made is a step with no decision in
  // it.
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

void MainWindow::show_page() {
  switch (state_) {
    // Until there is a call, the page depends only on whether anyone is signed
    // in: connecting and authenticated look the same to a person waiting.
    case client::app::CallSession::State::Idle:
    case client::app::CallSession::State::Failed:
    case client::app::CallSession::State::Connecting:
    case client::app::CallSession::State::Authenticated:
      pages_->setCurrentIndex(session_.local_user().id.empty() ? kLoginPage : kHomePage);
      break;
    case client::app::CallSession::State::InCall:
      pages_->setCurrentIndex(kRoomPage);
      room_title_->setText(
          QStringLiteral("Room: %1").arg(QString::fromStdString(session_.room_id())));
      break;
  }
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

  mute_button_->setChecked(session_.muted());
  mute_button_->setText(session_.muted() ? QStringLiteral("Unmute microphone")
                                         : QStringLiteral("Mute microphone"));

  const bool sharing = session_.sharing_screen();
  const std::string sharer = session_.screen_sharer();
  const bool someone_else_is_sharing = !sharer.empty() && sharer != session_.local_user().id;

  // Section 5.2 of SPEC.md allows one screen at a time, and the server refuses
  // a second one. Saying so with a disabled button beats letting the click
  // through and answering with an error.
  share_button_->setEnabled(in_call && !someone_else_is_sharing);
  share_button_->setChecked(sharing);
  share_button_->setText(sharing ? QStringLiteral("Stop sharing") : QStringLiteral("Share screen"));
}

}  // namespace dv::ui
