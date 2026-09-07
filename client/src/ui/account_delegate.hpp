#pragma once

#include <QStyledItemDelegate>
#include <Qt>

class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace dv::ui {

/// What a cell of the account table is, so the delegate knows how to draw it.
/// Carried on the item under `kAccountCellRole`, because the delegate is one
/// object for the whole table and a column number says nothing on its own.
enum class AccountCell : int {
  /// Drawn by the style: text in the table's font.
  Plain = 0,
  /// A dot: filled green when the account is signed in, an outline when not.
  /// The item's `kAccountOnlineRole` says which.
  Status = 1,
  /// The role, in small capitals, in the accent when it is an administrator.
  Role = 2,
  /// One chip per restriction, from the bits under `kAccountFlagsRole`.
  Restrictions = 3,
};

constexpr int kAccountCellRole = Qt::UserRole + 1;
constexpr int kAccountOnlineRole = Qt::UserRole + 2;
constexpr int kAccountFlagsRole = Qt::UserRole + 3;

/// The four restrictions as bits, in the order models::Restrictions declares
/// them. Plain constants rather than an enum for the reason the participant
/// state is: a set of flags is not a choice of one value.
inline constexpr int kAccountBanned = 1 << 0;
inline constexpr int kAccountMuted = 1 << 1;
inline constexpr int kAccountSilenced = 1 << 2;
inline constexpr int kAccountScreenBlocked = 1 << 3;

/// Draws a row of the administrator's account table so that it reads at a
/// glance: a dot for online, the role in colour, the restrictions as chips.
///
/// The words are still there for whoever cannot see the colours - the same
/// text is in the tooltip and is what the filter searches - but a column of
/// "yes" and a sentence of flag names are things to read, and a table of
/// forty accounts is a thing to scan.
class AccountDelegate : public QStyledItemDelegate {
 public:
  explicit AccountDelegate(QObject* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override;
};

}  // namespace dv::ui
