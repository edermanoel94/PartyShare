# CPackOptions.cmake
#
# Read by CPack once per generator, with CPACK_GENERATOR set to the one being
# run. That is the only place a variable can differ between the two generators
# Windows builds from a single configure, and CPACK_INCLUDE_TOPLEVEL_DIRECTORY
# has to, because the two want opposite answers.
#
# An archive wants it on: unpacking should produce one directory named after the
# file, not bin/ and share/ loose in whatever directory the person happened to
# be in. An installer wants it off, because the installer already puts what it
# carries inside CPACK_PACKAGE_INSTALL_DIRECTORY, and a top level directory on
# top of that is a second level nobody asked for.
#
# Every installer generator already defaults this to off for exactly that
# reason, with SetOptionIfNotSet - which means setting it unconditionally in
# Packaging.cmake did not add to the default, it replaced it. The first MSI this
# repository ever built installed to
#
#   C:\Program Files\PartyShare\partyshare-0.1.0-windows-x64\bin\partyshare.exe
#
# and, because the directory CPack packaged was then the parent of the staging
# tree, it also carried CPack's own WiX intermediates - files.wxs,
# directories.wxs, cpack_variables.wxi and the rest - into Program Files.

if(CPACK_GENERATOR STREQUAL "WIX")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
