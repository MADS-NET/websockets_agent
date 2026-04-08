#include "terminal_qr.hpp"

#if defined(_MSC_VER)
#include <string>
#include <string_view>
#else
#include <qr.h>

#include <sstream>
#include <string>
#include <string_view>
#endif

namespace MadsWebsockets {

namespace {

#if !defined(_MSC_VER)
constexpr int kQuietZoneModules = 2;
constexpr std::string_view kDarkModule = "██";
constexpr std::string_view kLightModule = "  ";

template<int Version>
std::string render_terminal_qr_impl(std::string_view payload) {
  qr::Qr<Version> code;
  if (!code.encode(payload.data(), payload.size(), qr::Ecc::M, -1)) {
    if constexpr (Version < 40) {
      return render_terminal_qr_impl<Version + 1>(payload);
    } else {
      return {};
    }
  }

  std::ostringstream stream;
  const auto side = code.side_size();
  for (int y = -kQuietZoneModules; y < side + kQuietZoneModules; ++y) {
    for (int x = -kQuietZoneModules; x < side + kQuietZoneModules; ++x) {
      const auto dark =
        x >= 0 && y >= 0 && x < side && y < side && code.module(x, y);
      stream << (dark ? kDarkModule : kLightModule);
    }
    stream << '\n';
  }

  return stream.str();
}
#endif

} // namespace

std::string render_terminal_qr(std::string_view payload) {
  if (payload.empty()) {
    return {};
  }
#if defined(_MSC_VER)
  return std::string{"[QR unavailable on this Windows build]\n"} +
         std::string{payload} + '\n';
#else
  return render_terminal_qr_impl<1>(payload);
#endif
}

} // namespace MadsWebsockets
