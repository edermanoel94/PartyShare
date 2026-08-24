#pragma once

#include <QDialog>
#include <QElapsedTimer>

#include "app/call_session.hpp"
#include "app/metrics_history.hpp"

class QLabel;
class QTimer;

namespace dv::ui {

class MetricsChart;

/// The call's numbers over time: what the network is carrying, and the three
/// measurements that decide whether it is carrying it well.
///
/// A window of its own and not a page, because it is read *during* a call and
/// a page would take the call off the screen to show numbers about it. Not
/// modal either, for the same reason: the controls have to stay reachable
/// while this is open.
///
/// The status bar already says "network good" in one word. This is the same
/// judgement with its working shown - the word comes from three measurements,
/// and when it turns amber the only useful question is which of the three did
/// it, which one word cannot answer.
///
/// It reads the session directly rather than waiting for the metrics callback.
/// That callback fires every five seconds, which is the right cadence for a
/// line in a status bar and far too slow for a chart: twelve points a minute
/// is not a trace of a call, it is twelve dots. The media layer refreshes its
/// own figures five times a second, so this asks at that rate and gets a fresh
/// answer every time.
class MetricsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit MetricsDialog(client::app::CallSession& session, QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private slots:
  /// Takes one reading and hands it to the charts.
  void poll();
  /// Moves the drawing on by one frame.
  void animate();

  // Not redundant: the section above is `private slots:`, which Qt's moc needs
  // as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  /// Rebuilds every chart's points out of the history.
  void hand_over();

  /// Says which of the three measurements is deciding the verdict.
  void show_verdict(const client::media::AudioStats& stats);

  client::app::CallSession& session_;
  client::app::MetricsHistory history_;

  /// One clock for the whole window. Four charts each reading their own would
  /// put the same reading at four slightly different places on four axes that
  /// are meant to be read against each other.
  QElapsedTimer clock_;
  qint64 drawn_at_ms_ = 0;

  QTimer* poller_ = nullptr;
  QTimer* frames_ = nullptr;

  MetricsChart* network_ = nullptr;
  MetricsChart* round_trip_ = nullptr;
  MetricsChart* jitter_ = nullptr;
  MetricsChart* loss_ = nullptr;

  QLabel* verdict_ = nullptr;
  /// The verdict on screen, so the label is only restyled when it changes.
  /// Setting a stylesheet re-resolves the widget's whole style, and this runs
  /// five times a second.
  int shown_quality_ = -1;
};

}  // namespace dv::ui
