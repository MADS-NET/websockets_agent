#include <cxxopts.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <readline/history.h>
#include <readline/readline.h>

using namespace std;
using namespace cxxopts;

namespace {

constexpr char kDefaultAddress[] = "ws://localhost:9002";
constexpr char kProtocolName[] = "mads-websockets";

atomic<bool> Running{true};

struct ClientConfig {
  string address = kDefaultAddress;
  bool listen = false;
  bool publish = false;
};

struct ParsedAddress {
  string protocol = "ws";
  string host = "localhost";
  int port = 9002;
  string path = "/";
  bool use_ssl = false;
};

struct ClientState {
  mutex mutex;
  deque<string> outbound_queue;
  string inbound_buffer;
  bool connected = false;
  bool connect_failed = false;
  bool close_requested = false;
  string error;
  lws *wsi = nullptr;
};

class WsClient {
public:
  explicit WsClient(ClientConfig config) : _config(std::move(config)) {}

  int run() {
    auto parsed = parse_address(_config.address);
    if (!parsed.has_value()) {
      cerr << "Invalid address: " << _config.address << endl;
      return EXIT_FAILURE;
    }
    _address = *parsed;

    signal(SIGINT, handle_signal);

    if (!create_context()) {
      cerr << _state.error << endl;
      return EXIT_FAILURE;
    }

    if (!connect()) {
      cerr << _state.error << endl;
      destroy_context();
      return EXIT_FAILURE;
    }

    optional<thread> input_thread;
    if (_config.publish) {
      input_thread.emplace([this]() { input_loop(); });
    }

    while (Running.load()) {
      lws_service(_context, 50);

      {
        lock_guard<mutex> lock(_state.mutex);
        if (_state.connect_failed) {
          Running.store(false);
        }
        if (_state.connected && !_state.outbound_queue.empty()) {
          lws_callback_on_writable(_state.wsi);
        }
        if (_state.close_requested && _state.connected && _state.outbound_queue.empty()) {
          Running.store(false);
        }
      }
    }

    if (input_thread.has_value()) {
      input_thread->join();
    }

    destroy_context();

    if (!_state.error.empty()) {
      cerr << _state.error << endl;
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

private:
  static void handle_signal(int) {
    Running.store(false);
  }

  static optional<ParsedAddress> parse_address(const string &uri) {
    ParsedAddress parsed;
    vector<char> buffer(uri.begin(), uri.end());
    buffer.push_back('\0');

    const char *protocol = nullptr;
    const char *address = nullptr;
    const char *path = "/";
    int port = 0;

    if (lws_parse_uri(buffer.data(), &protocol, &address, &port, &path) != 0) {
      return nullopt;
    }

    parsed.protocol = protocol != nullptr ? protocol : "ws";
    parsed.host = address != nullptr ? address : "localhost";
    parsed.port = port != 0 ? port : (parsed.protocol == "wss" ? 443 : 80);
    parsed.path = path != nullptr && *path != '\0' ? path : "/";
    parsed.use_ssl = parsed.protocol == "wss";
    return parsed;
  }

  bool create_context() {
    static const lws_protocols protocols[] = {
      {
        kProtocolName,
        &WsClient::callback,
        0,
        4096,
        0,
        nullptr,
        0
      },
      LWS_PROTOCOL_LIST_TERM
    };

    lws_context_creation_info info{};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user = this;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    _context = lws_create_context(&info);
    if (_context == nullptr) {
      _state.error = "Failed to create libwebsockets client context";
      return false;
    }
    return true;
  }

  bool connect() {
    lws_client_connect_info info{};
    info.context = _context;
    info.address = _address.host.c_str();
    info.port = _address.port;
    info.path = _address.path.c_str();
    info.host = _address.host.c_str();
    info.origin = _address.host.c_str();
    info.protocol = kProtocolName;
    info.local_protocol_name = kProtocolName;
    info.ssl_connection = _address.use_ssl ? LCCSCF_USE_SSL : 0;
    info.pwsi = &_state.wsi;

    auto *wsi = lws_client_connect_via_info(&info);
    if (wsi == nullptr) {
      _state.error = "Failed to initiate WebSocket client connection";
      return false;
    }
    return true;
  }

  void destroy_context() {
    if (_context != nullptr) {
      lws_context_destroy(_context);
      _context = nullptr;
    }
  }

  void input_loop() {
    while (Running.load()) {
      char *line = readline("> ");
      if (line == nullptr) {
        request_close();
        return;
      }

      string input(line);
      free(line);

      if (input.empty()) {
        request_close();
        return;
      }

      try {
        auto json = nlohmann::json::parse(input);
        auto normalized = json.dump();
        add_history(normalized.c_str());
        {
          lock_guard<mutex> lock(_state.mutex);
          _state.outbound_queue.push_back(std::move(normalized));
        }
      } catch (const std::exception &exc) {
        cerr << "Invalid JSON: " << exc.what() << endl;
      }
    }
  }

  void request_close() {
    lock_guard<mutex> lock(_state.mutex);
    _state.close_requested = true;
  }

  static int callback(
    lws *wsi,
    lws_callback_reasons reason,
    void *,
    void *in,
    size_t len
  ) {
    auto *self = static_cast<WsClient *>(lws_context_user(lws_get_context(wsi)));
    return self != nullptr ? self->handle_callback(wsi, reason, in, len) : 0;
  }

  int handle_callback(lws *wsi, lws_callback_reasons reason, void *in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
      lock_guard<mutex> lock(_state.mutex);
      _state.connected = true;
      _state.wsi = wsi;
      break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
      lock_guard<mutex> lock(_state.mutex);
      _state.connect_failed = true;
      _state.error = in != nullptr ? static_cast<const char *>(in) : "WebSocket connection error";
      break;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
      if (_config.listen) {
        _state.inbound_buffer.append(static_cast<const char *>(in), len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
          try {
            auto json = nlohmann::json::parse(_state.inbound_buffer);
            cout << json.dump() << endl;
          } catch (const std::exception &) {
            cout << _state.inbound_buffer << endl;
          }
          _state.inbound_buffer.clear();
        }
      }
      break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
      string payload;
      {
        lock_guard<mutex> lock(_state.mutex);
        if (_state.outbound_queue.empty()) {
          break;
        }
        payload = std::move(_state.outbound_queue.front());
        _state.outbound_queue.pop_front();
      }

      vector<unsigned char> buffer(LWS_PRE + payload.size());
      copy(payload.begin(), payload.end(), buffer.begin() + LWS_PRE);
      auto written = lws_write(
        wsi,
        buffer.data() + LWS_PRE,
        payload.size(),
        LWS_WRITE_TEXT
      );
      if (written < static_cast<int>(payload.size())) {
        lock_guard<mutex> lock(_state.mutex);
        _state.error = "Failed to send full WebSocket payload";
        Running.store(false);
        return -1;
      }

      lock_guard<mutex> lock(_state.mutex);
      if (!_state.outbound_queue.empty()) {
        lws_callback_on_writable(wsi);
      }
      break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED: {
      lock_guard<mutex> lock(_state.mutex);
      _state.connected = false;
      _state.wsi = nullptr;
      if (Running.load() && !_state.close_requested) {
        _state.error = "WebSocket connection closed";
      }
      Running.store(false);
      break;
    }
    default:
      break;
    }
    return 0;
  }

  ClientConfig _config;
  ParsedAddress _address;
  ClientState _state;
  lws_context *_context = nullptr;
};

} // namespace

int main(int argc, char *argv[]) {
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("a,address", "WebSocket server address as <proto>://<host>:<port>[/path]", value<string>()->default_value(kDefaultAddress))
    ("l,listen", "Listen and print incoming traffic to stdout")
    ("p,publish", "Read JSON from stdin and publish it; empty line exits")
    ("h,help", "Print usage");
  // clang-format on

  auto parsed = options.parse(argc, argv);
  if (parsed.count("help") != 0) {
    cout << options.help() << endl;
    return EXIT_SUCCESS;
  }

  ClientConfig config;
  config.address = parsed["address"].as<string>();
  config.listen = parsed.count("listen") != 0;
  config.publish = parsed.count("publish") != 0;

  if (!config.listen && !config.publish) {
    cerr << "Select at least one mode: --listen or --publish" << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }

  WsClient client(std::move(config));
  return client.run();
}
