#ifndef MADS_WEBSOCKETS_WEB_ASSETS_HPP
#define MADS_WEBSOCKETS_WEB_ASSETS_HPP

#include <cstddef>
#include <string_view>

namespace MadsWebsockets {

struct EmbeddedWebAsset {
  std::string_view path;
  const char *content_type;
  const unsigned char *data;
  std::size_t size;
  bool spa_entry = false;
  bool cache_forever = false;
};

bool has_embedded_web_assets();
const EmbeddedWebAsset *find_embedded_web_asset(std::string_view path);

} // namespace MadsWebsockets

#endif
