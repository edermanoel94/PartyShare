#include "ui/main_window.hpp"

#include <dv/logging/logger.hpp>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

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

}  // namespace

MainWindow::MainWindow(client::app::CallSession& session, QWidget* parent)
    : QMainWindow(parent), session_(session) {
  setWindowTitle(QStringLiteral("Voice Desktop"));
  setMinimumSize(520, 520);

  build_widgets();
  wire_session();
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

  auto* call = new QGroupBox(QStringLiteral("Chamada"), central);
  auto* call_layout = new QVBoxLayout(call);
  mute_button_ = new QPushButton(QStringLiteral("Mutar microfone"), call);
  mute_button_->setCheckable(true);
  participants_ = new QListWidget(call);
  participants_->setMinimumHeight(140);
  call_layout->addWidget(mute_button_);
  call_layout->addWidget(participants_);

  status_ = new QLabel(QStringLiteral("desconectado"), central);
  metrics_ = new QLabel(QStringLiteral("sem métricas ainda"), central);
  QFont small = metrics_->font();
  small.setPointSize(small.pointSize() - 1);
  metrics_->setFont(small);
  metrics_->setStyleSheet(QStringLiteral("color: palette(mid);"));

  layout->addWidget(account);
  layout->addWidget(room);
  layout->addWidget(call);
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
              QString label = QString::fromStdString(participant.user.display_name.empty()
                                                         ? participant.user.id
                                                         : participant.user.display_name);
              if (participant.muted) {
                label += QStringLiteral("  (mudo)");
              } else if (participant.audio_active) {
                label += QStringLiteral("  (falando)");
              }
              names.push_back(label);
            }
            QMetaObject::invokeMethod(this, "apply_participants", Qt::QueuedConnection,
                                      Q_ARG(QStringList, names));
          },
      .on_metrics =
          [this](client::audio::AudioStats stats) {
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
      .on_error =
          [this](Error error) {
            QMetaObject::invokeMethod(this, "apply_error", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(error.code)),
                                      Q_ARG(QString, QString::fromStdString(error.message)));
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

  if (const auto joined = session_.join(room.toStdString(), username_->text().toStdString());
      !joined) {
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
  participants_->clear();
  participants_->addItems(names);
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
}

}  // namespace dv::ui
