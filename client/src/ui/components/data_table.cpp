#include "ui/components/data_table.hpp"

#include <QResizeEvent>
#include <QTableWidgetItem>

#include "ui/components/empty_state.hpp"
#include "ui/table.hpp"

namespace dv::ui {

DataTable::DataTable(const QStringList& headers, QWidget* parent) : QTableWidget(parent) {
  set_up_table(this, headers);
}

void DataTable::set_rows(const QStringList& rows) {
  fill(this, rows);
  apply_filter();
}

QString DataTable::selected_id() const {
  return dv::ui::selected_id(this);
}

void DataTable::set_empty_state(Icon icon, const QString& title, const QString& hint) {
  empty_title_ = title;
  empty_hint_ = hint;
  if (empty_ == nullptr) {
    // A child of the viewport, so it sits where the rows would be and under
    // the header rather than over it.
    empty_ = new EmptyState(icon, title, hint, viewport());
    empty_->setGeometry(viewport()->rect());
  }
  apply_filter();
}

void DataTable::set_filter(const QString& text) {
  filter_ = text.trimmed();
  apply_filter();
}

void DataTable::resizeEvent(QResizeEvent* event) {
  QTableWidget::resizeEvent(event);
  if (empty_ != nullptr) {
    empty_->setGeometry(viewport()->rect());
  }
}

void DataTable::apply_filter() {
  int shown = 0;
  for (int row = 0; row < rowCount(); ++row) {
    bool matches = filter_.isEmpty();
    for (int column = 0; !matches && column < columnCount(); ++column) {
      const QTableWidgetItem* cell = item(row, column);
      matches = cell != nullptr && cell->text().contains(filter_, Qt::CaseInsensitive);
    }
    setRowHidden(row, !matches);
    shown += matches ? 1 : 0;
  }

  if (empty_ == nullptr) {
    return;
  }
  // Two different kinds of nothing, and they are not the same sentence: a
  // list with no records, and a list whose records the filter ruled out.
  if (rowCount() > 0 && shown == 0) {
    empty_->set_title(QStringLiteral("Nothing matches \"%1\"").arg(filter_));
    empty_->set_hint(QStringLiteral("Try fewer letters, or clear the filter."));
  } else {
    empty_->set_title(empty_title_);
    empty_->set_hint(empty_hint_);
  }
  empty_->setVisible(shown == 0);
}

}  // namespace dv::ui
