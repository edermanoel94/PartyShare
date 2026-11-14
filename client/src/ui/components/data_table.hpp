#pragma once

#include <QString>
#include <QStringList>
#include <QTableWidget>

#include "ui/components/icons.hpp"

class QResizeEvent;

namespace dv::ui {

class EmptyState;

/// A list of records: read-only, one row selected at a time, each row
/// carrying an identifier that is never a column.
///
/// The table every listing in the program is - rooms, accounts, sessions, the
/// audit log - with the two things each of them otherwise does by hand: it
/// says so when it has nothing to show, and it can be narrowed by typing.
/// The rows are the tab separated strings ui::fill takes, identifier first;
/// what they describe is none of this class's business.
class DataTable : public QTableWidget {
  Q_OBJECT

 public:
  explicit DataTable(const QStringList& headers, QWidget* parent = nullptr);

  /// Replaces every row. Keeps the selection by identifier, not by position,
  /// and applies the filter in force to the new rows. See ui::fill.
  void set_rows(const QStringList& rows);

  /// The identifier of the selected row, or empty.
  [[nodiscard]] QString selected_id() const;

  /// What to say when there are no rows at all.
  void set_empty_state(Icon icon, const QString& title, const QString& hint);

  /// Shows only the rows with `text` somewhere in a cell, whatever its case.
  /// Empty shows everything. Connect a SearchInput's textChanged to this.
  void set_filter(const QString& text);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  /// Hides the rows the filter rules out, and shows or hides the empty state.
  void apply_filter();

  QString filter_;
  EmptyState* empty_ = nullptr;
  /// What set_empty_state was given, kept for when the filter is what leaves
  /// the table empty and the sentence has to change and change back.
  QString empty_title_;
  QString empty_hint_;
};

}  // namespace dv::ui
