# Packaging.cmake
#
# The half of the M9 that is the same everywhere: what goes in an artefact, what
# it is called, and who it says it is from. What differs per platform is how the
# tree is wrapped, and that lives in scripts/package_*.sh and in the release
# workflow, not here.
#
# CPack produces the plain archives on every platform. The Windows installer,
# the AppImage and the .dmg are built by the platform tooling in scripts/,
# because none of the CPack generators for them can do what the platform's own
# tool does: bundle the Qt runtime.

# The name a person reads to know whether the file they downloaded is the one
# for their machine. CMAKE_SYSTEM_PROCESSOR spells the same architecture three
# ways depending on the platform, so it is normalised here rather than in each
# place that builds a file name.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(dv_arch "arm64")
else()
  set(dv_arch "x64")
endif()

if(WIN32)
  set(DV_PACKAGE_PLATFORM "windows-${dv_arch}")
elseif(APPLE)
  set(DV_PACKAGE_PLATFORM "macos-${dv_arch}")
else()
  set(DV_PACKAGE_PLATFORM "linux-${dv_arch}")
endif()
set(DV_PACKAGE_PLATFORM "${DV_PACKAGE_PLATFORM}" CACHE INTERNAL
  "Platform tag used in artefact file names")

set(CPACK_PACKAGE_NAME "PartyShare")
set(CPACK_PACKAGE_VENDOR "PartyShare")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PartyShare")
set(CPACK_PACKAGE_FILE_NAME "partyshare-${PROJECT_VERSION}-${DV_PACKAGE_PLATFORM}")

# A tarball that unpacks into a directory named after itself, rather than
# spraying bin/ and share/ into whatever directory it was unpacked in.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)

if(WIN32)
  # NSIS and not ZIP: task 1 asks for an installer, and an archive is not one.
  # What separates them is what a person gets afterwards - a start menu entry, a
  # shortcut, and a way to remove the thing - and none of that comes from
  # unpacking a folder somewhere. ZIP stays alongside for anyone who wants the
  # files without an installer touching their machine.
  set(CPACK_GENERATOR "NSIS;ZIP")

  set(CPACK_NSIS_PACKAGE_NAME "PartyShare")
  set(CPACK_NSIS_DISPLAY_NAME "PartyShare")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\partyshare.exe")
  set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/assets/partyshare.ico")
  set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/assets/partyshare.ico")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  # Upgrading in place rather than stacking a second copy next to the first,
  # which is what happens by default and is how a machine ends up running an
  # old version from a directory nobody remembers creating.
  set(CPACK_NSIS_UNINSTALL_NAME "Uninstall PartyShare")
  set(CPACK_NSIS_MODIFY_PATH OFF)

  # The shortcut is the client. The server is installed and deliberately not
  # given one: nobody double clicks a signaling server.
  set(CPACK_PACKAGE_EXECUTABLES "partyshare;PartyShare")
  set(CPACK_CREATE_DESKTOP_LINKS "partyshare")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
