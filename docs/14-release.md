# 14. Release

How a version ships, what it produces, and what is not verified yet.

One rule: **no artifact is ever built on a development machine.** A binary
published from a laptop is a binary whose contents nobody can reproduce, and the
first time that matters is when someone reports a crash in a build that no longer
exists anywhere.

## Cutting a release

Nothing. Merging code into `master` is cutting a release.

`.github/workflows/tag.yml` runs on every push to `master` and does what a person
used to have to remember: work out the next version, write it into `CMakeLists.txt`
and `vcpkg.json`, commit that line back, write the annotated tag, and ask
`release.yml` for the artifacts.

| What was merged | What comes out |
| --- | --- |
| Anything that touches code | The patch: 0.1.0 becomes 0.1.1 |
| A pull request labelled `minor` | 0.1.0 becomes 0.2.0 |
| A pull request labelled `major` | 0.1.0 becomes 1.0.0 |
| Only `.md` files | Nothing. No bump, no tag, no release |

The label is read off the pull request the merge commit belongs to, which the API
answers for a squash merge and a merge commit alike. Patch is the default because
it is the honest answer for most merges. It is a label rather than a commit
message convention because the messages here are narrative sentences, and a
machine-readable prefix would cost more than it buys.

Documentation is left alone for the same reason `ci.yml` ignores it: no install
rule packages a Markdown file and no test reads one, so a merge that changes
nothing else would produce bytes identical to the release before it. Two tags
pointing at the same binaries is a question somebody has to answer later, and
there is no good answer. The filter needs *every* changed file to be Markdown, so
a commit that edits a header and the page documenting it still releases.

**When the automatic number is wrong**, edit the version in the pull request. A
push that already raises `VERSION` is tagged as written rather than raised a
second time, which is how to jump to 2.0.0. `vcpkg.json` carries the same number,
and both have to move together.

**To exercise the pipeline**, use `workflow_dispatch` on `release.yml`. It builds
everything and publishes nothing, because a release without a tag has no version
to be.

## What the tag produces

| Artifact | Platform | State |
| --- | --- | --- |
| `partyshare-x.y.z-linux-x64.AppImage` | Linux x64 | Built and verified end to end |
| `partyshare-x.y.z-windows-x64.msi` | Windows x64 | Built, installed and run |
| `partyshare-x.y.z-windows-x64.zip` | Windows x64 | Built, installed and run |
| `partyshare-x.y.z-macos-arm64.dmg` | macOS ARM64 | Built by hand, installed and run. Ships a client that **cannot make a call** |
| `partyshare-x.y.z-macos-x64.dmg` | macOS x64 | Written, never run |
| `SHA256SUMS` | — | Generated over whatever exists |

The `publish` job runs with `always()` and requires that either the Windows or the
macOS job passed. A failing macOS job cannot hold back a Windows artifact that is
already built: partial is a worse release than complete, and a much better one
than none.

The Linux job is not among them, and is off entirely behind the `RELEASE_LINUX`
repository variable: `scripts/build_webrtc.sh` cannot get past the depot_tools
bootstrap on a runner, and without libwebrtc there is no AppImage. So an automatic
release today carries the Windows and macOS artifacts and no Linux one, which is
worth knowing before the first tag nobody asked for arrives.

## Linux

The only one this project verifies end to end. `scripts/appimage.sh` does the
work and can be run locally:

```sh
scripts/appimage.sh                 # configure, build, package
scripts/appimage.sh --skip-build    # only repackage what is already built
```

Three things the script handles that are not obvious:

- **The media layer is mandatory.** The first AppImage built came up without it:
  it opened the window, showed the login screen, and made no calls at all. That is
  worse than no artifact, because it looks like the product. A missing libwebrtc
  is an error now rather than a warning, and the CI smoke test rejects any
  artifact whose log says "no media layer".
- **The tool's `strip` is too old.** `linuxdeploy` carries its own binutils and
  rejects the `.relr.dyn` section a current linker emits, fatally and with no way
  to ignore it. Packaging happens in two passes because of that, with the system
  `strip` in between.
- **glibc cannot be bundled, and that decides where the artifact runs.** Qt and
  the C++ runtime go inside; glibc does not. The AppImage runs on any distribution
  whose glibc is at least as new as the one on the machine that built it, and on
  none older.

  Not theoretical: one built on this development machine (Arch, glibc 2.44) **does
  not start on a clean Ubuntu 24.04** — it asks for `GLIBC_2.43` and `GLIBC_2.44`,
  and 24.04 has 2.39. The script prints the floor of the file it just produced,
  because it is an invisible property until somebody cannot open the program. An
  AppImage built locally is a development artifact; the distributable comes out of
  CI, which builds on the oldest runner available.

### What the AppImage takes from the system

What stays out is deliberate: bundling fontconfig makes the program stop finding
the system fonts, and bundling the graphics stack makes it stop finding the
machine's driver. The exact set, verified with `ldd` over the extracted tree in a
clean container:

```text
libX11  libX11-xcb  libxcb  libICE  libSM
libEGL  libGLX  libOpenGL  libdrm  libgbm
libfontconfig  libfreetype  libharfbuzz
```

Every Linux desktop already has all thirteen. The workflow's clean machine test
installs exactly that list and nothing else, which is what keeps it honest: a new
dependency creeping in without being listed here fails there.

## Windows

An MSI built by CPack's WiX generator, with a start menu shortcut, a desktop
shortcut, its own icon and an entry in Add/Remove Programs. An MSI is what a
machine already knows how to install: `msiexec /i partyshare-x.y.z-windows-x64.msi /qn`
for a fleet, a double click for a person. The ZIP stays alongside it for people
who prefer the files without an installer touching their machine.

`CPACK_WIX_UPGRADE_GUID` in `cmake/Packaging.cmake` is what makes the next version
replace this one instead of installing beside it, so **it must never change**.

Both carry the client and the Qt runtime and nothing else: the job configures with
`-DDV_BUILD_SERVER=OFF`, because the server is a Linux daemon and nobody installs
one from a desktop MSI.

**The Windows client carries the media layer, and did not until v0.1.4.**
`DV_BUILD_CLIENT_MEDIA` defaults to OFF and the job did not pass it, so every MSI
up to v0.1.3 installed a client that connected, joined a room, and answered every
attempt to speak with "This build was compiled without audio and video". It is the
same failure the first AppImage had, found the same way. A step after the build
scans `partyshare.exe` for the sentence only the stub carries and fails the job
when it is there, because a flag that goes missing again would otherwise build,
package, install, sign and publish without a word.

Two things learned by installing it, both in entry 17 of
[chapter 15](15-postmortems.md): `msiexec` needs an elevated shell, and Smart App
Control blocks a new unsigned binary once while Microsoft's app intelligence
service makes up its mind about a file it has never seen.

## macOS

The `.dmg` has the bundle, the shortcut to `/Applications`, a volume icon and the
example configuration. It lacks a background image and icon positioning, which is
appearance rather than function.

**It still ships a client that cannot make a call.** A libwebrtc tree exists for
macOS now, produced by `scripts/build_webrtc.sh` on an Apple Silicon machine and
validated by linking the client against it. What is missing is publishing that
tree as a release asset and recording its URL and checksum in
`cmake/Findlibwebrtc.cmake`, the way `_dv_url_windows_x64_md` already is. Until
then the macOS job must not pass `-DDV_BUILD_CLIENT_MEDIA=ON`: the only tree it
could fetch is the prebuilt one, whose `std::__Cr` symbols cannot link against a
client built with Apple's own libc++.

## Signing and notarization

Conditional on the secrets existing, and not assumed. A fork or a first tag
produce unsigned artifacts rather than a broken pipeline: an unsigned build is a
build the operating system complains about, and that is a better failure than no
build at all.

| Secret | For |
| --- | --- |
| `WINDOWS_CERTIFICATE`, `WINDOWS_CERTIFICATE_PASSWORD` | Code signing certificate, a `.pfx` in base64 |
| `MACOS_CERTIFICATE`, `MACOS_CERTIFICATE_PASSWORD` | Developer ID Application certificate, a `.p12` in base64 |
| `MACOS_SIGNING_IDENTITY` | Identity name, as `security find-identity` shows it |
| `MACOS_NOTARY_APPLE_ID`, `MACOS_NOTARY_PASSWORD`, `MACOS_NOTARY_TEAM_ID` | Apple ID, an app-specific password, and the ten character Team ID |

**None of them is configured today.**

Notarization is not signing: Apple wants the bundle signed with a Developer ID,
then uploaded, then stapled, and a bundle that skips any of the three is one
Gatekeeper refuses to open on a machine that did not build it.

On Windows, two properties hold whatever certificate is bought: Smart App Control
accepts only certificates from a CA in the Microsoft Trusted Root Program, so a
self-signed one is no better than none, and its check does not read ECC at all, so
the certificate has to be RSA. The signing step covers every unsigned `.exe` and
`.dll` in the tree, not just the executable — Smart App Control checks every
binary as it is loaded.

## What is not done yet

- The macOS libwebrtc tree published as a release asset, which is what would make
  the `.dmg` a working product.
- A `.dmg` with a background image and icon positioning.
- Signing and notarization, which depend on certificates nobody has bought.
- macOS x64: never built, never run.
