#pragma once

#include <atomic>
#include <mutex>

#include <QImage>
#include <QPainterPath>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include "video/video_frame.hpp"

class QLabel;
class QMouseEvent;
class QTimer;

namespace dv::ui {

/// Draws the screen somebody else is sharing.
///
/// Frames arrive from a media thread thirty times a second and Qt widgets may
/// only be touched from the thread that owns them, so this keeps exactly one
/// pending frame and asks the interface thread to come and get it. Posting
/// every frame instead would queue three and a half megabytes at a time into
/// an event loop that may be busy, and the picture would drift behind reality
/// with no way to catch up.
///
/// Two buffers, swapped rather than allocated. The frame the interface thread
/// finishes with goes back to the media thread to be filled again, so a call
/// claims its memory once and a change of resolution is the only thing that
/// claims any more.
class ScreenView : public QWidget {
  Q_OBJECT

 public:
  explicit ScreenView(QWidget* parent = nullptr);

  /// Safe to call from any thread. Takes the frame and returns immediately.
  void submit(const client::video::VideoFrame& frame);

  /// Whether a share is running and its frames are worth drawing.
  ///
  /// Turning it off drops what is on screen and goes back to the placeholder,
  /// and - the reason this is a gate rather than a clear() - refuses whatever
  /// the decoder still has in hand. The remote video track is not taken down
  /// when a share ends: it carries whoever holds the floor rather than one
  /// participant, so it stays for the length of the call, and the frames
  /// buffered before the share stopped are decoded after it. Without the gate
  /// the last of them repaints the panel a moment after it was cleared, and
  /// that picture then stays there for the rest of the call with nothing
  /// coming to replace it.
  ///
  /// Read under the same mutex that submit() writes the frame under, so a
  /// frame is either drawn before the share ends or not at all. A clear()
  /// alone cannot promise that: it is a moment, and the media thread is free
  /// to arrive one instruction later.
  void set_receiving(bool receiving);

  /// What to say when nothing is being shared.
  void set_placeholder(QString text);

  /// Whether the picture is the whole screen.
  ///
  /// On, the rounded card goes and so does the window-coloured surround: the
  /// letterbox is black to the edges, which is what a screen shown full screen
  /// is expected to look like and the only thing that makes the shared
  /// picture's own edges readable against it. Off is the card in the room
  /// page. Nothing about the frames changes; this is paint, not layout.
  void set_edge_to_edge(bool on);

  /// Puts one sentence over the picture for a few seconds and takes it away
  /// again - "Press Esc to leave full screen", the one thing a person entering
  /// full screen has to be told and cannot be shown any other way, since
  /// everything that could show it has just been hidden.
  ///
  /// A child label rather than part of paintEvent, so it costs the frame path
  /// nothing: take_pending_frame goes on dirtying the picture's own rectangle
  /// and Qt composites the label over it. Calling it again restarts the clock.
  void show_notice(const QString& text);

 signals:
  /// Double-clicked. What that asks for - full screen, today - is the
  /// window's to decide; this widget only says it happened.
  void activated();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private slots:
  void take_pending_frame();

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  /// Works out where the picture goes, and answers whether that moved.
  ///
  /// Its own step because the answer decides how much has to be repainted. A
  /// frame arriving into a layout that has not changed only dirties the
  /// picture's own rectangle; one that lands somewhere else has to take the
  /// letterbox around it with it, or the last frame's edges stay on screen.
  bool place_frame();

  /// Works out the corner slivers for the size the widget is now.
  void rebuild_card();

  /// Puts the notice where it goes for the size the widget is now: centred,
  /// a little down from the top, where it covers the least of the picture.
  void place_notice();

  /// See set_edge_to_edge. Read on the interface thread only, in paintEvent.
  bool edge_to_edge_ = false;
  /// The sentence show_notice puts up, hidden until then and again after
  /// `notice_timer_` runs out. Transparent to the mouse, so a double click
  /// through it still reaches this widget.
  QLabel* notice_ = nullptr;
  QTimer* notice_timer_ = nullptr;

  std::mutex mutex_;
  QImage pending_;
  QImage current_;
  /// False until a share somebody else is running is announced. Guarded by
  /// `mutex_` rather than atomic, because the point of it is to be read in the
  /// same critical section that takes the frame.
  bool receiving_ = false;
  /// True while an invocation is already on its way to the interface thread.
  /// Without it a busy event loop would collect one queued call per frame.
  std::atomic<bool> delivery_pending_{false};
  QString placeholder_;

  /// Where the picture is drawn.
  QRect where_;
  /// The four corner slivers the rounded card is missing, filled back in with
  /// the window colour after the picture is drawn over them.
  ///
  /// Kept rather than clipped to. Setting this as a clip path would make Qt
  /// rasterise every frame through it; painting it back over four small
  /// slivers costs the slivers. Recomputed when the size changes, which is the
  /// only thing that changes it.
  QPainterPath outside_;
  /// The size `outside_` was worked out for. A widget that is laid out and
  /// shown without ever being resized gets no resizeEvent, and a corner path
  /// built for a size the widget no longer has cuts the picture in the wrong
  /// place.
  QSize card_for_;
};

}  // namespace dv::ui
