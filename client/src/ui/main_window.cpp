#include "ui/main_window.hpp"

#include <dv/logging/logger.hpp>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace dv::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Voice Desktop"));
  setMinimumSize(480, 320);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setSpacing(16);
  layout->setContentsMargins(48, 48, 48, 48);

  auto* title = new QLabel(QStringLiteral("Voice Desktop"), central);
  QFont title_font = title->font();
  title_font.setPointSize(title_font.pointSize() + 8);
  title_font.setBold(true);
  title->setFont(title_font);
  title->setAlignment(Qt::AlignCenter);

  auto* create_button = new QPushButton(QStringLiteral("Criar Sala"), central);
  auto* join_button = new QPushButton(QStringLiteral("Entrar em Sala"), central);
  create_button->setMinimumHeight(40);
  join_button->setMinimumHeight(40);

  layout->addStretch();
  layout->addWidget(title);
  layout->addSpacing(16);
  layout->addWidget(create_button);
  layout->addWidget(join_button);
  layout->addStretch();

  setCentralWidget(central);

  connect(create_button, &QPushButton::clicked, this, &MainWindow::on_create_room);
  connect(join_button, &QPushButton::clicked, this, &MainWindow::on_join_room);
}

void MainWindow::on_create_room() {
  DV_LOG_INFO("Create room requested (wired to the signaling client in M4)");
}

void MainWindow::on_join_room() {
  DV_LOG_INFO("Join room requested (wired to the signaling client in M4)");
}

}  // namespace dv::ui
