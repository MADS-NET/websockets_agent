find_package(Mads REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
find_program(NPX_EXECUTABLE npx REQUIRED)
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

set(REACT_CLIENT_DIR "${CMAKE_SOURCE_DIR}/react_client")
set(REACT_WEB_EXPORT_DIR "${CMAKE_BINARY_DIR}/react_web_export")
set(REACT_WEB_ASSETS_CPP "${CMAKE_BINARY_DIR}/generated/web_assets_generated.cpp")
set(REACT_WEB_INPUTS
  "${REACT_CLIENT_DIR}/App.tsx"
  "${REACT_CLIENT_DIR}/index.ts"
  "${REACT_CLIENT_DIR}/app.json"
  "${REACT_CLIENT_DIR}/package.json"
  "${REACT_CLIENT_DIR}/package-lock.json"
  "${REACT_CLIENT_DIR}/tsconfig.json"
)

add_custom_command(
  OUTPUT "${REACT_WEB_ASSETS_CPP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REACT_WEB_EXPORT_DIR}"
  COMMAND "${NPX_EXECUTABLE}" expo export . --platform web --output-dir "${REACT_WEB_EXPORT_DIR}" --clear
  COMMAND "${CMAKE_COMMAND}"
    -DINPUT_DIR=${REACT_WEB_EXPORT_DIR}
    -DOUTPUT_CPP=${REACT_WEB_ASSETS_CPP}
    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
  DEPENDS
    ${REACT_WEB_INPUTS}
    "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
  WORKING_DIRECTORY "${REACT_CLIENT_DIR}"
  VERBATIM
)

add_library(libMadsWebsockets STATIC
  banner.cpp
  bridge.cpp
  "${REACT_WEB_ASSETS_CPP}"
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
