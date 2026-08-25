#pragma once

#include <QString>
#include <QStringList>
#include <Qt>

class QTableWidget;
class QWidget;

namespace dv::ui {

/// Where the identifier of a row lives, the same idea as the participant list.
///
/// A row shows what a person needs to read and carries what the program needs
/// to act on. The identifier is the second thing: it is never a column, so it
/// cannot be sorted on, truncated, or mistaken for something to look at.
constexpr int kIdRole = Qt::UserRole;

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
