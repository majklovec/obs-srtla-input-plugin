# OBS plugin template compiler cache support
include_guard(GLOBAL)

option(ENABLE_CCACHE "Enable compiler caching with ccache" OFF)
mark_as_advanced(ENABLE_CCACHE)

if(ENABLE_CCACHE)
  find_program(CCACHE_PROGRAM ccache REQUIRED)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
