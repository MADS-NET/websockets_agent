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

add_library(libMadsWebsockets STATIC
  bridge.cpp
)

target_include_directories(libMadsWebsockets
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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
