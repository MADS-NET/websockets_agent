if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_CPP)
  message(FATAL_ERROR "INPUT_DIR and OUTPUT_CPP are required")
endif()

file(GLOB_RECURSE WEB_ASSET_FILES RELATIVE "${INPUT_DIR}" "${INPUT_DIR}/*")
list(SORT WEB_ASSET_FILES)

function(detect_content_type relative_path out_var)
  get_filename_component(extension "${relative_path}" EXT)
  string(TOLOWER "${extension}" extension)

  if(extension STREQUAL ".html")
    set(content_type "text/html; charset=utf-8")
  elseif(extension STREQUAL ".js")
    set(content_type "application/javascript; charset=utf-8")
  elseif(extension STREQUAL ".css")
    set(content_type "text/css; charset=utf-8")
  elseif(extension STREQUAL ".json")
    set(content_type "application/json; charset=utf-8")
  elseif(extension STREQUAL ".svg")
    set(content_type "image/svg+xml")
  elseif(extension STREQUAL ".png")
    set(content_type "image/png")
  elseif(extension STREQUAL ".jpg" OR extension STREQUAL ".jpeg")
    set(content_type "image/jpeg")
  elseif(extension STREQUAL ".webp")
    set(content_type "image/webp")
  elseif(extension STREQUAL ".ico")
    set(content_type "image/x-icon")
  elseif(extension STREQUAL ".map")
    set(content_type "application/json; charset=utf-8")
  elseif(extension STREQUAL ".txt")
    set(content_type "text/plain; charset=utf-8")
  else()
    set(content_type "application/octet-stream")
  endif()

  set(${out_var} "${content_type}" PARENT_SCOPE)
endfunction()

function(sanitize_symbol_name input out_var)
  string(MAKE_C_IDENTIFIER "${input}" sanitized)
  set(${out_var} "${sanitized}" PARENT_SCOPE)
endfunction()

file(WRITE "${OUTPUT_CPP}" "#include \"web_assets.hpp\"\n\n#include <string_view>\n\nnamespace {\n")

set(asset_entries "")

foreach(relative_path IN LISTS WEB_ASSET_FILES)
  if(IS_DIRECTORY "${INPUT_DIR}/${relative_path}")
    continue()
  endif()

  string(REPLACE "\\" "/" relative_path "${relative_path}")
  set(asset_path "/${relative_path}")
  sanitize_symbol_name("${relative_path}" asset_symbol)
  detect_content_type("${relative_path}" content_type)

  file(READ "${INPUT_DIR}/${relative_path}" asset_hex HEX)
  string(LENGTH "${asset_hex}" asset_hex_length)
  math(EXPR asset_byte_count "${asset_hex_length} / 2")
  string(REGEX REPLACE "(..)" "0x\\1," asset_bytes "${asset_hex}")

  file(APPEND "${OUTPUT_CPP}" "static const unsigned char ${asset_symbol}[] = {\n${asset_bytes}\n};\n\n")

  if(relative_path STREQUAL "index.html")
    set(spa_entry "true")
  else()
    set(spa_entry "false")
  endif()

  if(relative_path MATCHES [[\.[0-9a-f]{8,}\.]])
    set(cache_forever "true")
  else()
    set(cache_forever "false")
  endif()

  string(APPEND asset_entries
    "  {\"${asset_path}\", \"${content_type}\", ${asset_symbol}, sizeof(${asset_symbol}), ${spa_entry}, ${cache_forever}},\n")
endforeach()

file(APPEND "${OUTPUT_CPP}" "static const MadsWebsockets::EmbeddedWebAsset kAssets[] = {\n${asset_entries}};\n\n} // namespace\n\nnamespace MadsWebsockets {\n\nbool has_embedded_web_assets() {\n  return sizeof(kAssets) / sizeof(kAssets[0]) != 0;\n}\n\nconst EmbeddedWebAsset *find_embedded_web_asset(std::string_view path) {\n  for (const auto &asset : kAssets) {\n    if (asset.path == path) {\n      return &asset;\n    }\n  }\n  return nullptr;\n}\n\n} // namespace MadsWebsockets\n")
