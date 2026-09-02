#pragma once

#include <string>

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkReply;
class QTimer;

namespace dv::ui {

/// Asks GitHub whether a release newer than this build has been published, and
/// says so once, quietly.
///
/// What it is for: this program updates by somebody downloading an installer,
/// so a version that has shipped reaches nobody until they hear about it. Three
/// people in one room running three versions is the failure that follows, and
/// it is invisible from inside - the protocol has no version negotiation, so
/// what an old client does with a message it does not know is nothing at all.
///
/// What it deliberately is not: an updater. Nothing is downloaded, nothing is
/// executed, and nothing is written to disk. The whole result is a link in the
/// status bar, which is the smallest thing that can carry the news and the
/// largest thing that can be done without asking somebody's permission to
/// replace a signed binary on their machine.
///
/// Everything here fails silently. A machine with no route to the internet, a
/// proxy that refuses, a Qt without a TLS backend, GitHub answering 403 because
/// the address ran out of anonymous requests - each of those is a check that
/// did not happen and is forgotten until the next one. None of them is worth a
/// dialog, and none of them may delay the window appearing.
///
/// Silently to the person, that is, and not to the log: see report() for why
/// the first outcome of a run is written at info and the rest are not.
class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  explicit UpdateChecker(QObject* parent = nullptr);

  /// Turns the check on or off.
  ///
  /// Off is the state one of these is born in, so main() switching it on from
  /// `[ui] check_for_updates` is the only way a request is ever made. On, it
  /// checks once shortly afterwards and then every few hours; off, the timer
  /// stops, a request already in flight is ignored when it lands, and nothing
  /// new is sent.
  ///
  /// The settings dialog calls this as the box is ticked, before anything is
  /// written to config.ini - the same arrangement ui::set_chimes_enabled has,
  /// and for the same reason: unticking a box because you want something to
  /// stop should stop it now rather than at the next launch.
  ///
  /// Switching it on schedules a check on the same short delay startup uses,
  /// rather than waiting for the next six hourly beat. Somebody who has just
  /// ticked the box is somebody asking the question.
  void set_enabled(bool on);

  /// Whether a check would be made. What the settings dialog shows.
  [[nodiscard]] bool enabled() const { return enabled_; }

 signals:
  /// A release newer than this build exists.
  ///
  /// `version` is the three numbers without the tag's `v`, ready to be shown.
  /// `page` is where a person goes to get it, and is guaranteed to be an https
  /// URL on github.com: it arrives inside a JSON document from the network and
  /// is handed to a widget that opens a browser, which is a short enough path
  /// from someone else's bytes to a launched process to be worth checking.
  ///
  /// Raised at most once per version, however many times the check runs.
  void update_available(const QString& version, const QUrl& page);

 private slots:
  void check();

 private:
  void handle(QNetworkReply* reply);

  /// Writes down what a check concluded: at info the first time in a run, at
  /// debug every time after that.
  ///
  /// The split is not tidiness, it is the only way the outcome is visible at
  /// all. shared/CMakeLists.txt compiles with SPDLOG_ACTIVE_LEVEL at info in
  /// every build that is not Debug, so DV_LOG_DEBUG is *removed by the
  /// preprocessor* in the binaries people install - `--log-level=debug` cannot
  /// bring it back. Logging every outcome at debug would therefore mean that in
  /// a released client "the check ran and found nothing" and "the check never
  /// ran" produce the same empty log, which is exactly the question support
  /// gets asked.
  ///
  /// One line at info per run answers it. The ones after it are for somebody
  /// running a Debug build who wants the whole story, and cost a machine with
  /// no route out one line per run rather than one every six hours.
  void report(const std::string& outcome);

  QNetworkAccessManager network_;
  QTimer* timer_ = nullptr;
  bool enabled_ = false;
  /// The version already announced, so that a six hourly check does not raise
  /// the same news at every beat. Empty until something has been found.
  QString announced_;
  /// Whether anything has been written down yet in this run. See report().
  bool reported_ = false;
};

}  // namespace dv::ui
