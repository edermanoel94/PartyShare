#pragma once

#include <QJsonObject>
#include <QString>
#include <QUrl>

/// What the release check reads out of GitHub's answer, as functions of the
/// document alone: no request is made here and nothing is remembered. They are
/// apart from ui::UpdateChecker, which owns the request, so that a test can
/// hand them a document and look at what came back.
namespace dv::ui {

/// Whether a link out of a release document may be handed to a browser.
///
/// Every one of them ends as an argument to the platform's "open this", so it
/// is a string from the network that becomes a launched process. https and
/// github.com is the whole of what this feature ever needs, and anything else -
/// another host, another scheme, `file:`, `javascript:` - is refused.
[[nodiscard]] bool is_safe_release_link(const QUrl& url);

/// The file that installs `version` over *this build*, named the way
/// cmake/Packaging.cmake and the platform scripts name it:
/// `partyshare-0.1.42-windows-x64.msi` on the build the .msi installs. Empty
/// on a platform no installer is published for, where there is nothing to
/// download directly.
///
/// The build's own architecture and not the machine's: an x64 client running
/// under emulation is updated by the x64 installer it came from.
[[nodiscard]] QString installer_name(const QString& version);

/// The address the file called exactly `name` downloads from, out of a release
/// document's `assets`.
///
/// Invalid when there is none: the release carries no such file, `name` is
/// empty, or the address it gives is one is_safe_release_link refuses. A
/// release is published before its files are attached, so "none yet" is an
/// ordinary answer and the caller falls back to the release's page.
///
/// The whole name and not the way it ends. The list also holds the portable
/// .zip and the server's tarball, and a file somebody attaches by hand later
/// should have to be the installer, not resemble it, to be offered as one.
[[nodiscard]] QUrl find_asset(const QJsonObject& release, const QString& name);

}  // namespace dv::ui
