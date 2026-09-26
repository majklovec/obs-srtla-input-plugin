# OBS plugin template build dependencies module (Windows/macOS)
#
# Downloads the pre-built obs-deps and the obs-studio sources listed in
# buildspec.json into .deps/, then builds and installs libobs (and
# obs-frontend-api) there so find_package(libobs) works.
include_guard(GLOBAL)

# _check_deps_version: Checks for obs-deps VERSION file in prefix paths
function(_check_deps_version version)
  set(found FALSE)

  foreach(path IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${path}/share/obs-deps/VERSION")
      if(dependency STREQUAL qt6 AND NOT EXISTS "${path}/lib/cmake/Qt6/Qt6Config.cmake")
        set(found FALSE)
        continue()
      endif()

      file(READ "${path}/share/obs-deps/VERSION" _check_version)
      string(REPLACE "\n" "" _check_version "${_check_version}")
      string(REPLACE "-" "." _check_version "${_check_version}")
      string(REPLACE "-" "." version "${version}")

      if(_check_version VERSION_EQUAL version)
        set(found TRUE)
        break()
      elseif(_check_version VERSION_LESS version)
        message(AUTHOR_WARNING "Older ${label} version detected in ${path}: \n"
                               "Found ${_check_version}, require ${version}")
        list(REMOVE_ITEM CMAKE_PREFIX_PATH "${path}")
        list(APPEND CMAKE_PREFIX_PATH "${path}")
        set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH})
        continue()
      else()
        message(AUTHOR_WARNING "Newer ${label} version detected in ${path}: \n"
                               "Found ${_check_version}, require ${version}")
        set(found TRUE)
        break()
      endif()
    endif()
  endforeach()

  return(PROPAGATE found CMAKE_PREFIX_PATH)
endfunction()

# _setup_obs_studio: Configure the obs-studio sources, then build and install
# libobs and obs-frontend-api (Development component) into .deps/.
function(_setup_obs_studio)
  if(NOT libobs_DIR)
    set(_is_fresh --fresh)
  endif()

  set(_obs_source "${dependencies_dir}/${_obs_destination}")
  set(_obs_build "${_obs_source}/build_${arch}")

  if(OS_WINDOWS)
    set(_cmake_generator_args -G "${CMAKE_GENERATOR}" -A
                              "${arch},version=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    set(_cmake_extra -DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION} -DENABLE_UI:BOOL=OFF
                     -DENABLE_SCRIPTING:BOOL=OFF -DENABLE_BROWSER:BOOL=OFF -DENABLE_HEVC:BOOL=OFF)
  elseif(OS_MACOS)
    set(_cmake_generator_args -G Xcode "-DCMAKE_OSX_ARCHITECTURES:STRING=arm64;x86_64")
    set(_cmake_extra "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()

  if(EXISTS "${dependencies_dir}/lib/cmake/libobs/libobsConfig.cmake" AND
     EXISTS "${dependencies_dir}/.obs-studio_${_obs_version}_${arch}.built")
    message(STATUS "Setting up ${label} build (${arch}) - skipped, already installed")
    return()
  endif()

  message(STATUS "Configure ${label} (${arch})")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_obs_source}" -B "${_obs_build}" ${_cmake_generator_args}
            -DOBS_CMAKE_VERSION:STRING=3.0.0 -DENABLE_PLUGINS:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF
            -DOBS_VERSION_OVERRIDE:STRING=${_obs_version}
            "-DCMAKE_PREFIX_PATH:PATH=${CMAKE_PREFIX_PATH}" ${_is_fresh} ${_cmake_extra}
    RESULT_VARIABLE _process_result
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET)
  message(STATUS "Configure ${label} (${arch}) - done")

  foreach(_config IN ITEMS Debug Release)
    message(STATUS "Build ${label} (${_config} - ${arch})")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${_obs_build}" --target obs-frontend-api --config
              ${_config} --parallel
      RESULT_VARIABLE _process_result
      COMMAND_ERROR_IS_FATAL ANY
      OUTPUT_QUIET)
    message(STATUS "Build ${label} (${_config} - ${arch}) - done")
  endforeach()

  message(STATUS "Install ${label} (${arch})")
  foreach(_config IN ITEMS Debug Release)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --install "${_obs_build}" --component Development --config
              ${_config} --prefix "${dependencies_dir}"
      RESULT_VARIABLE _process_result
      COMMAND_ERROR_IS_FATAL ANY
      OUTPUT_QUIET)
  endforeach()
  file(WRITE "${dependencies_dir}/.obs-studio_${_obs_version}_${arch}.built" "${_obs_version}")
  message(STATUS "Install ${label} (${arch}) - done")
endfunction()

# _check_dependencies: Fetch and extract pre-built OBS build dependencies
function(_check_dependencies)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

  string(JSON dependency_data GET ${buildspec} dependencies)

  foreach(dependency IN LISTS dependencies_list)
    string(JSON data GET ${dependency_data} ${dependency})
    string(JSON version GET ${data} version)
    string(JSON hash GET ${data} hashes ${platform})
    string(JSON url GET ${data} baseUrl)
    string(JSON label GET ${data} label)
    string(JSON revision ERROR_VARIABLE error GET ${data} revision ${platform})

    message(STATUS "Setting up ${label} (${arch})")

    set(file "${${dependency}_filename}")
    set(destination "${${dependency}_destination}")
    string(REPLACE "VERSION" "${version}" file "${file}")
    string(REPLACE "VERSION" "${version}" destination "${destination}")
    string(REPLACE "ARCH" "${arch}" file "${file}")
    string(REPLACE "ARCH" "${arch}" destination "${destination}")
    if(revision)
      string(REPLACE "_REVISION" "_v${revision}" file "${file}")
      string(REPLACE "-REVISION" "-v${revision}" file "${file}")
    else()
      string(REPLACE "_REVISION" "" file "${file}")
      string(REPLACE "-REVISION" "" file "${file}")
    endif()

    if(EXISTS "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256")
      file(READ "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256"
           OBS_DEPENDENCY_${dependency}_${arch}_HASH)
    endif()

    set(skip FALSE)
    if(dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
      if(OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
        _check_deps_version(${version})

        if(found)
          set(skip TRUE)
        endif()
      endif()
    endif()

    if(skip)
      message(STATUS "Setting up ${label} (${arch}) - skipped")
      continue()
    endif()

    if(dependency STREQUAL obs-studio)
      set(url ${url}/${file})
    else()
      set(url ${url}/${version}/${file})
    endif()

    if(NOT EXISTS "${dependencies_dir}/${file}")
      message(STATUS "Downloading ${url}")
      file(DOWNLOAD "${url}" "${dependencies_dir}/${file}" STATUS download_status
           EXPECTED_HASH SHA256=${hash})

      list(GET download_status 0 error_code)
      list(GET download_status 1 error_message)
      if(error_code GREATER 0)
        file(REMOVE "${dependencies_dir}/${file}")
        message(FATAL_ERROR "Unable to download ${url}, failed with error: ${error_message}")
      else()
        message(STATUS "Downloading ${url} - done")
      endif()
    endif()

    if(NOT OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
      file(REMOVE_RECURSE "${dependencies_dir}/${destination}")
    endif()

    if(NOT EXISTS "${dependencies_dir}/${destination}")
      file(MAKE_DIRECTORY "${dependencies_dir}/${destination}")
      if(dependency STREQUAL obs-studio)
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}")
      else()
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}"
             DESTINATION "${dependencies_dir}/${destination}")
      endif()
    endif()

    file(WRITE "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" "${hash}")

    if(dependency STREQUAL prebuilt)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL qt6)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL obs-studio)
      set(_obs_version ${version})
      set(_obs_destination "${destination}")
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}")
    endif()

    message(STATUS "Setting up ${label} (${arch}) - done")
  endforeach()

  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)

  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

  _setup_obs_studio()
endfunction()
