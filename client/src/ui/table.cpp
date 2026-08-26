#include "ui/table.hpp"

#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QWidget>

namespace dv::ui {

QTableWidget* make_table(const QStringList& headers, QWidget* parent) {
  auto* table = new QTableWidget(0, static_cast<int>(headers.size()), parent);
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
}

void fill(QTableWidget* table, const QStringList& rows) {
  const QString selected =
      table->currentRow() >= 0 && table->item(table->currentRow(), 0) != nullptr
          ? table->item(table->currentRow(), 0)->data(kIdRole).toString()
          : QString();

  table->setRowCount(static_cast<int>(rows.size()));
  for (int row = 0; row < rows.size(); ++row) {
    const QStringList fields = rows.at(row).split(QLatin1Char('\t'));
    for (int column = 0; column < table->columnCount(); ++column) {
      // The identifier is field 0, so the columns start at 1.
      const QString text = fields.value(column + 1);
      auto* item = new QTableWidgetItem(text);
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      // The full text, for the cells that are too narrow to show it. A room
      // name is as long as somebody made it and an audit detail is a sentence,
      // and both land in a column sized by the panel around them: without this
      // the only way to read "Reunião de sexta às..." is to widen the window.
      // Harmless on a cell that fits, where the tooltip says what is already
      // on screen.
      if (!text.isEmpty()) {
        item->setToolTip(text);
      }
      if (column == 0) {
        item->setData(kIdRole, fields.value(0));
      }
      table->setItem(row, column, item);
    }
    // Restored by identity rather than by row: the order changes as accounts
    // and rooms come and go, and a selection that jumps to a different account
    // between a refresh and a click is how the wrong person gets deleted.
    //
    // This matters more than it used to. The room list is now pushed by the
    // server whenever a room appears or is closed, so a refresh can land
    // between somebody reading a row and clicking it, without them having
    // asked for anything.
    if (!selected.isEmpty() && fields.value(0) == selected) {
      table->selectRow(row);
    }
  }
}

QString selected_id(const QTableWidget* table) {
  const int row = table->currentRow();
  if (row < 0 || table->item(row, 0) == nullptr) {
    return {};
  }
  return table->item(row, 0)->data(kIdRole).toString();
}

}  // namespace dv::ui
