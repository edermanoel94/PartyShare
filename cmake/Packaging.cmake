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
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
