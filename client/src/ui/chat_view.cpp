#include "ui/chat_view.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QByteArray>
#include <QCursor>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QKeyEvent>
#include <QLatin1String>
#include <QListWidgetItem>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QResizeEvent>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include "app/chat_links.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The space between a row's text and the row's edges.
///
/// The delegate draws everything, so these are the whole of the geometry
/// rather than an adjustment to the style's. They are used identically by the
/// three things that have to agree about where a character is: the height a
/// row asks for, where its text is painted, and which URL is under a pixel.
/// The three disagreeing is what a link that opens when you click beside it
/// looks like.
constexpr int kHorizontalPadding = 8;
constexpr int kVerticalPadding = 6;

[[nodiscard]] QString escaped(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toHtmlEscaped();
}

/// The parts of a line that say who is speaking, and where what they said
/// starts. `body` is zero for a line that is not of that shape.
struct Speaker {
  std::string_view when;
  std::string_view name;
  std::size_t body = 0;
};

/// Reads "[HH:mm] Name: " or "Name: " off the front of a line.
///
/// By shape and not by a field handed over separately, because the line is
/// what crosses from the signaling thread and what the history arrives as. A
/// name with ": " inside it is cut at the first one, and the cost of that is
/// a bold run two words short - nothing here decides anything.
[[nodiscard]] Speaker speaker_of(std::string_view text) {
  Speaker speaker;
  std::size_t at = 0;
  // "[10:51] " is eight bytes, with the bracket closing at the seventh.
  constexpr std::size_t kStampLength = 8;
  if (text.size() > kStampLength && text[0] == '[' && text[3] == ':' && text[6] == ']' &&
      text[7] == ' ') {
    speaker.when = text.substr(1, 5);
    at = kStampLength;
  }
  const std::size_t colon = text.find(": ", at);
  // A name is short. A colon a long way into the line is punctuation in a
  // sentence, not the end of who is speaking.
  constexpr std::size_t kLongestName = 64;
  if (colon == std::string_view::npos || colon == at || colon - at > kLongestName) {
    return {};
  }
  speaker.name = text.substr(at, colon - at);
  speaker.body = colon + 2;
  return speaker;
}

/// One chat line as the rich text a row is drawn from.
///
/// Everything that is not a URL is escaped, which is the point as much as the
/// links are: this is a line another participant typed, and a row that renders
/// it as markup would let them write the interface rather than write in it.
[[nodiscard]] QString to_html(const QString& line) {
  const QByteArray utf8 = line.toUtf8();
  const std::string_view text(utf8.constData(), static_cast<std::size_t>(utf8.size()));
  const std::vector<client::app::LinkSpan> links = client::app::find_links(text);

  QString html;
  // Both casts matter: QString counts in a signed qsizetype and the vector
  // in an unsigned size_t, so without them the sum is worked out unsigned
  // and narrowed again on the way in.
  html.reserve(line.size() + (static_cast<qsizetype>(links.size()) * 32) + 96);

  // Who said it and when, on a line of their own above what was said: the
  // name in a heavier weight, the time quiet beside it. The line arrives as
  // "[10:51] Bruno: text" - see to_chat_line in ui/main_window.cpp - and is
  // taken apart here only to be drawn; what is kept on the item, selected and
  // copied is still that one line, exactly as it arrived.
  //
  // The classes are resolved by lay_out's stylesheet at every paint, so the
  // colours follow the theme rather than being frozen into the row.
  std::size_t at = 0;
  const Speaker speaker = speaker_of(text);
  const bool link_in_head = !links.empty() && links.front().begin < speaker.body;
  // A line that is not somebody speaking - a nudge, a notice from the room -
  // has no head and is drawn whole, as it always was. So is the rare line
  // whose "name" the link scanner has claimed part of: the spans below are
  // offsets into the whole line, and a head cut out from under one of them
  // would put a link's markup in the wrong place.
  if (speaker.body > 0 && !link_in_head) {
    html += QStringLiteral("<span class=\"who\">%1</span>").arg(escaped(speaker.name));
    if (!speaker.when.empty()) {
      html +=
          QStringLiteral("&nbsp;&nbsp;<span class=\"when\">%1</span>").arg(escaped(speaker.when));
    }
    html += QStringLiteral("<br>");
    at = speaker.body;
  }

  for (const client::app::LinkSpan& link : links) {
    html += escaped(text.substr(at, link.begin - at));
    // Escaped twice over: once for the attribute and once for what is shown.
    // The URL is its own label here, because a chat where the text of a link
    // and its destination can differ is a chat that can be used to lie about
    // where a click goes.
    const QString url = escaped(text.substr(link.begin, link.end - link.begin));
    html += QStringLiteral("<a href=\"%1\">%2</a>").arg(url, url);
    at = link.end;
  }
  html += escaped(text.substr(at));
  return html;
}

/// How wide a row's text may be before it wraps.
///
/// Taken from the viewport and not from the option's rectangle, because
/// sizeHint is asked before there is a rectangle and would otherwise wrap the
/// text at a different width than paint does - which is a row whose height
/// does not match its contents.
[[nodiscard]] int text_width(const QWidget* viewport) {
  if (viewport == nullptr) {
    return 1;
  }
  return std::max(1, viewport->width() - (2 * kHorizontalPadding));
}

void lay_out(QTextDocument& document, const QFont& font, const QString& html, int width) {
  document.setDefaultFont(font);
  // The margin is this file's business, not the document's.
  document.setDocumentMargin(0);
  // Before setHtml, which is when the sheet is applied. The accent is the same
  // indigo the rest of the interface uses for the thing to press, and it
  // follows the system's light and dark setting with everything else.
  //
  // The two classes are the head of a spoken line: see to_html. Set here and
  // not written into the row, so that a change of theme reaches the lines
  // already on screen.
  const theme::Colors& colours = theme::colors();
  document.setDefaultStyleSheet(
      QStringLiteral("a { color: %1; text-decoration: underline; }"
                     ".who { font-weight: 600; }"
                     ".when { color: %2; font-size: %3pt; }")
          .arg(colours.accent_text.name(), colours.muted.name())
          .arg(theme::font(theme::TextRole::Caption).pointSizeF(), 0, 'f', 1));
  document.setHtml(html);
  document.setTextWidth(width);
}

/// Draws a chat row, and answers which URL is under a point in it.
///
/// Both from the same laid-out document, which is the only way the answer and
/// the picture can be guaranteed to agree.
class ChatLineDelegate : public QStyledItemDelegate {
 public:
  explicit ChatLineDelegate(ChatView* view) : QStyledItemDelegate(view), view_(view) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    QStyleOptionViewItem styled = option;
    initStyleOption(&styled, index);

    // The background, the selection and the focus rectangle still come from
    // the style; the text does not. Clearing it is what stops the style from
    // painting a plain copy underneath the rich one.
    const QString plain = styled.text;
    styled.text.clear();
    QStyle* style = styled.widget != nullptr ? styled.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);

    const int width = text_width(view_ != nullptr ? view_->viewport() : nullptr);
    QTextDocument document;
    lay_out(document, styled.font, html_of(index, plain), width);

    painter->save();
    painter->translate(styled.rect.topLeft() + QPoint(kHorizontalPadding, kVerticalPadding));
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = styled.palette;
    if ((styled.state & QStyle::State_Selected) != 0) {
      context.palette.setColor(QPalette::Text, styled.palette.color(QPalette::HighlightedText));
    }
    context.clip = QRectF(0, 0, width, document.size().height());
    document.documentLayout()->draw(painter, context);
    painter->restore();
  }

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override {
    QStyleOptionViewItem styled = option;
    initStyleOption(&styled, index);

    const int width = text_width(view_ != nullptr ? view_->viewport() : nullptr);
    QTextDocument document;
    lay_out(document, styled.font, html_of(index, styled.text), width);
    return {width + (2 * kHorizontalPadding),
            static_cast<int>(document.size().height()) + (2 * kVerticalPadding)};
  }

  /// The href under `position`, which is in the row's own coordinates, or an
  /// empty string when the point is not inside a link.
  [[nodiscard]] QString anchor_at(const QModelIndex& index, const QFont& font,
                                  const QPointF& position) const {
    const int width = text_width(view_ != nullptr ? view_->viewport() : nullptr);
    QTextDocument document;
    lay_out(document, font, html_of(index, index.data(Qt::DisplayRole).toString()), width);
    return document.documentLayout()->anchorAt(position);
  }

 private:
  /// The rich text ChatView::append_line put on the item.
  ///
  /// `fallback` covers the row that got there another way - a plain addItem
  /// from code that has not been converted - by drawing it as escaped text
  /// with no links rather than as nothing at all.
  [[nodiscard]] static QString html_of(const QModelIndex& index, const QString& fallback) {
    const QVariant stored = index.data(ChatView::kHtmlRole);
    if (stored.isValid() && !stored.toString().isEmpty()) {
      return stored.toString();
    }
    return fallback.toHtmlEscaped();
  }

  ChatView* view_ = nullptr;
};

}  // namespace

ChatView::ChatView(QWidget* parent) : QListWidget(parent) {
  setItemDelegate(new ChatLineDelegate(this));
  // Both of them: the pointer has to change while no button is held, and the
  // viewport is what actually receives the moves.
  setMouseTracking(true);
  viewport()->setMouseTracking(true);
}

void ChatView::append_line(const QString& line) {
  // Parented to the view, which is what appends it.
  auto* item = new QListWidgetItem(line, this);
  // The plain text stays on the display role. It is what a selection copies
  // and what a screen reader is given, and neither of those wants markup.
  item->setData(kHtmlRole, to_html(line));
}

void ChatView::set_lines(const QStringList& lines) {
  clear();
  for (const QString& line : lines) {
    append_line(line);
  }
}

QString ChatView::link_at(const QPoint& position) const {
  const QModelIndex index = indexAt(position);
  if (!index.isValid()) {
    return {};
  }
  const auto* delegate = dynamic_cast<const ChatLineDelegate*>(itemDelegateForIndex(index));
  if (delegate == nullptr) {
    return {};
  }
  const QPoint origin = visualRect(index).topLeft() + QPoint(kHorizontalPadding, kVerticalPadding);
  return delegate->anchor_at(index, font(), QPointF(position - origin));
}

void ChatView::refresh_hover(const QPoint& position, Qt::KeyboardModifiers modifiers) {
  const QString link = link_at(position);
  if (link.isEmpty()) {
    viewport()->unsetCursor();
    viewport()->setToolTip(QString());
    return;
  }

  // The hand only while Ctrl is held, because only then does a click do
  // anything here. A pointer that promises a link over a click that selects a
  // row instead is worse than no pointer at all.
  if ((modifiers & Qt::ControlModifier) != 0) {
    viewport()->setCursor(Qt::PointingHandCursor);
  } else {
    viewport()->unsetCursor();
  }
  // The tooltip is shown whether Ctrl is held or not: it is where somebody
  // finds out that the modifier is what opens it.
  viewport()->setToolTip(QStringLiteral("%1\nCtrl+click to open").arg(link));
}

void ChatView::refresh_hover_where_the_pointer_is(Qt::KeyboardModifiers modifiers) {
  refresh_hover(viewport()->mapFromGlobal(QCursor::pos()), modifiers);
}

void ChatView::mouseMoveEvent(QMouseEvent* event) {
  refresh_hover(event->position().toPoint(), event->modifiers());
  QListWidget::mouseMoveEvent(event);
}

void ChatView::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier) != 0) {
    const QString link = link_at(event->position().toPoint());
    // Checked here and not only in the parser. client::app::find_links can only have
    // produced http or https, but this is the single line in the client that
    // hands a string another participant wrote to the system shell, and a
    // scheme check at the door costs nothing to keep.
    const QUrl url(link, QUrl::StrictMode);
    if (!link.isEmpty() && url.isValid() &&
        (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
      QDesktopServices::openUrl(url);
      // Not passed on. Ctrl and a click is also how this list adds a row to
      // the selection, and doing both would leave a row selected every time
      // somebody opened a link.
      event->accept();
      return;
    }
  }
  QListWidget::mousePressEvent(event);
}

void ChatView::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Control) {
    // The event's own modifiers do not yet include the key that is going down.
    refresh_hover_where_the_pointer_is(event->modifiers() | Qt::ControlModifier);
  }
  QListWidget::keyPressEvent(event);
}

void ChatView::keyReleaseEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Control) {
    refresh_hover_where_the_pointer_is(event->modifiers() & ~Qt::ControlModifier);
  }
  QListWidget::keyReleaseEvent(event);
}

void ChatView::leaveEvent(QEvent* event) {
  viewport()->unsetCursor();
  viewport()->setToolTip(QString());
  QListWidget::leaveEvent(event);
}

void ChatView::resizeEvent(QResizeEvent* event) {
  QListWidget::resizeEvent(event);
  // Every row's height is a function of how wide the view is, because that is
  // where its text wraps. All the cached hints are now answers to the old
  // width.
  scheduleDelayedItemsLayout();
}

}  // namespace dv::ui
