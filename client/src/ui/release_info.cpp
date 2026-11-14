#include "ui/release_info.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1String>
#include <QtGlobal>

namespace dv::ui {

bool is_safe_release_link(const QUrl& url) {
  if (!url.isValid() || url.scheme() != QLatin1String("https")) {
    return false;
  }
  const QString host = url.host().toLower();
  return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com"));
}

QString installer_name(const QString& version) {
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
  return QStringLiteral("partyshare-%1-windows-x64.msi").arg(version);
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
  return QStringLiteral("partyshare-%1-macos-arm64.dmg").arg(version);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
  return QStringLiteral("partyshare-%1-linux-x64.AppImage").arg(version);
#else
  (void)version;
  return {};
#endif
}

QUrl find_asset(const QJsonObject& release, const QString& name) {
  if (name.isEmpty()) {
    return {};
  }
  const QJsonArray assets = release.value(QLatin1String("assets")).toArray();
  for (const auto& entry : assets) {
    const QJsonObject asset = entry.toObject();
    if (asset.value(QLatin1String("name")).toString() != name) {
      continue;
    }
    const QUrl address(asset.value(QLatin1String("browser_download_url")).toString());
    // Refused is the end of it, and not "look for another of the same name":
    // a release holds one file per name.
    return is_safe_release_link(address) ? address : QUrl();
  }
  return {};
}

}  // namespace dv::ui
