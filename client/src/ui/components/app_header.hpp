#pragma once

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPaintEvent;

namespace dv::ui {

/// The bar across the top of a page: what the page is called, a quieter line
/// under that, and room on the right for what can be done from here.
///
/// One per page rather than one for the window, so that each page owns its
/// own actions and nothing has to show and hide buttons as pages change.
class AppHeader : public QWidget {
  Q_OBJECT

 public:
  explicit AppHeader(QWidget* parent = nullptr);

  void set_title(const QString& title);
  /// The quieter line under the title. Empty takes the line away, and the
  /// title is centred in the bar on its own.
  void set_subtitle(const QString& subtitle);

  /// Straight after the title: a chip, a small button. Things about what the
  /// title names.
  [[nodiscard]] QHBoxLayout* beside_title() const { return beside_title_; }
  /// Against the right hand edge: the page's actions, the primary one last.
  [[nodiscard]] QHBoxLayout* actions() const { return actions_; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QLabel* title_ = nullptr;
  QLabel* subtitle_ = nullptr;
  QHBoxLayout* beside_title_ = nullptr;
  QHBoxLayout* actions_ = nullptr;
};

}  // namespace dv::ui
