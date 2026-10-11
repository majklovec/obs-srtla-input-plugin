# NSIS installer target for Windows
#
# `cmake --build build_x64 --config Release --target installer` stages the
# plugin like an OBS Studio directory (obs-plugins\64bit + data\obs-plugins)
# and packages it with installer/obs-srtla-source.nsi into
# release/<name>-<version>-windows-x64-installer.exe.
include_guard(GLOBAL)

find_program(MAKENSIS_EXECUTABLE
  NAMES makensis
  HINTS "$ENV{ProgramFiles\(x86\)}/NSIS" "$ENV{ProgramFiles}/NSIS" "$ENV{ProgramW6432}/NSIS"
        "$ENV{LOCALAPPDATA}/Programs/NSIS" "$ENV{ChocolateyInstall}/bin" "C:/Program Files (x86)/NSIS"
  DOC "NSIS compiler (makensis.exe) used by the installer target")

if(NOT MAKENSIS_EXECUTABLE)
  message(STATUS "makensis not found; the 'installer' target is unavailable. "
                 "Install NSIS 3 (https://nsis.sourceforge.io) or `winget install NSIS.NSIS`.")
  return()
endif()

set(PLUGIN_INSTALLER_STAGING "${CMAKE_BINARY_DIR}/installer-staging")
set(PLUGIN_INSTALLER_FILE
    "${CMAKE_SOURCE_DIR}/release/${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION}-windows-x64-installer.exe")
file(TO_NATIVE_PATH "${PLUGIN_INSTALLER_STAGING}" _staging_native)
file(TO_NATIVE_PATH "${PLUGIN_INSTALLER_FILE}" _installer_native)

string(JSON _display_name GET "${buildspec}" displayName)

add_custom_target(installer
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PLUGIN_INSTALLER_STAGING}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config $<CONFIG>
          --prefix "${PLUGIN_INSTALLER_STAGING}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/release"
  COMMAND "${MAKENSIS_EXECUTABLE}" /V2 /WX
          "/DPLUGIN_NAME=${CMAKE_PROJECT_NAME}"
          "/DPLUGIN_DISPLAY_NAME=${_display_name}"
          "/DPLUGIN_VERSION=${CMAKE_PROJECT_VERSION}"
          "/DPLUGIN_AUTHOR=${PLUGIN_AUTHOR}"
          "/DPLUGIN_WEBSITE=${PLUGIN_WEBSITE}"
          "/DSTAGING_DIR=${_staging_native}"
          "/DOUTPUT_FILE=${_installer_native}"
          "${CMAKE_SOURCE_DIR}/installer/${CMAKE_PROJECT_NAME}.nsi"
  DEPENDS ${CMAKE_PROJECT_NAME}
  COMMENT "Building NSIS installer ${PLUGIN_INSTALLER_FILE}"
  VERBATIM
  USES_TERMINAL)
