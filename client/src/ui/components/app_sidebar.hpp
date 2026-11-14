#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include "ui/components/icons.hpp"

class QPaintEvent;
class QVBoxLayout;

namespace dv::ui {

class IconButton;

/// The navigation rail down the left edge of the window: the program's mark
/// at the top, the places to go under it, and at the bottom the things that
/// are about this machine or this person rather than about a place.
///
/// It knows the places only as numbers. Which page a number means, and
/// whether going there is allowed right now, is decided by whoever listens to
/// `destination_chosen` - the rail asks, it does not switch anything.
class AppSidebar : public QWidget {
  Q_OBJECT

 public:
  explicit AppSidebar(QWidget* parent = nullptr);

  /// A place to go. `name` is its tooltip and what a screen reader calls it.
  /// The button comes back so it can be hidden, or given a longer tooltip.
  IconButton* add_destination(int id, Icon icon, const QString& name);

  /// Something to do, at the bottom: it opens or performs rather than going
  /// anywhere, so it never looks selected. Connect to its `clicked`.
  IconButton* add_action(Icon icon, const QString& name);

  /// Anything else at the very bottom, under the actions: an avatar.
  void add_footer(QWidget* widget);

  /// Marks `id` as where the window is now. Tells nobody: this reports a
  /// navigation that happened, it does not ask for one.
  void set_current(int id);

 signals:
  /// Somebody pressed a destination. The rail has not selected it: the
  /// listener calls set_current when - and if - the page actually changes.
  void destination_chosen(int id);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QVBoxLayout* places_ = nullptr;
  QVBoxLayout* bottom_ = nullptr;
  QHash<int, IconButton*> destinations_;
};

}  // namespace dv::ui
