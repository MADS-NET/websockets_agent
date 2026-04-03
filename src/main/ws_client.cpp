#include <cxxopts.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <readline/history.h>
#include <readline/readline.h>

using namespace std;
using namespace cxxopts;

namespace {

constexpr char kDefaultBaseAddress[] = "ws://localhost:8080";
constexpr char kDefaultTopic[] = "_all";
constexpr char kTopicRoot[] = "/mads";
constexpr char kProtocolName[] = "mads-websockets";

atomic<bool> Running{true};

struct ClientConfig {
  string address = kDefaultBaseAddress;
  vector<string> topics;
  bool listen = false;
  bool publish = false;
  bool debug = false;
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
  unordered_map<lws *, ParsedAddress> connections;
  deque<string> outbound_queue;
  bool connect_failed = false;
  bool close_requested = false;
  string error;
  lws *publish_wsi = nullptr;
};

class WsClient {
public:
  explicit WsClient(ClientConfig config) : _config(std::move(config)) {}

  int run() {
    if (_config.topics.empty()) {
      _config.topics.push_back(kDefaultTopic);
    }

    for (const auto &topic : _config.topics) {
      auto address = build_address(_config.address, topic);
      auto parsed = parse_address(address);
      if (!parsed.has_value()) {
        cerr << "Invalid address: " << address
             << " (expected ws://<host>:<port>/mads/<topic>)" << endl;
        return EXIT_FAILURE;
      }
      _addresses.push_back(*parsed);
    }

    if (_config.publish && _addresses.size() != 1) {
      cerr << "Publish mode requires exactly one topic" << endl;
      return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);

    if (!create_context()) {
      cerr << _state.error << endl;
      return EXIT_FAILURE;
    }

    if (!connect_all()) {
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
        if (_state.publish_wsi != nullptr && !_state.outbound_queue.empty()) {
          lws_callback_on_writable(_state.publish_wsi);
        }
        if (_state.close_requested && _state.outbound_queue.empty()) {
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
    if (!parsed.path.empty() && parsed.path.front() != '/') {
      parsed.path.insert(parsed.path.begin(), '/');
    }
    parsed.use_ssl = parsed.protocol == "wss";
    if (!parsed.path.starts_with("/mads/") || parsed.path.size() <= 6) {
      return nullopt;
    }
    return parsed;
  }

  static string build_address(const string &base_address, string topic) {
    auto normalized_base = base_address;
    while (!normalized_base.empty() && normalized_base.back() == '/') {
      normalized_base.pop_back();
    }
    while (!topic.empty() && topic.front() == '/') {
      topic.erase(topic.begin());
    }
    return normalized_base + kTopicRoot + "/" + topic;
  }

  bool create_context() {
    lws_set_log_level(
      _config.debug ? (LLL_ERR | LLL_WARN | LLL_NOTICE) : 0,
      nullptr
    );

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

  bool connect_all() {
    for (std::size_t index = 0; index < _addresses.size(); ++index) {
      auto &address = _addresses[index];

      lws_client_connect_info info{};
      info.context = _context;
      info.address = address.host.c_str();
      info.port = address.port;
      info.path = address.path.c_str();
      info.host = address.host.c_str();
      info.origin = address.host.c_str();
      info.protocol = kProtocolName;
      info.local_protocol_name = kProtocolName;
      info.ssl_connection = address.use_ssl ? LCCSCF_USE_SSL : 0;

      auto *wsi = lws_client_connect_via_info(&info);
      if (wsi == nullptr) {
        _state.error = "Failed to initiate WebSocket client connection";
        return false;
      }

      {
        lock_guard<mutex> lock(_state.mutex);
        _state.connections.emplace(wsi, address);
        if (_config.publish && index == 0) {
          _state.publish_wsi = wsi;
        }
      }
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
        if (_context != nullptr) {
          lws_cancel_service(_context);
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
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
      lock_guard<mutex> lock(_state.mutex);
      _state.connect_failed = true;
      _state.error = in != nullptr ? static_cast<const char *>(in) : "WebSocket connection error";
      break;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
      if (_config.listen) {
        auto &buffer = _inbound_buffers[wsi];
        buffer.append(static_cast<const char *>(in), len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
          try {
            auto json = nlohmann::json::parse(buffer);
            cout << json.dump() << endl;
          } catch (const std::exception &) {
            cout << buffer << endl;
          }
          buffer.clear();
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
      _state.connections.erase(wsi);
      _inbound_buffers.erase(wsi);
      if (_state.publish_wsi == wsi) {
        _state.publish_wsi = nullptr;
      }
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
  vector<ParsedAddress> _addresses;
  ClientState _state;
  unordered_map<lws *, string> _inbound_buffers;
  lws_context *_context = nullptr;
};

} // namespace

int main(int argc, char *argv[]) {
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("a,address", "WebSocket server address as <proto>://<host>:<port>", value<string>()->default_value(kDefaultBaseAddress))
    ("l,listen", "Listen and print incoming traffic to stdout")
    ("p,publish", "Read JSON from stdin and publish it; empty line exits")
    ("t,topic", "Repeatable topic name appended as /mads/<topic>", value<vector<string>>())
    ("debug", "Enable libwebsockets debug output")
    ("h,help", "Print usage");
  // clang-format on

  auto parsed = options.parse(argc, argv);
  if (parsed.count("help") != 0) {
    cout << options.help() << endl;
    return EXIT_SUCCESS;
  }

  ClientConfig config;
  config.address = parsed["address"].as<string>();
  if (parsed.count("topic") != 0) {
    config.topics = parsed["topic"].as<vector<string>>();
  }
  config.listen = parsed.count("listen") != 0;
  config.publish = parsed.count("publish") != 0;
  config.debug = parsed.count("debug") != 0;

  if (!config.listen && !config.publish) {
    cerr << "Select at least one mode: --listen or --publish" << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }

  WsClient client(std::move(config));
  return client.run();
}
