# Imported targets for the FFmpeg and libsrt libraries shipped in obs-deps.
#
# OBS Studio for Windows installs these DLLs in bin/64bit and its FFmpeg is
# already linked against the same srt.dll, so the plugin uses them instead of
# building its own copies. Each DLL has its own symbol namespace, so no
# interposition issue exists like on Linux.
include_guard(GLOBAL)

find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h REQUIRED)

set(PLUGIN_FFMPEG_TARGETS "")
foreach(component IN ITEMS avcodec avformat avutil swresample swscale)
  find_library(FFMPEG_${component}_LIBRARY NAMES ${component} REQUIRED)
  set(_target FFmpeg::${component})
  if(NOT TARGET ${_target})
    add_library(${_target} UNKNOWN IMPORTED)
    set_target_properties(${_target} PROPERTIES
                          IMPORTED_LOCATION "${FFMPEG_${component}_LIBRARY}"
                          INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
  endif()
  list(APPEND PLUGIN_FFMPEG_TARGETS ${_target})
endforeach()

file(READ "${FFMPEG_INCLUDE_DIR}/libavformat/version_major.h" _avformat_version_header)
string(REGEX MATCH "LIBAVFORMAT_VERSION_MAJOR[ \t]+([0-9]+)" _match "${_avformat_version_header}")
if(CMAKE_MATCH_1 LESS 59)
  message(FATAL_ERROR "libavformat >= 59 is required, found major version ${CMAKE_MATCH_1}")
endif()

find_path(LIBSRT_INCLUDE_DIR NAMES srt/srt.h REQUIRED)
find_library(LIBSRT_LIBRARY NAMES srt REQUIRED)
if(NOT TARGET Libsrt::Libsrt)
  add_library(Libsrt::Libsrt UNKNOWN IMPORTED)
  set_target_properties(Libsrt::Libsrt PROPERTIES
                        IMPORTED_LOCATION "${LIBSRT_LIBRARY}"
                        INTERFACE_INCLUDE_DIRECTORIES "${LIBSRT_INCLUDE_DIR}")
endif()

# obs-deps ships the third-party license texts next to the libraries.
cmake_path(GET LIBSRT_LIBRARY PARENT_PATH _libsrt_lib_dir)
cmake_path(GET _libsrt_lib_dir PARENT_PATH _obs_deps_root)
set(LIBSRT_LICENSE_FILE "${_obs_deps_root}/licenses/srt/LICENSE")
