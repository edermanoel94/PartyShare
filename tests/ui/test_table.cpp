// The listings every page of the administration panel is made of. What is
// promised here is the cost of refilling one: a table that sizes its columns
// to what they hold must not measure the whole column again for every cell
// that is set.
//
// It did. Each setItem tells the view the cell changed; the view asks where
// that cell is; and a header with a column to be sized to its contents answers
// by sizing it first, which reads every row of the column. A cell at a time,
// that is every row read once per cell: on a server up for two months, with
// 156 sessions in the Sessions tab, the window stood still for eight seconds
// each time the tab was opened, and anything clicked in the meantime - the
// Audit tab, usually - looked like the thing that froze it.

#include <memory>

#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QHeaderView>
#include <QModelIndex>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>

#include "ui/table.hpp"
#include "ui_test.hpp"

namespace {

/// A delegate that counts how often the view measures a cell, which is the
/// unit the cost above is made of.
class CountingDelegate final : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override {
    ++measured;
    return QStyledItemDelegate::sizeHint(option, index);
  }

  mutable long long measured = 0;
};

constexpr int kRows = 200;

/// The shape of the Sessions table: every column sized to its contents but
/// one, which takes the slack.
struct SizedTable {
  SizedTable() : table(dv::ui::make_table(headers(), nullptr)) {
    table->setItemDelegate(delegate);
    QHeaderView* columns = table->horizontalHeader();
    columns->setStretchLastSection(false);
    columns->setSectionResizeMode(QHeaderView::ResizeToContents);
    columns->setSectionResizeMode(2, QHeaderView::Stretch);
    table->resize(900, 600);
    // Shown, because that is when it costs: a view that is not on screen
    // leaves the updates for later, and the tab in front is the one on screen.
    table->show();
    dv::ui::testing::settle();
  }

  [[nodiscard]] static QStringList headers() {
    return {QStringLiteral("Account"), QStringLiteral("State"), QStringLiteral("Address"),
            QStringLiteral("Last seen"), QStringLiteral("Connected")};
  }

  [[nodiscard]] static QStringList rows(int count) {
    QStringList rows;
    for (int row = 0; row < count; ++row) {
      rows.push_back(QStringLiteral("id%1\tedermanoel\tended\t179.218.9.93:%1\t"
                                    "2026-09-02 23:40:38\t2026-09-02 22:58:45")
                         .arg(7000 + row));
    }
    return rows;
  }

  std::unique_ptr<QTableWidget> table;
  CountingDelegate* delegate = new CountingDelegate(table.get());
};

}  // namespace

TEST(Table, RefillingMeasuresEachCellAHandfulOfTimesNotOncePerRow) {
  SizedTable sized;
  // The first fill is cheap whatever happens: adding the rows leaves the view
  // a layout to do later, and a view with a layout pending does not ask where
  // a changed cell is. Every answer after that one is a refill of a table the
  // same length - the panel asks again each time a tab is brought forward -
  // and that is where each cell cost a column.
  dv::ui::fill(sized.table.get(), SizedTable::rows(kRows));
  dv::ui::testing::settle();
  sized.delegate->measured = 0;

  dv::ui::fill(sized.table.get(), SizedTable::rows(kRows));
  dv::ui::testing::settle();

  // Sizing four columns once reads each of their cells once, and a paint
  // reads what is on screen: a few times the number of cells at most. Sizing
  // them again for every cell set reads each of them once per cell, which for
  // two hundred rows is a thousand times over.
  const long long cells = static_cast<long long>(kRows) * SizedTable::headers().size();
  EXPECT_LT(sized.delegate->measured, cells * 4)
      << "refilling " << kRows << " rows measured " << sized.delegate->measured
      << " cells: the columns are being sized again for every cell that is set";
}

TEST(Table, ColumnsAreStillSizedToWhatTheyHoldAfterARefill) {
  SizedTable sized;
  dv::ui::fill(sized.table.get(), SizedTable::rows(kRows));
  dv::ui::testing::settle();
  dv::ui::fill(sized.table.get(), SizedTable::rows(kRows));
  dv::ui::testing::settle();

  QHeaderView* columns = sized.table->horizontalHeader();
  // The modes are the ones the page asked for, and not whatever the refill
  // used while it ran.
  EXPECT_EQ(columns->sectionResizeMode(0), QHeaderView::ResizeToContents);
  EXPECT_EQ(columns->sectionResizeMode(2), QHeaderView::Stretch);
  EXPECT_EQ(columns->sectionResizeMode(4), QHeaderView::ResizeToContents);
  // And the sizing happened: a date and a time do not fit the width of the
  // word "Connected" in its header.
  const int connected =
      sized.table->fontMetrics().horizontalAdvance(QStringLiteral("2026-09-02 22:58:45"));
  EXPECT_GE(columns->sectionSize(4), connected)
      << "the Connected column is narrower than the times it holds";
}
