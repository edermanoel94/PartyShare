#pragma once

#include <QHeaderView>
#include <QList>
#include <QString>
#include <QStringList>
#include <Qt>

class QTableWidget;
class QWidget;

namespace dv::ui {

/// Holds a table's columns at the widths they have for as long as it lives,
/// and hands back the ones that were sized to their contents when it goes.
///
/// For a refill. Every cell set tells the view that cell changed, the view
/// asks the header where the cell is, and a header with a column to size to
/// its contents sizes it first - by reading every row of that column. Set a
/// cell at a time, a column is read once per cell: 156 sessions held the
/// window still for eight seconds each time the Sessions tab was opened.
/// Held, the columns are read once, after the last cell, when the modes come
/// back and the header sizes them on its next pass.
///
/// Only a table that already has its rows pays this. Rows being added leave
/// the view a layout to do later, and until it does it does not ask where a
/// changed cell is; a refill of a table the same length leaves it nothing.
class HeldColumns {
 public:
  explicit HeldColumns(QTableWidget* table);
  ~HeldColumns();

  HeldColumns(const HeldColumns&) = delete;
  HeldColumns& operator=(const HeldColumns&) = delete;
  HeldColumns(HeldColumns&&) = delete;
  HeldColumns& operator=(HeldColumns&&) = delete;

 private:
  QHeaderView* columns_;
  /// The mode of every column as the page set it, in column order.
  QList<QHeaderView::ResizeMode> modes_;
};

/// Where the identifier of a row lives, the same idea as the participant list.
///
/// A row shows what a person needs to read and carries what the program needs
/// to act on. The identifier is the second thing: it is never a column, so it
/// cannot be sorted on, truncated, or mistaken for something to look at.
constexpr int kIdRole = Qt::UserRole;

/// Turns `table` into what every listing in the program is: read-only, one
/// row selected at a time, these headers, no grid, rows tall enough to aim at.
/// What make_table does to a new table, for a table somebody else made - a
/// subclass, or one dropped into a form in Qt Designer.
void set_up_table(QTableWidget* table, const QStringList& headers);

/// A read-only, single-selection, row-selecting table with these headers.
[[nodiscard]] QTableWidget* make_table(const QStringList& headers, QWidget* parent);

/// Fills a table from rows of tab separated fields.
///
/// The first field is the identifier and is not shown; the rest are the
/// columns, in order. Rebuilt wholesale on every answer, because the answer is
/// the whole list and merging it into what is on screen would be a second
/// implementation of the truth.
void fill(QTableWidget* table, const QStringList& rows);

/// The identifier of the selected row, or empty when nothing is selected.
[[nodiscard]] QString selected_id(const QTableWidget* table);

}  // namespace dv::ui
