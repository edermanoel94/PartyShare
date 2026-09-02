#include "ui/update_checker.hpp"

#include <optional>
#include <string>

#include <dv/core/version.hpp>
#include <dv/logging/logger.hpp>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace dv::ui {
namespace {

/// The repository this program is released from.
///
/// Written out rather than derived from anything, because there is nothing to
/// derive it from: the same string is in .github/workflows/tag.yml, which
/// refuses to tag a fork, and both are statements about where the releases a
/// user should install actually come from. A fork that publishes its own
/// releases changes this line.
constexpr const char* kLatestReleaseEndpoint =
    "https://api.github.com/repos/edermanoel94/PartyShare/releases/latest";

/// Where somebody is sent when the answer carries no usable link of its own.
constexpr const char* kReleasesPage = "https://github.com/edermanoel94/PartyShare/releases/latest";

/// Long enough after startup that this is never on the path to the first
/// window. It is not on it anyway - the request is asynchronous - but a socket
/// being opened while Qt is still laying out widgets is time taken from the
/// thread that is doing the laying out.
constexpr int kFirstCheckDelayMs = 5000;

/// Six hours. The thing being watched changes at most a few times a day, and
/// this client is left open for days at a time; anything faster is asking a
/// question whose answer cannot have changed, and anything slower means the
/// person who leaves it running all week hears nothing all week.
constexpr int kRecheckIntervalMs = 6 * 60 * 60 * 1000;

/// Ten seconds, and then the check simply did not happen. The default is no
/// timeout at all, which on a network that black holes outbound traffic leaves
/// a socket open until the process ends.
constexpr int kTimeoutMs = 10000;

/// A release document is a couple of kilobytes. This is two orders of magnitude
/// above that, and exists so that an answer from something that is not GitHub -
/// a captive portal, a proxy serving its own page - cannot be read into memory
/// unbounded.
constexpr qint64 kMaxResponseBytes = 256LL * 1024;

/// Whether a link out of the answer may be handed to a browser.
///
/// The status bar opens this with QDesktopServices, so it is a string from the
/// network that ends as an argument to the shell's "open this" call. https and
/// github.com is the whole of what this feature ever needs, and anything else -
/// another host, another scheme, `file:`, `javascript:` - is refused in favour
/// of the releases page, which is where the person was going anyway.
bool is_safe_release_link(const QUrl& url) {
  if (!url.isValid() || url.scheme() != QLatin1String("https")) {
    return false;
  }
  const QString host = url.host().toLower();
  return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com"));
}

}  // namespace

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent), timer_(new QTimer(this)) {
  connect(&network_, &QNetworkAccessManager::finished, this, &UpdateChecker::handle);
  timer_->setInterval(kRecheckIntervalMs);
  connect(timer_, &QTimer::timeout, this, &UpdateChecker::check);
}

void UpdateChecker::set_enabled(bool on) {
  if (on == enabled_) {
    return;
  }
  enabled_ = on;

  if (!on) {
    timer_->stop();
    // A request already on its way is not cancelled, because cancelling it
    // achieves nothing this does not: check() and handle() both refuse to act
    // while this is off, so the answer arrives, is dropped, and the reply
    // deletes itself. Aborting would be one more failure path for the same
    // outcome.
    DV_LOG_INFO("Update check: off");
    return;
  }

  timer_->start();
  // The first one on its own short delay rather than on the timer's, which
  // would not come round for six hours. At startup that is because the check
  // worth having is the one that happens while somebody is looking at the
  // window they just opened; from the settings dialog it is because somebody
  // who has just ticked the box is asking the question.
  QTimer::singleShot(kFirstCheckDelayMs, this, &UpdateChecker::check);
}

void UpdateChecker::check() {
  // The delayed first check above outlives a box being unticked in the two
  // seconds after it was ticked, so this is not redundant with set_enabled.
  if (!enabled_) {
    return;
  }

  QNetworkRequest request{QUrl(QLatin1String(kLatestReleaseEndpoint))};
  // The two headers GitHub's REST API asks for, and one it insists on: a
  // request with no User-Agent is answered with 403 whoever sends it.
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
  request.setRawHeader("User-Agent", "PartyShare/" DV_VERSION);
  request.setTransferTimeout(kTimeoutMs);
  network_.get(request);
}

void UpdateChecker::report(const std::string& outcome) {
  if (!reported_) {
    reported_ = true;
    DV_LOG_INFO("Update check: {}", outcome);
    return;
  }
  DV_LOG_DEBUG("Update check: {}", outcome);
}

void UpdateChecker::handle(QNetworkReply* reply) {
  // Whatever happens below, this reply is finished with. deleteLater rather
  // than delete, because this runs inside the reply's own finished signal.
  reply->deleteLater();

  // Switched off while this was in flight. Reading it would be harmless and
  // acting on it would not: a notice appearing after somebody turned the
  // feature off is the one thing the switch promises will not happen.
  if (!enabled_) {
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    // Not a warning. A client on a network with no way out would raise one of
    // these every six hours for as long as it runs, and a log full of warnings
    // about a feature nobody asked for is a log people stop reading.
    report(reply->errorString().toStdString());
    return;
  }
  if (reply->bytesAvailable() > kMaxResponseBytes) {
    report("the answer was too large to be a release document");
    return;
  }

  QJsonParseError failure;
  const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &failure);
  if (failure.error != QJsonParseError::NoError || !document.isObject()) {
    report("the answer was not a JSON object");
    return;
  }

  const QJsonObject release = document.object();
  const QString tag = release.value(QLatin1String("tag_name")).toString();
  const std::optional<core::Version> published = core::parse_version(tag.toStdString());
  if (!published) {
    // A tag this cannot read is not an error on anybody's part: it is a release
    // named in a shape this build predates. Nothing is shown, which is the
    // behaviour described in dv/core/version.hpp.
    report("'" + tag.toStdString() + "' is not a version this understands");
    return;
  }

  const core::Version running = core::running_version();
  if (!core::is_update(*published, running)) {
    report(published->to_string() + " is the newest release, and this is " + running.to_string());
    return;
  }

  const QString version = QString::fromStdString(published->to_string());
  if (version == announced_) {
    return;
  }
  announced_ = version;

  QUrl page(release.value(QLatin1String("html_url")).toString());
  if (!is_safe_release_link(page)) {
    page = QUrl(QLatin1String(kReleasesPage));
  }

  // Always at info, and not through report(): a release worth telling somebody
  // about is worth writing down every time it is found, in every build.
  DV_LOG_INFO("Update available: {}, and this is {}. {}", published->to_string(),
              running.to_string(), page.toString().toStdString());
  emit update_available(version, page);
}

}  // namespace dv::ui
