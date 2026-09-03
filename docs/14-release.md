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

## How anybody finds out there is a new version

Everything above ends with a tag and four files on a page. Nothing in it reaches
the machine of somebody who installed 0.1.30 in March, and that is the failure
this section is about: three people in one room running three versions, which is
invisible from inside, because the protocol has no version negotiation and what
an old client does with a message it does not know is nothing at all.

So the client asks. Five seconds after the window opens, and every six hours
after that, it makes one HTTPS request:

```text
GET https://api.github.com/repos/edermanoel94/PartyShare/releases/latest
```

It reads `tag_name` out of the answer, drops the `v`, and compares the three
numbers against its own `DV_VERSION`. Those are the same three numbers by
construction — `tag.yml` writes the version into `CMakeLists.txt` and then names
the tag after it — so this is a comparison between a released version and a
running one, not between two conventions that could drift.

If the published one is **strictly** greater, the version in the status bar
becomes a sentence:

```text
PartyShare 0.1.41 · 0.1.42 available
```

The second half is a link to the release page. That is the entire feature.
**Nothing is downloaded, nothing is executed, and nothing is written to disk.**
A program that replaces its own signed binary is a different thing to build and
a different thing to trust, and this one does not.

What it deliberately does not do, and why:

- **It never interrupts.** No dialog, no toast, nothing to dismiss. The news can
  arrive in the middle of a call, and a call is not the moment to hand somebody
  a modal window about housekeeping.
- **It says nothing to a build that is ahead of the newest release** — every
  build from `master` after a tag, and every build from a pull request. The
  alternative, treating "different" as "out of date", offers an older version to
  exactly the people working on the newer one.
- **It fails silently — to the person, not to the log.** No route to the
  internet, a proxy that refuses, a Qt with no TLS backend, GitHub answering 403
  because the address ran out of anonymous requests: each of those is a check
  that did not happen, and none of them is worth interrupting anybody over. The
  first outcome of each run is written at `info` whatever it was, so
  `Update check: 0.1.41 is the newest release, and this is 0.1.41` in the log
  answers "is this even switched on". The ones after it are at `debug`, which
  the preprocessor removes from every build that is not `Debug` — which is why
  the first one cannot be.
- **It refuses a link that is not `https` on `github.com`.** The URL arrives
  inside a JSON document from the network and ends as an argument to the
  platform's "open this", so it is checked where it becomes a link; anything
  else falls back to the releases page.

**Check GitHub for new versions**, in the Connection group of the Settings
dialog, turns it off, and then no request is made at all. It writes
`[ui] check_for_updates` into this user's `config.ini`, which is also editable
by hand and is what an administrator sets machine-wide. Off is the right setting
on a LAN with no route out, where every check is a timeout, and on a network
whose administrator decides what talks to the outside.

The comparison lives in `shared/include/dv/core/version.hpp` and is tested in
`tests/unit/test_version.cpp`; the request lives in
`client/src/ui/update_checker.cpp`. The repository name is written out in both
that file and `tag.yml` — a fork that publishes its own releases changes those
two lines.

## What the tag produces

**Four files and a checksum list.** That is the whole answer today, and it is
worth stating before the table, because two of the six jobs this repository
carries do not run:

```text
partyshare-x.y.z-macos-arm64.dmg
partyshare-x.y.z-windows-x64.msi
partyshare-x.y.z-windows-x64.zip
partyshare-server-x.y.z-linux-x64.tar.gz
SHA256SUMS
```

The first three were verified against every release from v0.1.31 to v0.1.34,
each of which carried exactly those and nothing else. The server tarball arrived
later, and the first tag to carry it is the one that proves the job.

| Job | Runs on a tag? | Produces | State |
| --- | --- | --- | --- |
| Linux x64 server | **yes** | `partyshare-server-…-linux-x64.tar.gz` | Built with MongoDB on Ubuntu 22.04, smoke tested; what `scripts/install_server.sh` downloads |
| Windows x64 installer | **yes** | `.msi` and `.zip` | Built, installed and run |
| macOS arm64 | **yes** | `.dmg` | Built and installed. Ships a client that **cannot make a call** |
| Linux x64 AppImage | **no** — skipped | nothing | Off behind `RELEASE_LINUX`, below |
| macOS x64 | **no** — the matrix entry is commented out | nothing | Off, with a known cause, below |
| Publish | yes | `SHA256SUMS` over whatever exists | — |

The `publish` job runs with `always()` and requires that either the Windows or the
macOS job passed. A failing macOS job cannot hold back a Windows artifact that is
already built: partial is a worse release than complete, and a much better one
than none.

### Why the Linux job does not run

`if: vars.RELEASE_LINUX == 'true'`, and that repository variable is unset, so the
job is **skipped on every tag**. It is a variable rather than `if: false` because
actionlint rejects a constant condition, and rightly — a switch that can only be
flipped by editing the workflow is not a switch. Setting it to `true` under
Settings → Secrets and variables → Actions → Variables turns it back on.

The reason it is off: `scripts/build_webrtc.sh` cannot get past the depot_tools
CIPD bootstrap on a runner — curl gets a 403 one second in, before a byte of
WebRTC is fetched — and without libwebrtc there is no AppImage. The job is kept
rather than deleted because nothing *in it* is known to be wrong.

**A skipped job is green.** The release run reports success with the Linux job
skipped, so nothing in the workflow's own output says an artifact is missing.
The way to know is to look at what the release actually carries, which is the
list at the top of this section.

### Why the macOS x64 job does not run

The matrix entry is commented out in `release.yml`, and the reason is specific
rather than "nobody got to it": on the Intel runner Homebrew lives in
`/usr/local`, which is on the default include path, so its OpenSSL headers reach
the compiler ahead of the ones vcpkg passes with `-isystem`, and
`-Wold-style-cast -Werror` kills the build inside `safestack.h`. On Apple Silicon
Homebrew is in `/opt/homebrew`, off that path, which is the whole reason arm64
builds and x64 does not.

## Linux

Everything below describes a path that **is not currently exercised by any tag**,
for the reason in the section above. It is kept because the script works when it
is run by hand, and because turning `RELEASE_LINUX` back on is one variable rather
than a rewrite.

`scripts/appimage.sh` does the work and can be run locally:

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

Every Linux desktop already has all thirteen. The clean machine test installs
exactly that list and nothing else inside an `ubuntu:24.04` container, which is
what would keep the list honest: a new dependency creeping in without being listed
here fails there.

**Would**, not does. That test lives inside the `linux` job of `release.yml`, so
it is skipped along with everything else in that job, and it has not run on any
tag. The list above is therefore as good as the last time somebody ran it by
hand, and not a claim the pipeline is currently checking.

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

**The macOS client carries the media layer, and did not until v0.1.45.**
Every `.dmg` up to v0.1.44 installed a client that connected, joined a room and
answered every attempt to speak or share a screen with "This build has no media
layer", the same failure the Windows job had until v0.1.4 and the first AppImage
had before that.
It could not be fixed by passing the flag alone: the only tree the job could
fetch was the prebuilt one, whose `std::__Cr` symbols cannot link against a
client built with Apple's own libc++.
What unblocked it was publishing the source build under the
`webrtc-m152.7977.0.0-macos-arm64` tag and recording its URL and checksum in
`cmake/Findlibwebrtc.cmake` as `_dv_url_macos_arm64_src`, the way
`_dv_url_windows_x64_md` already was.
A step after the build scans the bundle's executable for the sentence only the
stub carries, because a flag that goes missing again would otherwise build,
bundle, sign, notarise and publish without a word.

**The hardened runtime is a second gate in front of the microphone.**
`codesign --options runtime` refuses the input device to a bundle carrying no
`com.apple.security.device.audio-input`, whatever `NSMicrophoneUsageDescription`
says, and the room then runs on silence with nothing in any log to explain it.
The signing step passes `assets/partyshare.entitlements` for that one key.
The screen needs neither an entitlement nor a plist key: macOS asks for it once,
on its own, and remembers the answer in System Settings.

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

Ordered by what it costs the person downloading a release.

- **No Linux client artifact.** Blocked on the depot_tools CIPD bootstrap getting
  a 403 on a runner. Until that is solved, a Linux user has to build the client
  from source, and [INSTALL.md](../INSTALL.md) is the path. The server is not
  affected: it needs no libwebrtc, and its tarball is built and published on
  every tag by the `linux-server` job.
- **The macOS `.dmg` ships a client that cannot make a call.** Blocked on
  publishing the macOS libwebrtc tree as a release asset and recording its URL and
  checksum in `cmake/Findlibwebrtc.cmake`, the way `_dv_url_windows_x64_md`
  already is.
- **No macOS x64 build.** Blocked on the Homebrew include-path collision
  described above — a real diagnosis, not an untried job.
- Signing and notarization, which depend on certificates nobody has bought.
- A `.dmg` with a background image and icon positioning. Appearance, not function.

So one of the three platforms ships something that works, and it is Windows. That
is worth saying plainly here, because the release run is green either way.
