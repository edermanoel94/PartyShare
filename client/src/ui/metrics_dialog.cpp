#include "ui/metrics_dialog.hpp"

#include <deque>
#include <string_view>
#include <utility>
#include <vector>

#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QHideEvent>
#include <QLabel>
#include <QPointF>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "app/network_quality.hpp"
#include "ui/metrics_chart.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// How much history the charts hold. A minute is long enough to show that the
/// last ten seconds were unusual, and short enough that the last ten seconds
/// still take up a sixth of the width.
constexpr double kWindowMs = 60000.0;

/// How often a reading is taken.
///
/// The media layer refreshes its own figures every 200 ms - kLevelInterval in
/// webrtc/libwebrtc_media_session.cpp - so asking faster than this hands back
/// the same numbers again and charts them as though they were new.
constexpr int kPollMs = 200;

/// Thirty frames a second, and only while the window is on screen: hideEvent
/// stops it.
///
/// Sixty was the first answer and it was the wrong one. Nothing here moves
/// fast enough to need it - the fastest thing on the window is a reading
/// easing into place over a fifth of a second - and four charts repainting at
/// sixty cost twenty-two percent of a core against two and a half for the
/// whole rest of the call. Thirty is the floor for movement that reads as
/// movement, and it halves that outright. MetricsChart::advance takes the rest
/// off by skipping the frames on which its own drawing would not change.
constexpr int kFrameMs = 33;

[[nodiscard]] std::vector<std::vector<QPointF>> one_line(std::vector<QPointF> points) {
  // A vector built and returned rather than a braced list at the call site.
  // The elements of an initializer list are const, so a move into one is a
  // copy wearing a std::move, and this runs five times a second.
  std::vector<std::vector<QPointF>> lines;
  lines.push_back(std::move(points));
  return lines;
}

[[nodiscard]] std::vector<std::vector<QPointF>> two_lines(std::vector<QPointF> first,
                                                          std::vector<QPointF> second) {
  std::vector<std::vector<QPointF>> lines;
  lines.reserve(2);
  lines.push_back(std::move(first));
  lines.push_back(std::move(second));
  return lines;
}

}  // namespace

MetricsDialog::MetricsDialog(client::app::CallSession& session, QWidget* parent)
    : QDialog(parent), session_(session), history_(kWindowMs) {
  setWindowTitle(QStringLiteral("Call metrics"));
  // Not modal, and deleted when it is closed. Not modal because the mute and
  // share buttons have to stay reachable while this is open - a window about
  // the call that stops the call being operated is worse than no window.
  // Deleted because what it draws belongs to the call it was opened during,
  // and reopening it should start again rather than resume.
  setModal(false);
  setAttribute(Qt::WA_DeleteOnClose);
  resize(560, 660);
  setMinimumSize(420, 460);

  auto* layout = new QVBoxLayout(this);

  verdict_ = new QLabel(QString{}, this);
  QFont strong = verdict_->font();
  strong.setBold(true);
  verdict_->setFont(strong);

  auto* note = new QLabel(
      QStringLiteral("The worst of the three below is what the indicator in the status bar "
                     "reports. Loss is charted per reading, while the verdict weighs the whole "
                     "call, so a short burst shows up here without moving it."),
      this);
  note->setWordWrap(true);
  note->setProperty("hint", true);

  const theme::Colors& colours = theme::colors();

  network_ = new MetricsChart(QStringLiteral("Audio carried (kbps)"), QStringLiteral("kbps"), this);
  // Two colours chosen to be told apart, not to be read as a verdict: neither
  // direction of a bitrate is good or bad on its own.
  network_->set_lines(
      {{QStringLiteral("↑"), colours.accent}, {QStringLiteral("↓"), colours.success}});

  round_trip_ =
      new MetricsChart(QStringLiteral("Round trip time (ms)"), QStringLiteral("ms"), this);
  round_trip_->set_lines({{QStringLiteral("rtt"), colours.accent}});
  round_trip_->set_thresholds(client::app::kFairRoundTripMs, client::app::kPoorRoundTripMs);

  jitter_ = new MetricsChart(QStringLiteral("Jitter (ms)"), QStringLiteral("ms"), this);
  jitter_->set_lines({{QStringLiteral("jitter"), colours.accent}});
  jitter_->set_thresholds(client::app::kFairJitterMs, client::app::kPoorJitterMs);

  loss_ = new MetricsChart(QStringLiteral("Packet loss (%)"), QStringLiteral("%"), this);
  loss_->set_lines({{QStringLiteral("loss"), colours.accent}});
  loss_->set_thresholds(client::app::kFairLossPercent, client::app::kPoorLossPercent);

  for (MetricsChart* chart : {network_, round_trip_, jitter_, loss_}) {
    chart->set_window(kWindowMs);
  }

  auto* buttons = new QDialogButtonBox(this);
  // Named here rather than taken from QDialogButtonBox::Close, whose label
  // comes from Qt's own translations. See SettingsDialog for the same choice.
  auto* close = buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::AcceptRole);
  close->setProperty("accent", true);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

  layout->addWidget(verdict_);
  layout->addWidget(note);
  layout->addWidget(network_, 1);
  layout->addWidget(round_trip_, 1);
  layout->addWidget(jitter_, 1);
  layout->addWidget(loss_, 1);
  layout->addWidget(buttons);

  poller_ = new QTimer(this);
  poller_->setInterval(kPollMs);
  connect(poller_, &QTimer::timeout, this, &MetricsDialog::poll);

  frames_ = new QTimer(this);
  frames_->setInterval(kFrameMs);
  connect(frames_, &QTimer::timeout, this, &MetricsDialog::animate);

  clock_.start();
  show_verdict(client::media::AudioStats{});
}

void MetricsDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  drawn_at_ms_ = clock_.elapsed();
  poller_->start();
  frames_->start();
  // One reading straight away, so the charts do not open on "waiting" for a
  // fifth of a second. Through the event loop rather than called here: poll()
  // closes this window when there is no call, and closing a window from inside
  // its own showEvent is not a thing to do.
  QTimer::singleShot(0, this, &MetricsDialog::poll);
}

void MetricsDialog::hideEvent(QHideEvent* event) {
  // Repainting four charts nobody can see is the definition of work that is
  // not worth doing. A minimised window gets no hideEvent on every platform,
  // which is why the poll timer stops here as well: the far cheaper of the two
  // is the one that would otherwise keep running.
  frames_->stop();
  poller_->stop();
  QDialog::hideEvent(event);
}

void MetricsDialog::poll() {
  if (session_.state() != client::app::CallSession::State::InCall) {
    // The call is over, and every number here belongs to it. Left open, the
    // charts would slide the last reading off the left edge over the next
    // minute and end up as four empty grids, which reads as a call with
    // nothing wrong with it rather than as no call at all.
    close();
    return;
  }

  const client::media::AudioStats stats = session_.stats();
  history_.observe(stats, static_cast<double>(clock_.elapsed()));
  hand_over();
  show_verdict(stats);
}

void MetricsDialog::hand_over() {
  const std::deque<client::app::MetricsSample>& samples = history_.samples();
  const std::size_t count = samples.size();

  std::vector<QPointF> up;
  std::vector<QPointF> down;
  std::vector<QPointF> round_trip;
  std::vector<QPointF> jitter;
  std::vector<QPointF> loss;
  for (std::vector<QPointF>* series : {&up, &down, &round_trip, &jitter, &loss}) {
    series->reserve(count);
  }

  for (const client::app::MetricsSample& sample : samples) {
    up.emplace_back(sample.at_ms, sample.send_kbps);
    down.emplace_back(sample.at_ms, sample.receive_kbps);
    round_trip.emplace_back(sample.at_ms, sample.round_trip_time_ms);
    jitter.emplace_back(sample.at_ms, sample.jitter_ms);
    loss.emplace_back(sample.at_ms, sample.loss_percent);
  }

  network_->set_points(two_lines(std::move(up), std::move(down)));
  round_trip_->set_points(one_line(std::move(round_trip)));
  jitter_->set_points(one_line(std::move(jitter)));
  loss_->set_points(one_line(std::move(loss)));
}

void MetricsDialog::show_verdict(const client::media::AudioStats& stats) {
  const client::app::NetworkQuality quality = client::app::quality_of(stats);
  const auto measured = static_cast<int>(quality);
  if (measured == shown_quality_) {
    return;
  }
  shown_quality_ = measured;

  const theme::Colors& colours = theme::colors();
  QColor colour = colours.muted;
  switch (quality) {
    case client::app::NetworkQuality::Good:
      colour = colours.success;
      break;
    case client::app::NetworkQuality::Fair:
      colour = colours.warn;
      break;
    case client::app::NetworkQuality::Poor:
      colour = colours.danger;
      break;
    case client::app::NetworkQuality::Unknown:
      break;
  }

  const std::string_view word = client::app::to_string(quality);
  verdict_->setText(
      quality == client::app::NetworkQuality::Unknown
          ? QStringLiteral("● nothing measured yet")
          : QStringLiteral("● network %1")
                .arg(QString::fromUtf8(word.data(), static_cast<qsizetype>(word.size()))));
  verdict_->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
}

void MetricsDialog::animate() {
  const qint64 now = clock_.elapsed();
  const auto elapsed = static_cast<double>(now - drawn_at_ms_);
  drawn_at_ms_ = now;

  const auto at = static_cast<double>(now);
  for (MetricsChart* chart : {network_, round_trip_, jitter_, loss_}) {
    chart->advance(at, elapsed);
  }
}

}  // namespace dv::ui
