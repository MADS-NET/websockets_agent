#ifndef MADS_WEBSOCKETS_TERMINAL_QR_HPP
#define MADS_WEBSOCKETS_TERMINAL_QR_HPP

#include <string>
#include <string_view>

namespace MadsWebsockets {

std::string render_terminal_qr(std::string_view payload);

} // namespace MadsWebsockets

#endif
