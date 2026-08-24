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

  /// Drops what is on screen and goes back to the placeholder.
  void clear();

  /// What to say when nothing is being shared.
  void set_placeholder(QString text);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

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

  std::mutex mutex_;
  QImage pending_;
  QImage current_;
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
