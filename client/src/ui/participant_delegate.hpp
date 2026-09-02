#pragma once

#include <QString>
#include <QStyledItemDelegate>
#include <Qt>

class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace dv::ui {

/// What a participant row says besides the name, as bits on
/// `kParticipantStateRole`. Bits rather than an enum of combinations because
/// sharing is independent of the microphone: somebody can be muted and
/// sharing, or speaking and sharing, and each of those is two icons.
enum ParticipantState : int {
  kParticipantNone = 0,
  /// Their microphone is off, by their own hand or an administrator's.
  kParticipantMuted = 1 << 0,
  /// Their voice is coming through right now.
  kParticipantSpeaking = 1 << 1,
  /// Their audio track is up, whether or not anything is being said on it.
  kParticipantAudioActive = 1 << 2,
  /// Their screen is what everybody is watching.
  kParticipantSharing = 1 << 3,
  /// And what their machine is playing comes with it, which is why their
  /// volume slider is now also the volume of a film. See docs/09-screen-audio.md.
  kParticipantSharingWithSound = 1 << 4,
};

/// The roles a participant row carries. The identifier is the one every
/// context menu and slider acts on; the name is what they say; the state and
/// level are what the delegate draws beside it.
constexpr int kParticipantIdRole = Qt::UserRole;
constexpr int kParticipantNameRole = Qt::UserRole + 1;
constexpr int kParticipantStateRole = Qt::UserRole + 2;
/// How loud they are, as the fraction a meter would show: 0 to 1.
constexpr int kParticipantLevelRole = Qt::UserRole + 3;

/// The words the icons replaced, joined for a tooltip: "muted", "speaking",
/// "sharing with sound". An icon says it faster and a word says it to
/// somebody who cannot see the icon, or does not know it yet.
[[nodiscard]] QString describe_participant_state(int state);

/// Draws a participant row: the name, then a small icon for each thing the
/// state says about them, in the theme's colours.
///
/// A microphone, green and filling with their level while they speak, grey
/// when their track is up and quiet, red and struck through when they are
/// muted. A monitor, in the accent colour, while they share, with sound waves
/// beside it when the share carries audio. Drawn rather than loaded from
/// files so that they follow the colour scheme and the pixel density, and so
/// that the level can move: a list that is redrawn several times a second
/// while somebody talks may as well show it.
class ParticipantDelegate : public QStyledItemDelegate {
 public:
  explicit ParticipantDelegate(QObject* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override;
};

}  // namespace dv::ui
