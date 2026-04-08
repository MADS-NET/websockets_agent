find_package(Mads REQUIRED CONFIG)
set(LIBWEBSOCKETS_LINK_TARGET "")
set(LIBWEBSOCKETS_FETCHED OFF)

find_package(libwebsockets CONFIG QUIET
  NO_CMAKE_PACKAGE_REGISTRY
  NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
)
find_package(Libwebsockets CONFIG QUIET
  NO_CMAKE_PACKAGE_REGISTRY
  NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
)

foreach(candidate
  unofficial::libwebsockets::libwebsockets
  libwebsockets::websockets
  websockets
  websockets_shared
  websockets_static
)
  if(TARGET "${candidate}")
    set(LIBWEBSOCKETS_LINK_TARGET "${candidate}")
    break()
  endif()
endforeach()

if(NOT LIBWEBSOCKETS_LINK_TARGET)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBWEBSOCKETS QUIET IMPORTED_TARGET libwebsockets)
    if(TARGET PkgConfig::LIBWEBSOCKETS)
      set(LIBWEBSOCKETS_LINK_TARGET PkgConfig::LIBWEBSOCKETS)
    endif()
  endif()
endif()

if(NOT LIBWEBSOCKETS_LINK_TARGET)
  set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
  set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_SSL OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_SYS_STATE OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_SYS_SMD OFF CACHE BOOL "" FORCE)
  set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
  set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
  set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LWS_WITHOUT_EXTENSIONS ON CACHE BOOL "" FORCE)

  FetchContent_Declare(
    libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG v4.3.5
  )
  FetchContent_MakeAvailable(libwebsockets)

  foreach(candidate
    websockets
    websockets_static
    libwebsockets
  )
    if(TARGET "${candidate}")
      set(LIBWEBSOCKETS_LINK_TARGET "${candidate}")
      set(LIBWEBSOCKETS_FETCHED ON)
      break()
    endif()
  endforeach()
endif()

if(NOT LIBWEBSOCKETS_LINK_TARGET)
  message(FATAL_ERROR "libwebsockets was not found, and the vendored fallback did not expose a usable target")
endif()

find_path(READLINE_INCLUDE_DIR readline/readline.h
  HINTS
    /opt/homebrew/opt/readline/include
    /usr/local/opt/readline/include
)
find_library(READLINE_LIBRARY readline
  HINTS
    /opt/homebrew/opt/readline/lib
    /usr/local/opt/readline/lib
)

set(WEB_ASSETS_DIR "${CMAKE_SOURCE_DIR}/web")
set(WEB_ASSETS_CPP "${CMAKE_BINARY_DIR}/generated/web_assets_generated.cpp")
file(GLOB WEB_ASSET_INPUTS "${WEB_ASSETS_DIR}/*")

add_custom_command(
  OUTPUT "${WEB_ASSETS_CPP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}"
    -DINPUT_DIR=${WEB_ASSETS_DIR}
    -DOUTPUT_CPP=${WEB_ASSETS_CPP}
    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
  DEPENDS
    ${WEB_ASSET_INPUTS}
    "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
  VERBATIM
)

add_library(libMadsWebsockets STATIC
  banner.cpp
  bridge.cpp
  "${WEB_ASSETS_CPP}"
)

add_library(libMadsWebsocketsQr OBJECT
  terminal_qr.cpp
)

target_include_directories(libMadsWebsockets
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(libMadsWebsocketsQr
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(libMadsWebsocketsQr
  PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:libqr>
)

target_compile_features(libMadsWebsocketsQr PRIVATE cxx_std_20)

target_sources(libMadsWebsockets
  PRIVATE
    $<TARGET_OBJECTS:libMadsWebsocketsQr>
)

target_link_libraries(libMadsWebsockets
  PUBLIC
    Mads::Mads
    ${LIBWEBSOCKETS_LINK_TARGET}
)

target_compile_features(libMadsWebsockets PUBLIC cxx_std_20)

add_executable(mads-websockets
  main/mads-websockets.cpp
)

target_link_libraries(mads-websockets PRIVATE libMadsWebsockets)

add_executable(ws_client
  main/ws_client.cpp
)

if(READLINE_INCLUDE_DIR)
  target_include_directories(ws_client PRIVATE ${READLINE_INCLUDE_DIR})
  target_compile_definitions(ws_client PRIVATE MADS_HAS_READLINE=1)
endif()

target_link_libraries(ws_client
  PRIVATE
    Mads::Mads
    ${LIBWEBSOCKETS_LINK_TARGET}
)

if(READLINE_LIBRARY)
  target_link_libraries(ws_client PRIVATE ${READLINE_LIBRARY})
endif()

if(LIBWEBSOCKETS_FETCHED)
  target_compile_definitions(ws_client PRIVATE MADS_LWS_WITHOUT_TLS=1)
endif()
