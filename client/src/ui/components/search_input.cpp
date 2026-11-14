#include "ui/components/search_input.hpp"

#include <QPaintEvent>
#include <QPainter>
#include <QRectF>

#include "ui/components/icons.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// Where the glass sits in the left padding the stylesheet leaves for it:
/// `QLineEdit[search="true"]` in ui/theme.cpp. The two numbers move together.
constexpr int kGlassLeft = 12;
constexpr int kGlassSide = 15;

constexpr int kHeight = 38;

}  // namespace

SearchInput::SearchInput(const QString& placeholder, QWidget* parent) : QLineEdit(parent) {
  setPlaceholderText(placeholder);
  setProperty("search", true);
  setClearButtonEnabled(true);
  setMinimumHeight(kHeight);
  // The placeholder is the only label a search box has, and a screen reader
  // does not read placeholders on every platform.
  setAccessibleName(placeholder);
}

SearchInput::SearchInput(QWidget* parent) : SearchInput(QString{}, parent) {}

void SearchInput::paintEvent(QPaintEvent* event) {
  QLineEdit::paintEvent(event);
  QPainter painter(this);
  const QRectF glass(kGlassLeft, (height() - kGlassSide) / 2.0, kGlassSide, kGlassSide);
  // The accent while the field has the focus: the same thing the border does,
  // said a second time where the eye already is.
  paint_icon(painter, Icon::Search, glass,
             hasFocus() ? theme::colors().accent_text : theme::colors().muted);
}

}  // namespace dv::ui
