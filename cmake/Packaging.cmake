# Packaging.cmake
#
# The half of the M9 that is the same everywhere: what goes in an artefact, what
# it is called, and who it says it is from. What differs per platform is how the
# tree is wrapped, and that lives in scripts/package_*.sh and in the release
# workflow, not here.
#
# CPack produces the plain archives on every platform, and on Windows the MSI as
# well. The AppImage and the .dmg are built by the platform tooling in scripts/,
# because none of the CPack generators for them can do what the platform's own
# tool does: bundle the Qt runtime. Windows is the exception only because
# windeployqt runs before CPack and the result is fed in as the install tree.

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
#
# Right for an archive and wrong for an installer, which already installs into
# CPACK_PACKAGE_INSTALL_DIRECTORY. Windows builds both from this one configure,
# so the answer has to be per generator, and CPackOptions.cmake is the only file
# CPack reads late enough to know which generator is running.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackOptions.cmake")

# What the installer says about the project, from the project rather than from
# CMake. Left unset, both of these point at templates inside the CMake
# installation, and the licence page of the MSI read "This is an installer
# created using CPack. No license provided." - a sentence about the tool that
# built the file, shown to the person who downloaded it.
#
# The licence goes through a copy with a .txt extension: the WiX generator
# converts .txt to the RTF its dialog needs and refuses any other extension,
# and the file at the root is called LICENSE so that GitHub finds it.
set(dv_packaging_dir "${CMAKE_BINARY_DIR}/packaging")
configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${dv_packaging_dir}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${dv_packaging_dir}/LICENSE.txt")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")

if(WIN32)
  # WiX and not NSIS: task 1 asks for an installer, and an MSI is the installer
  # Windows itself understands. A person gets it in Add/Remove Programs, a fleet
  # gets `msiexec /i partyshare.msi /qn`, and neither needs anyone to describe
  # what the file does first. ZIP stays alongside for anyone who wants the files
  # without an installer touching their machine.
  set(CPACK_GENERATOR "WIX;ZIP")

  # The identity of this product across every version it will ever have, which
  # is what Windows matches an upgrade against. It is written down rather than
  # generated because CPack invents a new one on each configure when it is
  # absent, and a new GUID means the next MSI installs beside this one instead
  # of replacing it. Never change it.
  set(CPACK_WIX_UPGRADE_GUID "9316F1D1-ED63-4C61-9492-4DF24C637A3F")

  set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/assets/partyshare.ico")
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "PartyShare")

  # What Add/Remove Programs shows next to the entry. Left out, the fields are
  # blank, which is how an installed program ends up looking like something that
  # arrived without being asked for.
  set(CPACK_WIX_PROPERTY_ARPCOMMENTS "${PROJECT_DESCRIPTION}")
  set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/edermanoel94/PartyShare")

  # The shortcut is the client, which on Windows is the whole artefact: the
  # release job builds with -DDV_BUILD_SERVER=OFF, because nobody double clicks
  # a signaling server and nobody installs one from a desktop MSI.
  set(CPACK_PACKAGE_EXECUTABLES "partyshare;PartyShare")
  set(CPACK_CREATE_DESKTOP_LINKS "partyshare")

  # The pages of the installer. WiX's own sequence goes Welcome, licence,
  # folder, ready; this one has a "What's new" page between Welcome and the
  # licence, showing CHANGELOG.md, so that somebody running an upgrade sees
  # what it brings before agreeing to anything. The page lives in
  # cmake/wix/PartyShareUI.wxs, and the text it shows is CHANGELOG.md turned
  # into RTF here at configure time - the only format the control reads. The
  # define is how the .wxs learns where the RTF landed.
  include("${CMAKE_CURRENT_LIST_DIR}/ChangelogRtf.cmake")
  dv_changelog_to_rtf("${CMAKE_SOURCE_DIR}/CHANGELOG.md" "${dv_packaging_dir}/whatsnew.rtf")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/CHANGELOG.md")
  set(CPACK_WIX_UI_REF "WixUI_PartyShare")
  set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_LIST_DIR}/wix/PartyShareUI.wxs")
  set(CPACK_WIX_CANDLE_EXTRA_FLAGS "-dDvWhatsNewRtf=${dv_packaging_dir}/whatsnew.rtf")

  # Beside bin/, so that what the installer showed is also what it left on the
  # disk. The .txt copy rather than LICENSE, because on Windows a file with no
  # extension opens nowhere.
  install(FILES "${dv_packaging_dir}/LICENSE.txt" "${CMAKE_SOURCE_DIR}/CHANGELOG.md"
    DESTINATION .
  )
else()
  set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
