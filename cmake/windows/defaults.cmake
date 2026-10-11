# OBS plugin template Windows defaults
include_guard(GLOBAL)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

include(buildspec)

# The install tree mirrors an OBS Studio installation directory
# (obs-plugins/64bit + data/obs-plugins/<plugin>); the NSIS installer packages
# it and `cmake --install` can also copy it straight into an OBS directory.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/release/${CMAKE_PROJECT_NAME}" CACHE PATH
      "Staging directory laid out like an OBS Studio installation" FORCE)
endif()
