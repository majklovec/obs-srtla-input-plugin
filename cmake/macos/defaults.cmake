# OBS plugin template macOS defaults
include_guard(GLOBAL)

include(GNUInstallDirs)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
set(CMAKE_MACOSX_RPATH TRUE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
      CACHE PATH "Default plugin installation directory" FORCE)
endif()
