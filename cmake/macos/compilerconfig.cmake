# OBS plugin template macOS compiler configuration
include_guard(GLOBAL)

if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
  message(FATAL_ERROR "Building OBS plugins on macOS requires the Xcode generator.")
endif()

include(compiler_common)

add_compile_options(-fopenmp-simd)
add_compile_definitions(
  $<$<CONFIG:DEBUG>:DEBUG>
  $<$<CONFIG:DEBUG>:_DEBUG>
  SIMDE_ENABLE_OPENMP
)
