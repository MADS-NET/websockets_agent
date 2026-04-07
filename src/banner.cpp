#include "banner.hpp"

#include "terminal_qr.hpp"

#include <goback.hpp>
#include <rang.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace MadsWebsockets {

namespace {

using namespace rang;

std::string footer_text(const std::vector<std::string> &http_addresses,
                        std::size_t selected_index,
                        std::size_t connected_clients) {
  std::ostringstream stream;

  if (http_addresses.empty()) {
    stream << "  Browser QR:\n";
    stream << "<no external address found>\n";
    stream << "URL: <unavailable>\n";
    return stream.str();
  }

  auto safe_index = std::min(selected_index, http_addresses.size() - 1);
  auto qr = render_terminal_qr(http_addresses[safe_index]);
  stream << "  Browser QR:\n";
  stream << qr;
  stream << "URL: " << style::bold << http_addresses[safe_index] << style::reset
         << " (↑/↓ to select another address)"
         << '\n';
  stream << "Connected clients: " << style::bold << connected_clients
         << style::reset << '\n';
  return stream.str();
}

#if defined(_WIN32)
bool stdin_is_tty() { return _isatty(_fileno(stdin)) != 0; }
#else
bool stdin_is_tty() { return isatty(STDIN_FILENO) != 0; }
#endif

} // namespace

BannerController::BannerController() = default;

BannerController::~BannerController() { shutdown(); }

void BannerController::render_footer(std::vector<std::string> http_addresses,
                                     std::size_t connected_clients) {
  std::lock_guard<std::mutex> lock(_mutex);
  _http_addresses = std::move(http_addresses);
  _connected_clients = connected_clients;
  if (_selected_index >= _http_addresses.size()) {
    _selected_index = 0;
  }
  render_footer_locked();
  start_input_loop_if_supported();
}

void BannerController::update_client_count(std::size_t connected_clients) {
  std::lock_guard<std::mutex> lock(_mutex);
  _connected_clients = connected_clients;
  if (_footer_rendered) {
    render_footer_locked();
  }
}

void BannerController::shutdown() { stop_input_loop(); }

void BannerController::start_input_loop_if_supported() {
  if (_input_thread.joinable() || _http_addresses.size() <= 1 ||
      !stdin_is_tty()) {
    return;
  }

  _stop_requested.store(false);
  _input_thread = std::thread([this]() { input_loop(); });
}

void BannerController::stop_input_loop() {
  _stop_requested.store(true);
  if (_input_thread.joinable()) {
    _input_thread.join();
  }
}

void BannerController::input_loop() {
#if defined(_WIN32)
  while (!_stop_requested.load()) {
    auto key = read_keypress();
    switch (key) {
    case KeyPress::arrow_up:
      select_previous_address();
      break;
    case KeyPress::arrow_down:
      select_next_address();
      break;
    case KeyPress::none:
    default:
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      break;
    }
  }
#else
  termios original{};
  if (tcgetattr(STDIN_FILENO, &original) != 0) {
    return;
  }

  termios raw = original;
  raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    return;
  }

  while (!_stop_requested.load()) {
    auto key = read_keypress();
    switch (key) {
    case KeyPress::arrow_up:
      select_previous_address();
      break;
    case KeyPress::arrow_down:
      select_next_address();
      break;
    case KeyPress::none:
    default:
      break;
    }
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &original);
#endif
}

BannerController::KeyPress BannerController::read_keypress() {
#if defined(_WIN32)
  if (!_kbhit()) {
    return KeyPress::none;
  }

  auto key = _getch();
  if (key == 0 || key == 224) {
    auto extended = _getch();
    if (extended == 72) {
      return KeyPress::arrow_up;
    }
    if (extended == 80) {
      return KeyPress::arrow_down;
    }
  }
  return KeyPress::none;
#else
  fd_set set;
  FD_ZERO(&set);
  FD_SET(STDIN_FILENO, &set);
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;

  auto ready = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout);
  if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &set)) {
    return KeyPress::none;
  }

  unsigned char bytes[3]{};
  auto count = read(STDIN_FILENO, bytes, sizeof(bytes));
  if (count >= 3 && bytes[0] == 27 && bytes[1] == '[') {
    if (bytes[2] == 'A') {
      return KeyPress::arrow_up;
    }
    if (bytes[2] == 'B') {
      return KeyPress::arrow_down;
    }
  }

  return KeyPress::none;
#endif
}

void BannerController::select_previous_address() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_http_addresses.size() <= 1) {
    return;
  }
  _selected_index =
      (_selected_index + _http_addresses.size() - 1) % _http_addresses.size();
  render_footer_locked();
}

void BannerController::select_next_address() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_http_addresses.size() <= 1) {
    return;
  }
  _selected_index = (_selected_index + 1) % _http_addresses.size();
  render_footer_locked();
}

void BannerController::render_footer_locked() {
  auto footer = footer_text(_http_addresses, _selected_index,
                            _connected_clients);
  auto next_lines = count_rendered_lines(footer);

  if (_footer_rendered && _footer_lines > 0) {
    std::cout << Mads::goback(_footer_lines);
  }

  std::cout << footer << std::flush;
  _footer_lines = next_lines;
  _footer_rendered = true;
}

std::size_t BannerController::count_rendered_lines(const std::string &text) {
  auto lines =
      static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  if (!text.empty() && text.back() != '\n') {
    ++lines;
  }
  return lines;
}

} // namespace MadsWebsockets
