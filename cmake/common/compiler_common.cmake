# OBS plugin template common compiler configuration
include_guard(GLOBAL)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)

set(_obs_clang_common_options
    -fno-strict-aliasing
    -Wno-trigraphs
    -Wno-missing-field-initializers
    -Wno-missing-prototypes
    -Werror=return-type
    -Wunreachable-code
    -Wno-missing-braces
    -Wparentheses
    -Wswitch
    -Wno-unused-function
    -Wno-unused-label
    -Wunused-parameter
    -Wunused-variable
    -Wunused-value
    -Wempty-body
    -Wuninitialized
    -Wno-unknown-pragmas
    -Wfour-char-constants
    -Wconstant-conversion
    -Wno-conversion
    -Wint-conversion
    -Wbool-conversion
    -Wenum-conversion
    -Wnon-literal-null-conversion
    -Wsign-compare
    -Wshorten-64-to-32
    -Wpointer-sign
    -Wnewline-eof
    -Wno-implicit-fallthrough
    -Wdeprecated-declarations
    -Wno-sign-conversion
    -Winfinite-recursion
    -Wcomma
    -Wno-strict-prototypes
    -Wformat-security
    -Wvla
    -Wno-error=shorten-64-to-32)

set(_obs_clang_c_options ${_obs_clang_common_options} -Wno-shadow -Wno-float-conversion)
set(_obs_clang_cxx_options
    ${_obs_clang_common_options}
    -Wno-non-virtual-dtor
    -Wno-overloaded-virtual
    -Wno-exit-time-destructors
    -Wno-shadow
    -Winvalid-offsetof
    -Wmove
    -Werror=block-capture-autoreleasing
    -Wrange-loop-analysis)

if(NOT DEFINED CMAKE_COMPILE_WARNING_AS_ERROR)
  set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
endif()
