#include "bridge.hpp"
#include "web_assets.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libwebsockets.h>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#endif

namespace MadsWebsockets {

namespace {

constexpr char kControlTopic[] = "control";
constexpr char kProtocolName[] = "mads-websockets";
constexpr char kHttpProtocolName[] = "http";
constexpr char kDefaultWsBasePath[] = "/mads";
constexpr char kDefaultHttpBasePath[] = "/mads";
constexpr char kAllTopics[] = "_all";
constexpr std::size_t kHttpWriteChunkSize = 16 * 1024;

std::string json_dump(const nlohmann::json &value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string age_string(std::chrono::steady_clock::time_point when) {
  if (when == std::chrono::steady_clock::time_point{}) {
    return "-";
  }
  auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - when);
  std::ostringstream stream;
  stream << age.count() << "ms";
  return stream.str();
}

std::string normalize_base_path(std::string path, std::string_view fallback) {
  if (path.empty()) {
    return std::string(fallback);
  }
  if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string normalize_ws_base_path(std::string path) {
  return normalize_base_path(std::move(path), kDefaultWsBasePath);
}

std::string normalize_http_base_path(std::string path) {
  return normalize_base_path(std::move(path), kDefaultHttpBasePath);
}

std::string websocket_root_uri(const BridgeConfig &config) {
  return "ws://" + config.ws_host + ":" + std::to_string(config.ws_port) +
         normalize_ws_base_path(config.ws_path);
}

std::string websocket_uri_for_host(const BridgeConfig &config,
                                   std::string_view host) {
  return "ws://" + std::string(host) + ":" + std::to_string(config.ws_port) +
         normalize_ws_base_path(config.ws_path);
}

std::string http_uri_for_host(const BridgeConfig &config,
                              std::string_view host) {
  return "http://" + std::string(host) + ":" +
         std::to_string(config.http_port) +
         normalize_http_base_path(config.http_path);
}

bool is_excluded_address(std::string_view address) {
  return address.empty() || address == "0.0.0.0" || address == "127.0.0.1" ||
         address == "::" || address == "::1";
}

#if defined(_WIN32)
std::vector<std::string> collect_external_ip_addresses() {
  ULONG buffer_size = 16 * 1024;
  std::vector<unsigned char> buffer(buffer_size);
  auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;
  auto result =
      GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &buffer_size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(buffer_size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses,
                                  &buffer_size);
  }
  if (result != NO_ERROR) {
    return {};
  }

  std::set<std::string> collected;
  std::array<char, INET6_ADDRSTRLEN> text_buffer{};
  for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr) {
        continue;
      }
      auto family = unicast->Address.lpSockaddr->sa_family;
      const void *raw_address = nullptr;
      if (family == AF_INET) {
        raw_address =
            &reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr)
                 ->sin_addr;
      } else {
        continue;
      }
      if (InetNtopA(family, const_cast<void *>(raw_address), text_buffer.data(),
                    static_cast<DWORD>(text_buffer.size())) == nullptr) {
        continue;
      }
      std::string address = text_buffer.data();
      if (!is_excluded_address(address)) {
        collected.insert(std::move(address));
      }
    }
  }

  return {collected.begin(), collected.end()};
}
#else
std::vector<std::string> collect_external_ip_addresses() {
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) {
    return {};
  }

  std::set<std::string> collected;
  std::array<char, INET6_ADDRSTRLEN> text_buffer{};
  for (auto *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_UP) == 0 ||
        (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    auto family = ifa->ifa_addr->sa_family;
    const void *raw_address = nullptr;
    if (family == AF_INET) {
      raw_address = &reinterpret_cast<sockaddr_in *>(ifa->ifa_addr)->sin_addr;
    } else {
      continue;
    }
    if (inet_ntop(family, raw_address, text_buffer.data(),
                  text_buffer.size()) == nullptr) {
      continue;
    }
    std::string address = text_buffer.data();
    if (!is_excluded_address(address)) {
      collected.insert(std::move(address));
    }
  }

  freeifaddrs(interfaces);
  return {collected.begin(), collected.end()};
}
#endif

std::vector<std::string> external_hosts_for_bind(std::string_view host) {
  if (!is_excluded_address(host) && host != "localhost") {
    return {std::string(host)};
  }

  return collect_external_ip_addresses();
}

std::vector<std::string> websocket_external_hosts(const BridgeConfig &config) {
  return external_hosts_for_bind(config.ws_host);
}

std::vector<std::string> websocket_external_uris(const BridgeConfig &config) {
  auto hosts = websocket_external_hosts(config);
  std::vector<std::string> uris;
  uris.reserve(hosts.size());
  for (const auto &host : hosts) {
    uris.push_back(websocket_uri_for_host(config, host));
  }
  return uris;
}

std::vector<std::string> http_external_hosts(const BridgeConfig &config) {
  if (!config.http_enabled) {
    return {};
  }
  return external_hosts_for_bind(config.http_host);
}

std::vector<std::string> http_external_uris(const BridgeConfig &config) {
  auto hosts = http_external_hosts(config);
  std::vector<std::string> uris;
  uris.reserve(hosts.size());
  for (const auto &host : hosts) {
    uris.push_back(http_uri_for_host(config, host));
  }
  return uris;
}

std::optional<std::string>
websocket_bootstrap_payload(const BridgeConfig &config,
                            const std::vector<std::string> &hosts) {
  if (hosts.empty()) {
    return std::nullopt;
  }

  nlohmann::json payload = {{"scheme", "ws"},
                            {"port", config.ws_port},
                            {"path", normalize_ws_base_path(config.ws_path)},
                            {"addresses", hosts}};
  return json_dump(payload);
}

std::optional<std::string> topic_from_uri(std::string_view uri,
                                          std::string_view base_path) {
  if (uri.empty()) {
    return std::nullopt;
  }
  if (!uri.starts_with(base_path)) {
    return std::nullopt;
  }

  auto topic_pos = base_path.size();
  if (uri.size() <= topic_pos || uri[topic_pos] != '/') {
    return std::nullopt;
  }

  auto topic = std::string(uri.substr(topic_pos + 1));
  if (topic.empty()) {
    return std::nullopt;
  }

  auto query_pos = topic.find_first_of("?#");
  if (query_pos != std::string::npos) {
    topic.erase(query_pos);
  }

  if (topic.empty()) {
    return std::nullopt;
  }

  return topic;
}

std::optional<std::string> request_uri(lws *wsi) {
  auto length = lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI);
  if (length <= 0) {
    return std::nullopt;
  }

  std::string uri(static_cast<std::size_t>(length), '\0');
  auto copied = lws_hdr_copy(wsi, uri.data(), static_cast<int>(uri.size()) + 1,
                             WSI_TOKEN_GET_URI);
  if (copied <= 0) {
    return std::nullopt;
  }

  uri.resize(static_cast<std::size_t>(copied));
  return uri;
}

const char *bind_iface(const std::string &host) {
  if (host.empty() || host == "0.0.0.0" || host == "::") {
    return nullptr;
  }
  return host.c_str();
}

struct SessionState {
  std::string topic;
  std::string rx_buffer;
  std::deque<std::string> tx_queue;
};

struct HttpSessionState {
  const EmbeddedWebAsset *asset = nullptr;
  std::size_t offset = 0;
  EmbeddedWebAsset dynamic_asset{};
  std::string owned_payload;
};

bool is_http_asset_path(std::string_view path) {
  return find_embedded_web_asset(path) != nullptr;
}

std::string resolve_http_request_path(const BridgeConfig &config,
                                      std::string_view uri) {
  auto http_base_path = normalize_http_base_path(config.http_path);
  if (uri.empty()) {
    return "/index.html";
  }

  if (uri == "/") {
    return "/index.html";
  }

  if (uri == http_base_path || uri == http_base_path + "/") {
    return "/index.html";
  }

  if (uri.starts_with(http_base_path + "/")) {
    std::string stripped = std::string(uri.substr(http_base_path.size()));
    if (is_http_asset_path(stripped)) {
      return stripped;
    }
    return "/index.html";
  }

  if (is_http_asset_path(uri)) {
    return std::string(uri);
  }

  return {};
}

std::string http_cache_control_header(const EmbeddedWebAsset &asset) {
  if (asset.spa_entry) {
    return "no-cache";
  }
  if (asset.cache_forever) {
    return "public, max-age=31536000, immutable";
  }
  return "public, max-age=3600";
}

int write_http_response_headers(lws *wsi, const EmbeddedWebAsset &asset) {
  std::array<unsigned char, 1024> buffer{};
  unsigned char *start = buffer.data();
  unsigned char *p = start;
  unsigned char *end = buffer.data() + buffer.size() - 1;

  if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, asset.content_type,
                                  asset.size, &p, end) != 0) {
    return 1;
  }

  auto cache_control = http_cache_control_header(asset);
  if (lws_add_http_header_by_name(
          wsi, reinterpret_cast<const unsigned char *>("cache-control:"),
          reinterpret_cast<const unsigned char *>(cache_control.c_str()),
          cache_control.size(), &p, end) != 0) {
    return 1;
  }

  if (lws_finalize_write_http_header(wsi, start, &p, end) != 0) {
    return 1;
  }

  return 0;
}

} // namespace

BridgeConfig BridgeConfig::from_settings(const nlohmann::json &settings) {
  BridgeConfig config;

  if (settings.contains("ws_host")) {
    config.ws_host = settings["ws_host"].get<std::string>();
  }
  if (settings.contains("ws_port")) {
    config.ws_port = settings["ws_port"].get<int>();
  }
  if (settings.contains("ws_path")) {
    config.ws_path = settings["ws_path"].get<std::string>();
  }
  if (settings.contains("http_enabled")) {
    config.http_enabled = settings["http_enabled"].get<bool>();
  }
  if (settings.contains("http_host")) {
    config.http_host = settings["http_host"].get<std::string>();
  }
  if (settings.contains("http_port")) {
    config.http_port = settings["http_port"].get<int>();
  }
  if (settings.contains("http_path")) {
    config.http_path = settings["http_path"].get<std::string>();
  }
  if (settings.contains("period")) {
    config.period = std::chrono::milliseconds(settings["period"].get<int>());
  }
  if (settings.contains("receive_timeout")) {
    config.receive_timeout =
        std::chrono::milliseconds(settings["receive_timeout"].get<int>());
  }
  if (settings.contains("non_blocking")) {
    config.non_blocking = settings["non_blocking"].get<bool>();
  }
  if (settings.contains("ws_silent")) {
    config.ws_silent = settings["ws_silent"].get<bool>();
  }

  return config;
}

BridgeCore::BridgeCore(IMadsTransport &mads_transport,
                       IWebSocketTransport &ws_transport)
    : _mads_transport(mads_transport), _ws_transport(ws_transport) {}

bool BridgeCore::tick() {
  _mads_transport.poll();
  _ws_transport.poll();

  while (true) {
    auto message = _ws_transport.pop_incoming_message();
    if (!message.has_value()) {
      break;
    }

    if (!_mads_transport.publish(*message)) {
      _healthy = false;
      _should_stop = true;
      _error = _mads_transport.error();
      return false;
    }
  }

  while (true) {
    auto message = _mads_transport.pop_incoming();
    if (!message.has_value()) {
      break;
    }

    if (!_ws_transport.broadcast(*message)) {
      _healthy = false;
      _should_stop = true;
      _error = _ws_transport.error();
      return false;
    }
  }

  if (!_mads_transport.healthy()) {
    _healthy = false;
    _should_stop = true;
    _error = _mads_transport.error();
    return false;
  }

  if (!_ws_transport.healthy()) {
    _healthy = false;
    _should_stop = true;
    _error = _ws_transport.error();
    return false;
  }

  if (_mads_transport.should_stop()) {
    _should_stop = true;
  }

  return _healthy;
}

void BridgeCore::shutdown() {
  _ws_transport.stop();
  _mads_transport.stop();
}

bool BridgeCore::healthy() const { return _healthy; }

bool BridgeCore::should_stop() const { return _should_stop; }

std::string BridgeCore::error() const { return _error; }

std::string BridgeCore::stats() const {
  std::ostringstream stream;
  stream << "mads{" << _mads_transport.stats() << "} "
         << "ws{" << _ws_transport.stats() << "}";
  return stream.str();
}

MadsTransport::MadsTransport(std::unique_ptr<Mads::Agent> agent,
                             BridgeConfig config)
    : _agent(std::move(agent)), _config(std::move(config)) {}

bool MadsTransport::start() {
  if (_agent == nullptr) {
    _healthy = false;
    _error = "MADS agent is not available";
    return false;
  }

  try {
    _agent->set_receive_timeout(_config.receive_timeout);
    _agent->register_event(Mads::event_type::startup);
    _started = true;
    return true;
  } catch (const std::exception &exc) {
    _healthy = false;
    _error = exc.what();
    return false;
  }
}

void MadsTransport::poll() {
  if (!_started || !_healthy) {
    return;
  }

  try {
    while (true) {
      auto type = _agent->receive(true);
      if (type != Mads::message_type::json) {
        break;
      }

      auto [topic, payload_str] = _agent->last_message();

      if (topic != kControlTopic) {
        _incoming_queue.push_back(
            BridgeMessage{topic, nlohmann::json::parse(payload_str)});
        ++_received_json_count;
        _last_received_topic = topic;
        _last_received_at = std::chrono::steady_clock::now();
      }
    }

    if (!Mads::running.load()) {
      _should_stop = true;
    }
  } catch (const std::exception &exc) {
    _healthy = false;
    _error = exc.what();
  }
}

bool MadsTransport::publish(const BridgeMessage &message) {
  if (!_started || !_healthy) {
    return false;
  }

  try {
    _agent->publish(message.message, message.topic);
    ++_published_count;
    _last_published_topic = message.topic;
    _last_published_at = std::chrono::steady_clock::now();
    return true;
  } catch (const std::exception &exc) {
    _healthy = false;
    _error = exc.what();
    return false;
  }
}

std::optional<BridgeMessage> MadsTransport::pop_incoming() {
  if (_incoming_queue.empty()) {
    return std::nullopt;
  }
  auto incoming = _incoming_queue.front();
  _incoming_queue.pop_front();
  return incoming;
}

void MadsTransport::stop() {
  if (_agent == nullptr) {
    return;
  }

  try {
    _agent->register_event(Mads::event_type::shutdown);
    _restart_requested = _agent->restart();
    _agent->disconnect();
  } catch (const std::exception &exc) {
    _healthy = false;
    _error = exc.what();
  }
  _started = false;
}

bool MadsTransport::healthy() const { return _healthy; }

bool MadsTransport::should_stop() const { return _should_stop; }

bool MadsTransport::restart_requested() const { return _restart_requested; }

std::string MadsTransport::error() const { return _error; }

std::string MadsTransport::stats() const {
  std::ostringstream stream;
  stream << "received=" << _received_json_count
         << " published=" << _published_count
         << " queued=" << _incoming_queue.size() << " last_rx_topic="
         << (_last_received_topic.empty() ? "-" : _last_received_topic)
         << " last_tx_topic="
         << (_last_published_topic.empty() ? "-" : _last_published_topic)
         << " last_rx_age=" << age_string(_last_received_at)
         << " last_tx_age=" << age_string(_last_published_at)
         << " healthy=" << (_healthy ? "true" : "false");
  return stream.str();
}

struct WebSocketTransport::Impl {
  explicit Impl(BridgeConfig bridge_config)
      : config(std::move(bridge_config)),
        ws_base_path(normalize_ws_base_path(config.ws_path)),
        http_base_path(normalize_http_base_path(config.http_path)) {}

  BridgeConfig config;
  std::string ws_base_path;
  std::string http_base_path;
  lws_context *ws_context = nullptr;
  lws_context *http_context = nullptr;
  WebSocketTransport *owner = nullptr;
  mutable std::mutex mutex;
  std::unordered_map<lws *, SessionState> sessions;
  std::deque<BridgeMessage> inbound_messages;
  bool started = false;
  bool healthy = true;
  bool http_shared_with_ws = false;
  std::string error;
  std::size_t accepted_connections = 0;
  std::size_t closed_connections = 0;
  std::size_t inbound_frames = 0;
  std::size_t outbound_enqueued = 0;
  std::size_t outbound_written = 0;
  bool client_count_changed = false;
  std::chrono::steady_clock::time_point last_inbound_at{};
  std::chrono::steady_clock::time_point last_outbound_at{};
  std::atomic<bool> stop_requested{false};
  std::thread service_thread;

  static WebSocketTransport *transport_for_wsi(lws *wsi) {
    auto *context = lws_get_context(wsi);
    if (context == nullptr) {
      return nullptr;
    }
    return static_cast<WebSocketTransport *>(lws_context_user(context));
  }

  static int ws_callback(lws *wsi, lws_callback_reasons reason, void *user,
                         void *in, size_t len) {
    auto *self = transport_for_wsi(wsi);
    if (self == nullptr || self->_impl == nullptr) {
      return 0;
    }
    return self->_impl->handle_ws_callback(wsi, reason, user, in, len);
  }

  static int http_callback(lws *wsi, lws_callback_reasons reason, void *user,
                           void *in, size_t len) {
    auto *self = transport_for_wsi(wsi);
    if (self == nullptr || self->_impl == nullptr) {
      return 0;
    }
    return self->_impl->handle_http_callback(wsi, reason, user, in, len);
  }

  static const lws_protocols *ws_protocols() {
    static const lws_protocols protocols[] = {
        {kProtocolName, &WebSocketTransport::Impl::ws_callback, 0, 4096, 0,
         nullptr, 0},
        LWS_PROTOCOL_LIST_TERM};
    return protocols;
  }

  static const lws_protocols *http_protocols() {
    static const lws_protocols protocols[] = {
        {kHttpProtocolName, &WebSocketTransport::Impl::http_callback,
         sizeof(HttpSessionState), 0, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM};
    return protocols;
  }

  static const lws_protocols *combined_protocols() {
    static const lws_protocols protocols[] = {
        {kHttpProtocolName, &WebSocketTransport::Impl::http_callback,
         sizeof(HttpSessionState), 0, 0, nullptr, 0},
        {kProtocolName, &WebSocketTransport::Impl::ws_callback, 0, 4096, 0,
         nullptr, 0},
        LWS_PROTOCOL_LIST_TERM};
    return protocols;
  }

  bool should_share_http_with_ws() const {
    return config.http_enabled && config.http_port == config.ws_port &&
           config.http_host == config.ws_host;
  }

  bool create_context(lws_context *&target, int port, const char *iface,
                      const lws_protocols *protocols,
                      const char *failure_message) {
    lws_context_creation_info info{};
    info.port = port;
    info.iface = iface;
    info.protocols = protocols;
    info.user = owner;
    info.options = 0;

    target = lws_create_context(&info);
    if (target == nullptr) {
      healthy = false;
      error = failure_message;
      return false;
    }
    return true;
  }

  int handle_ws_callback(lws *wsi, lws_callback_reasons reason, void *user,
                         void *in, size_t len) {
    (void)user;

    switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
      auto uri = request_uri(wsi);
      auto topic =
          uri.has_value() ? topic_from_uri(*uri, ws_base_path) : std::nullopt;
      if (!topic.has_value()) {
        std::lock_guard<std::mutex> lock(mutex);
        error = "Invalid WebSocket path; expected " + ws_base_path + "/<topic>";
        return 1;
      }
      break;
    }
    case LWS_CALLBACK_ESTABLISHED: {
      auto uri = request_uri(wsi);
      auto topic =
          uri.has_value() ? topic_from_uri(*uri, ws_base_path) : std::nullopt;
      if (!topic.has_value()) {
        healthy = false;
        error = "Missing topic for established WebSocket session";
        return -1;
      }

      std::lock_guard<std::mutex> lock(mutex);
      sessions.try_emplace(wsi, SessionState{.topic = *topic});
      ++accepted_connections;
      client_count_changed = true;
    } break;
    case LWS_CALLBACK_CLOSED: {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.erase(wsi);
      ++closed_connections;
      client_count_changed = true;
    } break;
    case LWS_CALLBACK_RECEIVE: {
      std::lock_guard<std::mutex> lock(mutex);
      auto iter = sessions.find(wsi);
      if (iter != sessions.end()) {
        iter->second.rx_buffer.append(static_cast<const char *>(in), len);
        if (lws_is_final_fragment(wsi) &&
            lws_remaining_packet_payload(wsi) == 0) {
          try {
            inbound_messages.push_back(
                BridgeMessage{iter->second.topic,
                              nlohmann::json::parse(iter->second.rx_buffer)});
            ++inbound_frames;
            last_inbound_at = std::chrono::steady_clock::now();
          } catch (const std::exception &exc) {
            healthy = false;
            error = exc.what();
            return -1;
          }
          iter->second.rx_buffer.clear();
        }
      }
      break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      std::string payload;
      {
        std::lock_guard<std::mutex> lock(mutex);
        auto iter = sessions.find(wsi);
        if (iter == sessions.end() || iter->second.tx_queue.empty()) {
          break;
        }
        payload = iter->second.tx_queue.front();
      }

      std::vector<unsigned char> buffer(LWS_PRE + payload.size());
      std::copy(payload.begin(), payload.end(), buffer.begin() + LWS_PRE);
      auto written = lws_write(wsi, buffer.data() + LWS_PRE, payload.size(),
                               LWS_WRITE_TEXT);
      if (written < static_cast<int>(payload.size())) {
        std::lock_guard<std::mutex> lock(mutex);
        healthy = false;
        error = "Failed to write full WebSocket frame";
        return -1;
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        auto iter = sessions.find(wsi);
        if (iter != sessions.end() && !iter->second.tx_queue.empty()) {
          iter->second.tx_queue.pop_front();
          ++outbound_written;
          last_outbound_at = std::chrono::steady_clock::now();
          if (!iter->second.tx_queue.empty()) {
            lws_callback_on_writable(wsi);
          }
        }
      }
      break;
    }
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto &[client_wsi, state] : sessions) {
        if (!state.tx_queue.empty()) {
          lws_callback_on_writable(client_wsi);
        }
      }
      break;
    }
    default:
      break;
    }

    return 0;
  }

  int handle_http_callback(lws *wsi, lws_callback_reasons reason, void *user,
                           void *in, size_t len) {
    auto *state = static_cast<HttpSessionState *>(user);

    switch (reason) {
    case LWS_CALLBACK_HTTP: {
      if (!config.http_enabled) {
        return 1;
      }

      std::string uri;
      if (in != nullptr && len > 0) {
        uri.assign(static_cast<const char *>(in), len);
      } else {
        uri = request_uri(wsi).value_or("/");
      }

      if (uri == "/bootstrap.json" ||
          uri == http_base_path + "/bootstrap.json") {
        auto payload = websocket_bootstrap_payload(
            config, websocket_external_hosts(config));
        if (!payload.has_value()) {
          lws_return_http_status(wsi, HTTP_STATUS_SERVICE_UNAVAILABLE, nullptr);
          return -1;
        }

        state->owned_payload = *payload;
        state->dynamic_asset = EmbeddedWebAsset{
            .path = "/bootstrap.json",
            .content_type = "application/json; charset=utf-8",
            .data = reinterpret_cast<const unsigned char *>(
                state->owned_payload.data()),
            .size = state->owned_payload.size(),
            .spa_entry = false,
            .cache_forever = false,
        };
        state->asset = &state->dynamic_asset;
      } else {
        auto resolved_path = resolve_http_request_path(config, uri);
        if (resolved_path.empty()) {
          lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
          return -1;
        }

        state->asset = find_embedded_web_asset(resolved_path);
        if (state->asset == nullptr) {
          lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
          return -1;
        }
      }

      state->offset = 0;
      if (write_http_response_headers(wsi, *state->asset) != 0) {
        return 1;
      }
      lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_HTTP_WRITEABLE: {
      if (state == nullptr || state->asset == nullptr) {
        return -1;
      }

      auto remaining = state->asset->size - state->offset;
      if (remaining == 0) {
        auto completed = lws_http_transaction_completed(wsi);
        state->asset = nullptr;
        state->offset = 0;
        return completed != 0 ? -1 : 0;
      }

      auto chunk_size = std::min(remaining, kHttpWriteChunkSize);
      auto flags =
          remaining > chunk_size ? LWS_WRITE_HTTP : LWS_WRITE_HTTP_FINAL;
      auto written = lws_write(
          wsi, const_cast<unsigned char *>(state->asset->data + state->offset),
          chunk_size, static_cast<lws_write_protocol>(flags));
      if (written < 0 || static_cast<std::size_t>(written) != chunk_size) {
        healthy = false;
        error = "Failed to write HTTP response body";
        return -1;
      }

      state->offset += chunk_size;
      if (state->offset < state->asset->size) {
        lws_callback_on_writable(wsi);
        return 0;
      }

      auto completed = lws_http_transaction_completed(wsi);
      state->asset = nullptr;
      state->offset = 0;
      return completed != 0 ? -1 : 0;
    }
    case LWS_CALLBACK_CLOSED_HTTP:
      if (state != nullptr) {
        state->asset = nullptr;
        state->offset = 0;
        state->owned_payload.clear();
      }
      break;
    default:
      break;
    }

    return 0;
  }
};

WebSocketTransport::WebSocketTransport(BridgeConfig config)
    : _impl(std::make_unique<Impl>(std::move(config))) {
  _impl->owner = this;
}

WebSocketTransport::~WebSocketTransport() { stop(); }

bool WebSocketTransport::start() {
  if (_impl == nullptr) {
    return false;
  }

  if (_impl->config.http_enabled && !has_embedded_web_assets()) {
    _impl->healthy = false;
    _impl->error =
        "HTTP UI is enabled but no embedded web assets are available";
    return false;
  }

  lws_set_log_level(
      _impl->config.ws_silent ? 0 : (LLL_ERR | LLL_WARN | LLL_NOTICE), nullptr);

  _impl->http_shared_with_ws = _impl->should_share_http_with_ws();
  if (_impl->http_shared_with_ws) {
    if (!_impl->create_context(
            _impl->ws_context, _impl->config.ws_port,
            bind_iface(_impl->config.ws_host),
            WebSocketTransport::Impl::combined_protocols(),
            "Failed to create combined HTTP/WebSocket context")) {
      return false;
    }
  } else {
    if (!_impl->create_context(_impl->ws_context, _impl->config.ws_port,
                               bind_iface(_impl->config.ws_host),
                               WebSocketTransport::Impl::ws_protocols(),
                               "Failed to create WebSocket context")) {
      return false;
    }

    if (_impl->config.http_enabled) {
      if (!_impl->create_context(_impl->http_context, _impl->config.http_port,
                                 bind_iface(_impl->config.http_host),
                                 WebSocketTransport::Impl::http_protocols(),
                                 "Failed to create HTTP context")) {
        if (_impl->ws_context != nullptr) {
          lws_context_destroy(_impl->ws_context);
          _impl->ws_context = nullptr;
        }
        return false;
      }
    }
  }

  _impl->started = true;
  _impl->stop_requested.store(false);
  _impl->service_thread = std::thread([impl = _impl.get()]() {
    while (!impl->stop_requested.load()) {
      if (impl->ws_context != nullptr) {
        lws_service(impl->ws_context, 0);
      }
      if (impl->http_context != nullptr) {
        lws_service(impl->http_context, 0);
      }
    }
  });
  return true;
}

void WebSocketTransport::poll() { return; }

bool WebSocketTransport::broadcast(const BridgeMessage &message) {
  if (_impl == nullptr || !_impl->started || !_impl->healthy) {
    return false;
  }

  auto payload = json_dump(message.message);

  {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    for (auto &[wsi, state] : _impl->sessions) {
      if (state.topic != kAllTopics && state.topic != message.topic) {
        continue;
      }
      (void)wsi;
      state.tx_queue.push_back(payload);
      ++_impl->outbound_enqueued;
    }
  }
  if (_impl->ws_context != nullptr) {
    lws_cancel_service(_impl->ws_context);
  }

  return true;
}

std::optional<BridgeMessage> WebSocketTransport::pop_incoming_message() {
  if (_impl == nullptr) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(_impl->mutex);
  if (_impl->inbound_messages.empty()) {
    return std::nullopt;
  }
  auto message = _impl->inbound_messages.front();
  _impl->inbound_messages.pop_front();
  return message;
}

void WebSocketTransport::stop() {
  if (_impl == nullptr ||
      (_impl->ws_context == nullptr && _impl->http_context == nullptr)) {
    return;
  }
  _impl->stop_requested.store(true);
  if (_impl->ws_context != nullptr) {
    lws_cancel_service(_impl->ws_context);
  }
  if (_impl->http_context != nullptr) {
    lws_cancel_service(_impl->http_context);
  }
  if (_impl->service_thread.joinable()) {
    _impl->service_thread.join();
  }
  if (_impl->http_context != nullptr) {
    lws_context_destroy(_impl->http_context);
    _impl->http_context = nullptr;
  }
  if (_impl->ws_context != nullptr) {
    lws_context_destroy(_impl->ws_context);
    _impl->ws_context = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->sessions.clear();
    _impl->inbound_messages.clear();
  }
  _impl->started = false;
}

bool WebSocketTransport::healthy() const {
  return _impl != nullptr && _impl->healthy;
}

std::size_t WebSocketTransport::connected_clients() const {
  if (_impl == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(_impl->mutex);
  return _impl->sessions.size();
}

bool WebSocketTransport::consume_client_count_changed(std::size_t &count) {
  if (_impl == nullptr) {
    count = 0;
    return false;
  }
  std::lock_guard<std::mutex> lock(_impl->mutex);
  count = _impl->sessions.size();
  auto changed = _impl->client_count_changed;
  _impl->client_count_changed = false;
  return changed;
}

std::string WebSocketTransport::error() const {
  return _impl == nullptr ? "WebSocket transport is not available"
                          : _impl->error;
}

std::string WebSocketTransport::stats() const {
  if (_impl == nullptr) {
    return "unavailable";
  }

  std::size_t pending_frames = 0;
  std::lock_guard<std::mutex> lock(_impl->mutex);
  for (const auto &[wsi, state] : _impl->sessions) {
    (void)wsi;
    pending_frames += state.tx_queue.size();
  }

  std::ostringstream stream;
  stream << "clients=" << _impl->sessions.size()
         << " accepted=" << _impl->accepted_connections
         << " closed=" << _impl->closed_connections
         << " rx_frames=" << _impl->inbound_frames
         << " tx_enqueued=" << _impl->outbound_enqueued
         << " tx_written=" << _impl->outbound_written
         << " last_rx_age=" << age_string(_impl->last_inbound_at)
         << " last_tx_age=" << age_string(_impl->last_outbound_at)
         << " pending=" << pending_frames
         << " healthy=" << (_impl->healthy ? "true" : "false");
  return stream.str();
}

BridgeRuntime::BridgeRuntime(std::string agent_name, std::string settings_uri,
                             bool webserver_enabled)
    : _agent_name(std::move(agent_name)),
      _settings_uri(std::move(settings_uri)),
      _webserver_enabled(webserver_enabled) {}

BridgeRuntime::BridgeRuntime(std::unique_ptr<IMadsTransport> mads_transport,
                             std::unique_ptr<IWebSocketTransport> ws_transport,
                             BridgeConfig config)
    : _config(std::move(config)), _mads_transport(std::move(mads_transport)),
      _ws_transport(std::move(ws_transport)), _manual_components(true) {}

bool BridgeRuntime::initialize() {
  if (_initialized) {
    return true;
  }

  try {
    if (!_manual_components) {
      auto agent = std::make_unique<Mads::Agent>(_agent_name, _settings_uri);
      agent->init(false, false);
      auto settings = agent->get_settings();
      _config = BridgeConfig::from_settings(settings);
      _config.http_enabled = _config.http_enabled && _webserver_enabled;
      if (settings.contains("sub_topic") && settings["sub_topic"].is_array()) {
        agent->set_sub_topic(
            settings["sub_topic"].get<std::vector<std::string>>());
      }
      if (settings.contains("pub_topic") && settings["pub_topic"].is_string()) {
        agent->set_pub_topic(settings["pub_topic"].get<std::string>());
      }
      agent->connect();
      _mads_transport =
          std::make_unique<MadsTransport>(std::move(agent), _config);
      _ws_transport = std::make_unique<WebSocketTransport>(_config);
    }

    if (_mads_transport == nullptr || _ws_transport == nullptr) {
      _error = "Runtime transports are not configured";
      return false;
    }

    if (!_mads_transport->start()) {
      _error = _mads_transport->error();
      return false;
    }
    if (!_ws_transport->start()) {
      _error = _ws_transport->error();
      _mads_transport->stop();
      return false;
    }

    _bridge_core =
        std::make_unique<BridgeCore>(*_mads_transport, *_ws_transport);
    _initialized = true;
    return true;
  } catch (const std::exception &exc) {
    _error = exc.what();
    return false;
  }
}

RuntimeDecision BridgeRuntime::idle() {
  if (!_initialized) {
    return RuntimeDecision::stop;
  }
  if (!_mads_transport->healthy() || !_ws_transport->healthy()) {
    _error = !_mads_transport->healthy() ? _mads_transport->error()
                                         : _ws_transport->error();
    return RuntimeDecision::stop;
  }
  if (_mads_transport->should_stop()) {
    return RuntimeDecision::stop;
  }
  return RuntimeDecision::run;
}

RuntimeDecision BridgeRuntime::run_once() {
  if (!_initialized || _bridge_core == nullptr) {
    return RuntimeDecision::stop;
  }
  if (!_bridge_core->tick()) {
    _error = _bridge_core->error();
    return RuntimeDecision::stop;
  }
  if (_bridge_core->should_stop()) {
    return RuntimeDecision::stop;
  }
  return RuntimeDecision::run;
}

void BridgeRuntime::shutdown() {
  if (_shutdown) {
    return;
  }
  if (_bridge_core != nullptr) {
    _bridge_core->shutdown();
  } else {
    if (_ws_transport != nullptr) {
      _ws_transport->stop();
    }
    if (_mads_transport != nullptr) {
      _mads_transport->stop();
    }
  }
  _shutdown = true;
}

void BridgeRuntime::reset() { _error.clear(); }

bool BridgeRuntime::restart_requested() const {
  return _mads_transport != nullptr && _mads_transport->restart_requested();
}

bool BridgeRuntime::ready() const { return _initialized; }

const BridgeConfig &BridgeRuntime::config() const { return _config; }

std::string BridgeRuntime::agent_name() const { return _agent_name; }

std::string BridgeRuntime::error() const { return _error; }

std::vector<std::string> BridgeRuntime::websocket_external_hosts() const {
  return ::MadsWebsockets::websocket_external_hosts(_config);
}

std::vector<std::string> BridgeRuntime::websocket_external_addresses() const {
  return websocket_external_uris(_config);
}

std::vector<std::string> BridgeRuntime::http_external_hosts() const {
  return ::MadsWebsockets::http_external_hosts(_config);
}

std::vector<std::string> BridgeRuntime::http_external_addresses() const {
  return http_external_uris(_config);
}

std::optional<std::string> BridgeRuntime::websocket_bootstrap_payload() const {
  return websocket_bootstrap_payload(websocket_external_hosts());
}

std::optional<std::string> BridgeRuntime::websocket_bootstrap_payload(
    const std::vector<std::string> &hosts) const {
  return ::MadsWebsockets::websocket_bootstrap_payload(_config, hosts);
}

std::size_t BridgeRuntime::connected_clients() const {
  return _ws_transport != nullptr ? _ws_transport->connected_clients() : 0;
}

bool BridgeRuntime::consume_client_count_changed(std::size_t &count) {
  if (_ws_transport == nullptr) {
    count = 0;
    return false;
  }
  return _ws_transport->consume_client_count_changed(count);
}

} // namespace MadsWebsockets
