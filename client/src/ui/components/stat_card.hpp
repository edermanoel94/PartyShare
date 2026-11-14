#pragma once

#include <cstdint>

#include <QFrame>
#include <QString>

class QLabel;
class QVBoxLayout;

namespace dv::ui {

/// A panel on the surface colour with a hairline round it: the paper a group
/// of related things sits on. What a QGroupBox is without its title.
///
/// Lay the content out in `body()`.
class Card : public QFrame {
  Q_OBJECT

 public:
  explicit Card(QWidget* parent = nullptr);

  [[nodiscard]] QVBoxLayout* body() const { return body_; }

 private:
  QVBoxLayout* body_ = nullptr;
};

/// One number and what it counts: rooms open, people in them, the round trip
/// to the server. Optionally a line under it saying whether that is good.
///
/// It is told what to show and shows it; where the number comes from is the
/// business of whoever owns the card.
class StatCard : public Card {
  Q_OBJECT

 public:
  /// The colour of the status line and of the dot in front of it. The word
  /// carries the meaning and the colour repeats it, never the other way.
  enum class Tone : std::uint8_t { Neutral, Good, Fair, Poor };

  explicit StatCard(const QString& label, QWidget* parent = nullptr);

  void set_value(const QString& value);
  /// The line under the number. Empty takes the line away.
  void set_status(const QString& text, Tone tone = Tone::Neutral);

 private:
  QLabel* value_ = nullptr;
  QLabel* status_ = nullptr;
  /// What set_status was last given, kept so the line can be written again
  /// in the new colours when the theme changes.
  QString status_text_;
  Tone tone_ = Tone::Neutral;
};

}  // namespace dv::ui
