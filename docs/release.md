# Release

How a version ships, what it produces, and what is not verified yet.

There is one rule: no artifact is ever built on a development machine.
A binary published from a laptop is a binary whose contents nobody can reproduce, and the first time that matters is when someone reports a crash in a build that no longer exists anywhere.

## Cutting a release

```sh
# 1. The version lives in one place only.
$EDITOR CMakeLists.txt        # project(partyshare VERSION x.y.z)
$EDITOR vcpkg.json            # the same number

# 2. Commit, tag, push.
git commit -am "Version x.y.z"
git tag vx.y.z
git push origin master vx.y.z
```

The tag triggers `.github/workflows/release.yml`.
Nothing beyond the tag is needed: the workflow builds, tests what can be tested, and publishes.

To exercise the pipeline without cutting a release, use `workflow_dispatch` from the GitHub interface.
It builds everything and publishes nothing, because a release without a tag has no version to be.

## What the tag produces

| Artifact | Platform | State |
| --- | --- | --- |
| `partyshare-x.y.z-linux-x64.AppImage` | Linux x64 | Built and verified |
| `partyshare-x.y.z-windows-x64.exe` | Windows x64 | Written, never run |
| `partyshare-x.y.z-windows-x64.zip` | Windows x64 | Written, never run |
| `partyshare-x.y.z-macos-arm64.dmg` | macOS ARM64 | Written, never run |
| `partyshare-x.y.z-macos-x64.dmg` | macOS x64 | Written, never run |
| `SHA256SUMS` | - | Generated over whatever exists |

The `publish` job runs with `always()` and requires only that the Linux job passed.
A failing macOS job cannot hold back a Linux artifact that is already built and tested: partial is a worse release than complete, and a much better one than none.

## Linux

The only one this machine can verify, and the only one that is verified.

`scripts/appimage.sh` does the work, and it can be run locally:

```sh
scripts/appimage.sh                 # configure, build, package
scripts/appimage.sh --skip-build    # only repackage what is already built
```

Three things the script handles that are not obvious:

- **The media layer is mandatory.**
  The first AppImage built came up without it: it opened the window, showed the login screen, and made no calls at all.
  That is worse than no artifact, because it looks like the product.
  A missing libwebrtc is now an error rather than a warning, and the CI smoke test rejects any artifact whose log says "no media layer".
- **The tool's `strip` is too old.**
  `linuxdeploy` carries its own binutils, and it rejects the `.relr.dyn` section a current linker emits, fatally and without a way to ignore it.
  Packaging happens in two passes because of that, with the system `strip` in between.
- **glibc cannot be bundled, and that decides where the artifact runs.**
  Qt and the C++ runtime go inside; glibc does not.
  The AppImage runs on any distribution whose glibc is at least as new as the one on the machine that built it, and on none older.

  This is not theoretical. An AppImage built on this development machine, which runs Arch with glibc 2.44, **does not start on a clean Ubuntu 24.04**: it asks for `GLIBC_2.43` and `GLIBC_2.44`, and 24.04 has 2.39.
  The script prints the floor of the file it just produced for that reason, because it is an invisible property until someone cannot open the program.

  An AppImage built locally is a development artifact, not a distribution one. The distributable comes out of CI, which builds on the oldest runner available.

### What the AppImage takes from the system

Not everything is bundled, and what stays out is deliberate: bundling fontconfig makes the program stop finding the system fonts, and bundling the graphics stack makes it stop finding the machine's driver.

The exact set, verified with `ldd` over the extracted tree in a clean container:

```text
libX11  libX11-xcb  libxcb  libICE  libSM
libEGL  libGLX  libOpenGL  libdrm  libgbm
libfontconfig  libfreetype  libharfbuzz
```

Every Linux desktop already has all thirteen.
The workflow's clean machine test installs exactly that list and nothing else, which is what keeps it honest: a new dependency that creeps in without being listed here fails there.

## Windows and macOS

Written from the documentation of each platform's tooling, and **never run**.
This repository is developed on Linux, and neither job has ever produced a file that anyone installed.

Treat the first run as the thing that will discover what is wrong with them, not as a regression.
What they intend to produce:

- The Windows installer is NSIS, with a start menu shortcut, its own icon, and uninstallation. The ZIP stays alongside it for people who prefer the files without an installer touching their machine.
- The `.dmg` has the bundle, the shortcut to `/Applications`, and a volume icon. It lacks a background image and icon positioning in the window, which is appearance rather than function.
- Neither has been opened on a clean machine, which is the milestone's acceptance criterion.

## Signing and notarization

Conditional on the secrets existing, and not assumed.
A fork or a first tag produce unsigned artifacts rather than a broken pipeline: an unsigned build is a build the operating system complains about, and that is a better failure than no build at all.

| Secret | What it is for |
| --- | --- |
| `WINDOWS_CERTIFICATE` | Code signing certificate, a `.pfx` in base64 |
| `WINDOWS_CERTIFICATE_PASSWORD` | Password for the `.pfx` |
| `MACOS_CERTIFICATE` | Developer ID Application certificate, a `.p12` in base64 |
| `MACOS_CERTIFICATE_PASSWORD` | Password for the `.p12` |
| `MACOS_SIGNING_IDENTITY` | Identity name, as it appears in `security find-identity` |
| `MACOS_NOTARY_APPLE_ID` | Apple ID of the developer account |
| `MACOS_NOTARY_PASSWORD` | App specific password, not the account password |
| `MACOS_NOTARY_TEAM_ID` | Ten character Team ID |

None of them is configured today.
Notarization is not signing: Apple wants the bundle signed with a Developer ID, then uploaded, then stapled, and a bundle that skips any of the three is one Gatekeeper refuses to open on a machine that did not build it.

## What is not done yet

Of the six M9 tasks, two are closed: the AppImage and publishing by tag.

- Running the Windows and macOS jobs for the first time, which is what will say what is wrong with them.
- A `.dmg` with a background image and icon positioning.
- Signing and notarization, which depend on certificates nobody has bought.
- Installing and running the Windows and macOS artifacts on a clean machine of the respective platform.

Linux is the only one that goes through the whole path, and the workflow's clean machine test is what holds that claim up: the job builds on Ubuntu 22.04 and starts the result inside an Ubuntu 24.04 container with only the thirteen system libraries listed above.
