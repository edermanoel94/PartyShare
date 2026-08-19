#pragma once

#include <QMainWindow>

namespace dv::ui {

/// The initial screen from section 19 of SPEC.md.
///
/// The UI layer never talks to capture, encoding or transport directly. It
/// will drive the application core, which owns those pipelines. For M0 the
/// buttons only log, so this window proves the Qt toolchain works on every
/// platform without pulling media code into the UI target.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

 private slots:
  void on_create_room();
  void on_join_room();
};

}  // namespace dv::ui
