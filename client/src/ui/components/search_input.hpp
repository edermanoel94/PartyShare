#pragma once

#include <QLineEdit>
#include <QString>

class QPaintEvent;

namespace dv::ui {

/// A line to type a filter or a search into: a QLineEdit with a magnifying
/// glass in front of the text and a way to empty it in one click.
///
/// It reports what was typed through QLineEdit's own textChanged and
/// returnPressed; what is searched, and how, belongs to whoever listens.
class SearchInput : public QLineEdit {
  Q_OBJECT

 public:
  explicit SearchInput(const QString& placeholder, QWidget* parent = nullptr);
  /// With no placeholder, and a parent that has to be given: the constructor
  /// Qt Designer calls for a widget promoted to this class. Set the
  /// placeholder and the accessible name in the form.
  explicit SearchInput(QWidget* parent);

 protected:
  void paintEvent(QPaintEvent* event) override;
};

}  // namespace dv::ui
