cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat>=59
    libavcodec>=59
    libavutil
    libswresample
    libswscale
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The embedded SRTLA receiver (irlserver/srtla) uses epoll and is Linux-only.")
endif()

# The SRTLA receiver is compiled into the plugin from pinned upstream sources;
# no srtla_rec executable is built or started. Set FETCHCONTENT_SOURCE_DIR_SRTLA
# or FETCHCONTENT_SOURCE_DIR_SPDLOG to use local checkouts (offline builds).
include(FetchContent)
set(SRTLA_GIT_TAG "b8359bc80ce99ac4fc8f603cfa455cdd0a0a4241" CACHE STRING "irlserver/srtla revision to embed")
FetchContent_Declare(srtla
    GIT_REPOSITORY https://github.com/irlserver/srtla.git
    GIT_TAG ${SRTLA_GIT_TAG}
    GIT_SUBMODULES ""
    # Only the sources are needed; skip upstream's CMakeLists (executables).
    SOURCE_SUBDIR do-not-build
)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/irlserver/spdlog.git
    GIT_TAG 1.9.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-build
)
FetchContent_MakeAvailable(srtla spdlog)

add_library(srtla_embedded STATIC
    ${srtla_SOURCE_DIR}/src/common.c
    ${srtla_SOURCE_DIR}/src/connection/connection.cpp
    ${srtla_SOURCE_DIR}/src/connection/connection_group.cpp
    ${srtla_SOURCE_DIR}/src/connection/connection_registry.cpp
    ${srtla_SOURCE_DIR}/src/quality/metrics_collector.cpp
    ${srtla_SOURCE_DIR}/src/quality/quality_evaluator.cpp
    ${srtla_SOURCE_DIR}/src/quality/load_balancer.cpp
    ${srtla_SOURCE_DIR}/src/protocol/srtla_handler.cpp
    ${srtla_SOURCE_DIR}/src/protocol/srt_handler.cpp
    ${srtla_SOURCE_DIR}/src/metrics/prometheus.cpp
    ${srtla_SOURCE_DIR}/src/utils/network_utils.cpp
    ${srtla_SOURCE_DIR}/src/utils/nak_dedup.cpp
    ${srtla_SOURCE_DIR}/src/utils/auth_rate_limiter.cpp
)
target_include_directories(srtla_embedded SYSTEM PUBLIC
    ${srtla_SOURCE_DIR}/src
    ${spdlog_SOURCE_DIR}/include
)
target_compile_definitions(srtla_embedded
    PUBLIC SPDLOG_HEADER_ONLY
    PRIVATE VERSION="srtla-${SRTLA_GIT_TAG}"
)
target_compile_features(srtla_embedded PUBLIC cxx_std_17)
target_compile_options(srtla_embedded PRIVATE -w)
target_link_libraries(srtla_embedded PUBLIC Threads::Threads)
set_target_properties(srtla_embedded PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    COMPILE_WARNING_AS_ERROR OFF
)

# libsrt is built from pinned sources as a static PIC library and linked into
# the plugin together with static OpenSSL libcrypto (SRT encryption), so the
# plugin does not depend on a system libsrt.
include(ExternalProject)
set(SRT_GIT_TAG "v1.5.7" CACHE STRING "Haivision libsrt release to embed")
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

set(SRT_PREFIX "${CMAKE_BINARY_DIR}/srt-install")
set(SRT_STATIC_LIB "${SRT_PREFIX}/lib/libsrt.a")
ExternalProject_Add(srt_external
    GIT_REPOSITORY https://github.com/Haivision/srt.git
    GIT_TAG ${SRT_GIT_TAG}
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${SRT_PREFIX}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DENABLE_APPS=OFF
        -DENABLE_ENCRYPTION=ON
        -DUSE_ENCLIB=openssl-evp
        -DOPENSSL_USE_STATIC_LIBS=TRUE
        -DENABLE_UNITTESTS=OFF
        -DENABLE_CXX_DEPS=OFF
    BUILD_BYPRODUCTS ${SRT_STATIC_LIB}
)
file(MAKE_DIRECTORY "${SRT_PREFIX}/include")
add_library(srt_static STATIC IMPORTED)
set_target_properties(srt_static PROPERTIES IMPORTED_LOCATION ${SRT_STATIC_LIB})
target_include_directories(srt_static INTERFACE "${SRT_PREFIX}/include")
target_link_libraries(srt_static INTERFACE OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(srt_static srt_external)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    OBS::libobs
    PkgConfig::FFMPEG
    srt_static
    Threads::Threads
    srtla_embedded
)
# Keep embedded libsrt/libcrypto symbols private: FFmpeg loads the system
# libsrt into the same OBS process, and the two must not interpose.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    "LINKER:--exclude-libs,ALL"
    "LINKER:-Bsymbolic"
    "LINKER:--no-undefined"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.c
    src/srtla-source.cpp
    src/srtla-receiver.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Production builds ship a stripped module (no debug info or local symbols).
option(STRIP_PLUGIN "Strip the plugin after building (Release/MinSizeRel)" ON)
if(STRIP_PLUGIN AND CMAKE_STRIP)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND "$<$<CONFIG:Release,MinSizeRel>:${CMAKE_STRIP};--strip-unneeded;$<TARGET_FILE:${CMAKE_PROJECT_NAME}>>"
        COMMAND_EXPAND_LISTS
        COMMENT "Stripping $<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}>"
        VERBATIM
    )
endif()
