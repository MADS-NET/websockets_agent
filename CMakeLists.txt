cmake_minimum_required(VERSION 3.24)

project(websockets_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(CTest)

if(NOT Mads_DIR)
  execute_process(
    COMMAND mads -p
    OUTPUT_VARIABLE MADS_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(MADS_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${MADS_PREFIX}")
  endif()
endif()

FetchContent_Declare(
  qr
  GIT_REPOSITORY https://github.com/nth-eye/qr.git
  GIT_TAG 38e61f76645ab78f2fa0d14ce4a1d29d8249f3d1
)
FetchContent_GetProperties(qr)
if(NOT qr_POPULATED)
  FetchContent_Populate(qr)
endif()

add_library(libqr INTERFACE)
target_include_directories(libqr SYSTEM INTERFACE "${qr_SOURCE_DIR}/inc")
target_compile_features(libqr INTERFACE cxx_std_17)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
