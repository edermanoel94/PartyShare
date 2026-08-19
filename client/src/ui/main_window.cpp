#include "ui/main_window.hpp"

#include <algorithm>
#include <cmath>

#include <dv/logging/logger.hpp>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "media/media_session.hpp"
#include "ui/screen_view.hpp"

namespace dv::ui {
namespace {

[[nodiscard]] QString to_display(client::app::CallSession::State state) {
  switch (state) {
    case client::app::CallSession::State::Idle:
      return QStringLiteral("desconectado");
    case client::app::CallSession::State::Connecting:
      return QStringLiteral("conectando");
    case client::app::CallSession::State::Authenticated:
      return QStringLiteral("conectado");
    case client::app::CallSession::State::InCall:
      return QStringLiteral("em chamada");
    case client::app::CallSession::State::Failed:
      return QStringLiteral("falhou");
  }
  return QStringLiteral("desconhecido");
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

/// Where the participant's plain name is kept on a list item, next to the
/// user id in Qt::UserRole. The visible text carries the state as well.
constexpr int kNameRole = Qt::UserRole + 1;

}  // namespace

MainWindow::MainWindow(client::app::CallSession& session, QWidget* parent)
    : QMainWindow(parent), session_(session) {
  setWindowTitle(QStringLiteral("Voice Desktop"));
  setMinimumSize(520, 520);
  // A starting size, so a floating window manager opens something shaped like
  // a call and not like whatever the layout's hint happened to add up to.
  resize(760, 680);

  build_widgets();
  wire_session();
  load_devices();
  load_monitors();
  refresh_controls();
}

MainWindow::~MainWindow() {
  // The session outlives this window, and its callbacks capture `this`. They
  // are dropped here so that nothing arrives at a window that is going away.
  session_.on_events({});
  session_.on_room_created(nullptr);
}

void MainWindow::build_widgets() {
  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setSpacing(12);
  layout->setContentsMargins(24, 24, 24, 24);

  auto* account = new QGroupBox(QStringLiteral("Conta"), central);
  auto* account_form = new QFormLayout(account);
  username_ = new QLineEdit(account);
  password_ = new QLineEdit(account);
  password_->setEchoMode(QLineEdit::Password);
  connect_button_ = new QPushButton(QStringLiteral("Conectar"), account);
  account_form->addRow(QStringLiteral("Usuário"), username_);
  account_form->addRow(QStringLiteral("Senha"), password_);
  account_form->addRow(connect_button_);

  auto* room = new QGroupBox(QStringLiteral("Sala"), central);
  auto* room_layout = new QVBoxLayout(room);
  room_id_ = new QLineEdit(room);
  room_id_->setPlaceholderText(QStringLiteral("ID da sala, por exemplo 8F42A1"));
  // The server assigns six uppercase hexadecimal characters, see section 3 of
  // docs/protocol.md.
  room_id_->setMaxLength(6);

  auto* room_buttons = new QHBoxLayout();
  create_button_ = new QPushButton(QStringLiteral("Criar sala"), room);
  join_button_ = new QPushButton(QStringLiteral("Entrar"), room);
  leave_button_ = new QPushButton(QStringLiteral("Sair"), room);
  room_buttons->addWidget(create_button_);
  room_buttons->addWidget(join_button_);
  room_buttons->addWidget(leave_button_);

  room_layout->addWidget(room_id_);
  room_layout->addLayout(room_buttons);

  auto* devices = new QGroupBox(QStringLiteral("Dispositivos"), central);
  auto* devices_form = new QFormLayout(devices);
  input_device_ = new QComboBox(devices);
  output_device_ = new QComboBox(devices);
  devices_form->addRow(QStringLiteral("Microfone"), input_device_);
  devices_form->addRow(QStringLiteral("Saída"), output_device_);

  auto* call = new QGroupBox(QStringLiteral("Chamada"), central);
  auto* call_layout = new QVBoxLayout(call);
  mute_button_ = new QPushButton(QStringLiteral("Mutar microfone"), call);
  mute_button_->setCheckable(true);

  microphone_level_ = new QProgressBar(call);
  microphone_level_->setRange(0, 100);
  microphone_level_->setValue(0);
  microphone_level_->setTextVisible(false);
  microphone_level_->setFixedHeight(8);

  participants_ = new QListWidget(call);
  participants_->setMinimumHeight(140);

  auto* volume_row = new QHBoxLayout();
  volume_label_ = new QLabel(QStringLiteral("Volume: selecione um participante"), call);
  volume_ = new QSlider(Qt::Horizontal, call);
  // 0 to 200 percent: above 100 is amplification, which WebRTC allows.
  volume_->setRange(0, 200);
  volume_->setValue(100);
  volume_->setEnabled(false);
  volume_row->addWidget(volume_label_);
  volume_row->addWidget(volume_, 1);

  call_layout->addWidget(mute_button_);
  call_layout->addWidget(microphone_level_);
  call_layout->addWidget(participants_);
  call_layout->addLayout(volume_row);

  auto* share = new QGroupBox(QStringLiteral("Tela"), central);
  auto* share_layout = new QVBoxLayout(share);

  auto* share_row = new QHBoxLayout();
  monitor_ = new QComboBox(share);
  share_button_ = new QPushButton(QStringLiteral("Compartilhar tela"), share);
  share_button_->setCheckable(true);
  sharing_label_ = new QLabel(QString{}, share);
  // The indicator section 5.2 of SPEC.md asks for. Empty until somebody
  // actually starts, so an idle room says nothing rather than saying no.
  sharing_label_->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: bold;"));
  share_row->addWidget(new QLabel(QStringLiteral("Monitor"), share));
  share_row->addWidget(monitor_, 1);
  share_row->addWidget(share_button_);
  share_row->addWidget(sharing_label_);

  screen_view_ = new ScreenView(share);

  share_layout->addLayout(share_row);
  share_layout->addWidget(screen_view_, 1);

  status_ = new QLabel(QStringLiteral("desconectado"), central);
  metrics_ = new QLabel(QStringLiteral("sem métricas ainda"), central);
  QFont small = metrics_->font();
  small.setPointSize(small.pointSize() - 1);
  metrics_->setFont(small);
  metrics_->setStyleSheet(QStringLiteral("color: palette(mid);"));

  layout->addWidget(account);
  layout->addWidget(room);
  layout->addWidget(devices);
  layout->addWidget(call);
  layout->addWidget(share, 1);
  layout->addWidget(status_);
  layout->addWidget(metrics_);

  setCentralWidget(central);

  connect(connect_button_, &QPushButton::clicked, this, &MainWindow::on_connect);

  // Enter has to do the obvious thing in each field. Without this the window
  // looks like it accepts the keyboard and then ignores it.
  connect(username_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  connect(password_, &QLineEdit::returnPressed, this, &MainWindow::on_connect);
  connect(room_id_, &QLineEdit::returnPressed, this, &MainWindow::on_join_room);

  connect(create_button_, &QPushButton::clicked, this, &MainWindow::on_create_room);
  connect(join_button_, &QPushButton::clicked, this, &MainWindow::on_join_room);
  connect(leave_button_, &QPushButton::clicked, this, &MainWindow::on_leave_room);
  connect(mute_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_mute);
  connect(input_device_, &QComboBox::currentIndexChanged, this,
          &MainWindow::on_input_device_changed);
  connect(output_device_, &QComboBox::currentIndexChanged, this,
          &MainWindow::on_output_device_changed);
  connect(participants_, &QListWidget::itemSelectionChanged, this,
          &MainWindow::on_participant_selected);
  connect(volume_, &QSlider::valueChanged, this, &MainWindow::on_volume_changed);
  connect(share_button_, &QPushButton::clicked, this, &MainWindow::on_toggle_share);
}

void MainWindow::wire_session() {
  // Each callback lands on a networking or media thread. Nothing here touches
  // a widget: it packages the news and posts it to the UI thread.
  session_.on_events({
      .on_state =
          [this](client::app::CallSession::State state, std::string detail) {
            QMetaObject::invokeMethod(this, "apply_state", Qt::QueuedConnection,
                                      Q_ARG(int, static_cast<int>(state)),
                                      Q_ARG(QString, QString::fromStdString(detail)));
          },
      .on_participants =
          [this](std::vector<client::app::Participant> list) {
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
                label += QStringLiteral("  (mudo)");
              } else if (participant.speaking) {
                label += QStringLiteral("  (falando)");
              } else if (participant.audio_active) {
                label += QStringLiteral("  (conectado)");
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
            const QString summary =
                QStringLiteral("rtt %1 ms · jitter %2 ms · perdidos %3 · %4 kbps ↑ · %5 kbps ↓")
                    .arg(stats.round_trip_time_ms, 0, 'f', 0)
                    .arg(stats.jitter_ms, 0, 'f', 1)
                    .arg(stats.packets_lost)
                    .arg(stats.send_bitrate_kbps, 0, 'f', 0)
                    .arg(stats.receive_bitrate_kbps, 0, 'f', 0);
            QMetaObject::invokeMethod(this, "apply_metrics", Qt::QueuedConnection,
                                      Q_ARG(QString, summary));
          },
      .on_local_level =
          [this](double level, bool speaking) {
            QMetaObject::invokeMethod(this, "apply_local_level", Qt::QueuedConnection,
                                      Q_ARG(double, level), Q_ARG(bool, speaking));
          },
      .on_error =
          [this](Error error) {
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
          [this](std::string user_id) {
            QMetaObject::invokeMethod(this, "apply_screen_share", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(user_id)));
          },
  });

  session_.on_room_created([this](std::string room_id) {
    QMetaObject::invokeMethod(this, "apply_room_created", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(room_id)));
  });
}

void MainWindow::on_connect() {
  const auto connected = session_.connect_and_authenticate(username_->text().toStdString(),
                                                           password_->text().toStdString());
  if (!connected) {
    apply_error(QString::fromStdString(connected.error().code),
                QString::fromStdString(connected.error().message));
  }
}

void MainWindow::on_create_room() {
  if (const auto created = session_.create_room("sala"); !created) {
    apply_error(QString::fromStdString(created.error().code),
                QString::fromStdString(created.error().message));
  }
}

void MainWindow::on_join_room() {
  const QString room = room_id_->text().trimmed().toUpper();
  if (room.isEmpty()) {
    status_->setText(QStringLiteral("informe o ID da sala"));
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
}

void MainWindow::on_toggle_mute() {
  const bool muted = mute_button_->isChecked();
  if (const auto applied = session_.set_muted(muted); !applied) {
    // The button goes back to what the session actually is, so the interface
    // never shows a state the session did not reach.
    mute_button_->setChecked(session_.muted());
    apply_error(QString::fromStdString(applied.error().code),
                QString::fromStdString(applied.error().message));
    return;
  }
  mute_button_->setText(muted ? QStringLiteral("Desmutar microfone")
                              : QStringLiteral("Mutar microfone"));
}

void MainWindow::apply_state(int state, const QString& detail) {
  state_ = static_cast<client::app::CallSession::State>(state);

  // What the session reports as `detail` is written for the log: it is in
  // English and it carries identifiers. Only the failure case is worth putting
  // in front of a person, and the identity is shown by name rather than by id.
  QString description;
  switch (state_) {
    case client::app::CallSession::State::Authenticated: {
      const models::User user = session_.local_user();
      description = QString::fromStdString(user.display_name.empty() ? user.id : user.display_name);
      break;
    }
    case client::app::CallSession::State::Failed:
      description = detail;
      break;
    case client::app::CallSession::State::Idle:
    case client::app::CallSession::State::Connecting:
    case client::app::CallSession::State::InCall:
      break;
  }

  status_->setText(description.isEmpty()
                       ? to_display(state_)
                       : QStringLiteral("%1 · %2").arg(to_display(state_), description));
  refresh_controls();
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

void MainWindow::apply_local_level(double level, bool speaking) {
  microphone_level_->setValue(bar_percentage(level));
  microphone_level_->setStyleSheet(
      speaking ? QStringLiteral("QProgressBar::chunk { background-color: palette(highlight); }")
               : QString());
}

void MainWindow::on_input_device_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString id = input_device_->itemData(index).toString();
  if (const auto applied = session_.set_input_device(id.toStdString()); !applied) {
    apply_error(QString::fromStdString(applied.error().code),
                QString::fromStdString(applied.error().message));
  }
}

void MainWindow::on_output_device_changed(int index) {
  if (index < 0) {
    return;
  }
  const QString id = output_device_->itemData(index).toString();
  if (const auto applied = session_.set_output_device(id.toStdString()); !applied) {
    apply_error(QString::fromStdString(applied.error().code),
                QString::fromStdString(applied.error().message));
  }
}

void MainWindow::on_participant_selected() {
  const QListWidgetItem* item = participants_->currentItem();
  if (item == nullptr) {
    selected_participant_.clear();
    volume_->setEnabled(false);
    volume_label_->setText(QStringLiteral("Volume: selecione um participante"));
    return;
  }

  selected_participant_ = item->data(Qt::UserRole).toString();

  // Nobody plays back their own voice, so there is no volume to set for
  // yourself. Leaving the slider live would let it be dragged with nothing
  // happening at the other end of it.
  if (selected_participant_ == QString::fromStdString(session_.local_user().id)) {
    volume_->setEnabled(false);
    volume_label_->setText(QStringLiteral("Volume: você não se escuta"));
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
  volume_label_->setText(QStringLiteral("Volume de %1: %2%").arg(participant).arg(volume));
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

  const QString monitor_id = monitor_->currentData().toString();
  if (const auto started = session_.start_screen_share(monitor_id.toStdString()); !started) {
    apply_error(QString::fromStdString(started.error().code),
                QString::fromStdString(started.error().message));
  }
  refresh_controls();
}

void MainWindow::apply_screen_share(const QString& user_id) {
  if (user_id.isEmpty()) {
    sharing_label_->setText(QString{});
    screen_view_->set_placeholder(QStringLiteral("ninguém está compartilhando a tela"));
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
  sharing_label_->setText(is_me ? QStringLiteral("você está compartilhando")
                                : QStringLiteral("%1 está compartilhando").arg(name));

  // Nobody receives their own screen back, so while you are the one sharing
  // this panel stays empty. Saying that nobody is sharing would be wrong in
  // exactly the moment it matters most.
  if (is_me) {
    screen_view_->set_placeholder(QStringLiteral("você está compartilhando esta tela com a sala"));
    screen_view_->clear();
  }
  refresh_controls();
}

void MainWindow::load_monitors() {
  monitor_->clear();
  const auto listed = session_.monitors();
  if (!listed) {
    monitor_->addItem(QStringLiteral("nenhum monitor disponível"), QString{});
    monitor_->setEnabled(false);
    share_button_->setEnabled(false);
    return;
  }

  for (const client::video::Monitor& screen : listed.value()) {
    monitor_->addItem(QString::fromStdString(screen.name), QString::fromStdString(screen.id));
  }
  monitor_->setEnabled(monitor_->count() > 0);
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

void MainWindow::load_devices() {
  // Enumeration needs no call in progress: the choice has to be possible
  // before anyone joins a room.
  const QSignalBlocker input_blocker(input_device_);
  const QSignalBlocker output_blocker(output_device_);

  if (auto found = client::media::input_devices(); found) {
    for (const client::media::AudioDevice& device : found.value()) {
      input_device_->addItem(QString::fromStdString(device.name),
                             QString::fromStdString(device.id));
    }
  } else {
    input_device_->addItem(QStringLiteral("indisponível"));
    input_device_->setEnabled(false);
  }

  if (auto found = client::media::output_devices(); found) {
    for (const client::media::AudioDevice& device : found.value()) {
      output_device_->addItem(QString::fromStdString(device.name),
                              QString::fromStdString(device.id));
    }
  } else {
    output_device_->addItem(QStringLiteral("indisponível"));
    output_device_->setEnabled(false);
  }
}

void MainWindow::apply_metrics(const QString& summary) {
  metrics_->setText(summary);
}

void MainWindow::apply_error(const QString& code, const QString& message) {
  status_->setText(QStringLiteral("erro [%1]: %2").arg(code, message));
}

void MainWindow::apply_room_created(const QString& room_id) {
  room_id_->setText(room_id);
  status_->setText(QStringLiteral("sala %1 criada, entre para começar").arg(room_id));
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
  leave_button_->setEnabled(in_call);
  mute_button_->setEnabled(authenticated);

  const bool sharing = session_.sharing_screen();
  const std::string sharer = session_.screen_sharer();
  const bool someone_else_is_sharing = !sharer.empty() && sharer != session_.local_user().id;

  // Section 5.2 of SPEC.md allows one screen at a time, and the server refuses
  // a second one. Saying so with a disabled button beats letting the click
  // through and answering with an error.
  share_button_->setEnabled(in_call && monitor_->count() > 0 && !someone_else_is_sharing);
  share_button_->setChecked(sharing);
  share_button_->setText(sharing ? QStringLiteral("Parar de compartilhar")
                                 : QStringLiteral("Compartilhar tela"));
  monitor_->setEnabled(!sharing && monitor_->count() > 0);
}

}  // namespace dv::ui
