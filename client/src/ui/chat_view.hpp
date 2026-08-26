#pragma once

#include <QListWidget>
#include <QString>
#include <QStringList>
#include <Qt>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPoint;
class QResizeEvent;

namespace dv::ui {

/// The room's conversation, with the URLs in it made clickable.
///
/// Still a list of lines, and still selectable line by line, because that is
/// what a chat beside a screen share is for and copying a whole exchange out
/// of it is half of the job. What changed is how a line is drawn: each row is
/// laid out as a small rich text document rather than as plain text, which is
/// what makes it possible to know that a particular pixel is inside a URL
/// rather than beside one.
///
/// Ctrl and a left click on a link opens it in the system browser. Ctrl and
/// not a bare click on purpose: this is a list whose rows are selected by
/// clicking them, and a bare click that sometimes selects a row and sometimes
/// launches a browser would be a chat where nobody can safely click anything.
/// The pointer turns into a hand while Ctrl is held over a link, so the
/// difference is visible before it is committed to.
///
/// What counts as a link is decided by app::find_links, and it is only ever
/// http and https. The reasoning is in client/src/app/chat_links.hpp, and it
/// matters: the text here was written by another participant.
class ChatView : public QListWidget {
  Q_OBJECT

 public:
  explicit ChatView(QWidget* parent = nullptr);

  /// Adds one line to the end.
  ///
  /// Use this and not addItem. The rich text a row is drawn from is built once,
  /// here, and kept on the item: building it in the delegate instead would
  /// re-scan every visible line for URLs on every repaint, and a repaint is
  /// what a scroll is made of.
  void append_line(const QString& line);

  /// Replaces everything on screen with `lines`.
  void set_lines(const QStringList& lines);

  /// Where a row's rich text lives on the item.
  static constexpr int kHtmlRole = Qt::UserRole + 1;

 protected:
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  /// The URL under `position`, in viewport coordinates, or empty.
  [[nodiscard]] QString link_at(const QPoint& position) const;

  /// Puts the pointer and the tooltip in the state `position` calls for.
  void refresh_hover(const QPoint& position, Qt::KeyboardModifiers modifiers);

  /// The same, for the pointer where it already is. What Ctrl going down or up
  /// needs, since neither of those moves the mouse.
  void refresh_hover_where_the_pointer_is(Qt::KeyboardModifiers modifiers);
};

}  // namespace dv::ui
