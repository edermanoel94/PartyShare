#pragma once

#include <QDialog>
#include <QString>

class QFormLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QShowEvent;
class QVBoxLayout;

namespace dv::ui {

/// The frame every dialog of the program stands in: a title, an optional
/// sentence under it, the content, and a row of buttons at the bottom right
/// with the one that answers the dialog last.
///
/// A subclass, or a caller, fills `body()` and asks for its buttons; the
/// margins, the order of the buttons and the way the window arrives are the
/// same everywhere because they are decided here once.
class AppDialog : public QDialog {
  Q_OBJECT

 public:
  explicit AppDialog(const QString& title, QWidget* parent = nullptr);

  /// The sentence under the title: what the dialog is asking, or the one
  /// consequence worth knowing before answering it.
  void set_description(const QString& text);

  /// Where the content goes.
  [[nodiscard]] QVBoxLayout* body() const { return body_; }

  /// The button that answers the dialog: filled, the default, last in the
  /// row. It accepts the dialog unless the caller disconnects it.
  QPushButton* add_primary(const QString& text);
  /// Any other button, before the primary one. The first of these rejects
  /// the dialog, as Esc does; connect the others yourself.
  QPushButton* add_secondary(const QString& text);

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  QLabel* description_ = nullptr;
  QVBoxLayout* body_ = nullptr;
  QHBoxLayout* buttons_ = nullptr;
  bool has_secondary_ = false;
};

/// A column of labelled fields at the spacing every form in the program
/// uses, with the two things forms keep needing: a muted sentence under a
/// field, and one line at the bottom for what is wrong.
///
/// Holds fields and says nothing about what they mean. Validation is the
/// caller's; this only has somewhere to put the result.
class AppForm : public QWidget {
  Q_OBJECT

 public:
  explicit AppForm(QWidget* parent = nullptr);

  void add_row(const QString& label, QWidget* field);
  /// A widget across both columns: a check box, a button.
  void add_wide(QWidget* widget);
  /// A muted sentence across both columns. Handed back so it can be changed
  /// or hidden.
  QLabel* add_hint(const QString& text);

  /// Says what is wrong, under everything else. Empty takes the line away
  /// rather than leaving a blank one - a form that keeps a row for an error
  /// it does not have jumps the first time it gets one.
  void show_error(const QString& text);

  [[nodiscard]] QFormLayout* layout_form() const { return form_; }

 private:
  QFormLayout* form_ = nullptr;
  QLabel* error_ = nullptr;
};

}  // namespace dv::ui
