cmake_minimum_required(VERSION 3.24)

project(websockets_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
