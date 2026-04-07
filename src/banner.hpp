#ifndef MADS_WEBSOCKETS_BANNER_HPP
#define MADS_WEBSOCKETS_BANNER_HPP

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MadsWebsockets {

class BannerController {
public:
  BannerController();
  ~BannerController();

  void render_footer(std::vector<std::string> http_addresses,
                     std::size_t connected_clients);
  void update_client_count(std::size_t connected_clients);
  void shutdown();

private:
  enum class KeyPress {
    none,
    arrow_up,
    arrow_down,
  };

  void start_input_loop_if_supported();
  void stop_input_loop();
  void input_loop();
  KeyPress read_keypress();
  void select_previous_address();
  void select_next_address();
  void render_footer_locked();
  static std::size_t count_rendered_lines(const std::string &text);

  std::mutex _mutex;
  std::vector<std::string> _http_addresses;
  std::size_t _selected_index = 0;
  std::size_t _connected_clients = 0;
  std::size_t _footer_lines = 0;
  bool _footer_rendered = false;
  std::atomic<bool> _stop_requested{false};
  std::thread _input_thread;
};

} // namespace MadsWebsockets

#endif
