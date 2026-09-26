# OBS plugin template common helper functions
include_guard(GLOBAL)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in")
  configure_file(src/plugin-support.c.in plugin-support.c @ONLY)
  add_library(plugin-support STATIC)
  target_sources(plugin-support PRIVATE plugin-support.c PUBLIC src/plugin-support.h)
  target_include_directories(plugin-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
    set_property(TARGET plugin-support PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()
endif()
