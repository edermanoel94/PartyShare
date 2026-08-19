#pragma once

#include <atomic>
#include <mutex>

#include <QImage>
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

 private slots:
  void take_pending_frame();

 private:
  std::mutex mutex_;
  QImage pending_;
  QImage current_;
  /// True while an invocation is already on its way to the interface thread.
  /// Without it a busy event loop would collect one queued call per frame.
  std::atomic<bool> delivery_pending_{false};
  QString placeholder_;
};

}  // namespace dv::ui
