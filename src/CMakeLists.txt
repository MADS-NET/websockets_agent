find_package(Mads REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
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

if(NOT READLINE_INCLUDE_DIR OR NOT READLINE_LIBRARY)
  message(FATAL_ERROR "readline not found")
endif()

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
    libqr
)

target_compile_features(libMadsWebsocketsQr PRIVATE cxx_std_20)

target_sources(libMadsWebsockets
  PRIVATE
    $<TARGET_OBJECTS:libMadsWebsocketsQr>
)

target_link_libraries(libMadsWebsockets
  PUBLIC
    Mads::Mads
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::LIBWEBSOCKETS
)

target_compile_features(libMadsWebsockets PUBLIC cxx_std_20)

add_executable(mads-websockets
  main/mads-websockets.cpp
)

target_link_libraries(mads-websockets PRIVATE libMadsWebsockets)

add_executable(ws_client
  main/ws_client.cpp
)

target_include_directories(ws_client PRIVATE ${READLINE_INCLUDE_DIR})

target_link_libraries(ws_client
  PRIVATE
    Mads::Mads
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::LIBWEBSOCKETS
    ${READLINE_LIBRARY}
)
