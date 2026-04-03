#include "bridge.hpp"

#include <deque>
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libwebsockets.h>

namespace MadsWebsockets {

namespace {

constexpr char kControlTopic[] = "control";
constexpr char kProtocolName[] = "mads-websockets";
constexpr char kDefaultWsBasePath[] = "/mads";
constexpr char kAllTopics[] = "_all";

std::string json_dump(const nlohmann::json &value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string age_string(std::chrono::steady_clock::time_point when) {
  if (when == std::chrono::steady_clock::time_point{}) {
    return "-";
  }
  auto age =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - when
    );
  std::ostringstream stream;
  stream << age.count() << "ms";
  return stream.str();
}

std::string websocket_bind_uri(const BridgeConfig &config) {
  return "ws://" + config.ws_host + ":" + std::to_string(config.ws_port) +
         config.ws_path;
}

std::string normalize_ws_base_path(std::string path) {
  if (path.empty()) {
    return kDefaultWsBasePath;
  }
  if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::optional<std::string> topic_from_uri(std::string_view uri, std::string_view base_path) {
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
  auto copied = lws_hdr_copy(
    wsi,
    uri.data(),
    static_cast<int>(uri.size()) + 1,
    WSI_TOKEN_GET_URI
  );
  if (copied <= 0) {
    return std::nullopt;
  }

  uri.resize(static_cast<std::size_t>(copied));
  return uri;
}

const char *websocket_iface(const BridgeConfig &config) {
  if (config.ws_host.empty() || config.ws_host == "0.0.0.0" ||
      config.ws_host == "::") {
    return nullptr;
  }
  return config.ws_host.c_str();
}

struct SessionState {
  std::string topic;
  std::string rx_buffer;
  std::deque<std::string> tx_queue;
};

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

BridgeCore::BridgeCore(IMadsTransport &mads_transport, IWebSocketTransport &ws_transport)
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

MadsTransport::MadsTransport(std::unique_ptr<Mads::Agent> agent, BridgeConfig config)
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
        _incoming_queue.push_back(BridgeMessage{
          topic,
          nlohmann::json::parse(payload_str)
        });
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
         << " queued=" << _incoming_queue.size()
         << " last_rx_topic=" << (_last_received_topic.empty() ? "-" : _last_received_topic)
         << " last_tx_topic=" << (_last_published_topic.empty() ? "-" : _last_published_topic)
         << " last_rx_age=" << age_string(_last_received_at)
         << " last_tx_age=" << age_string(_last_published_at)
         << " healthy=" << (_healthy ? "true" : "false");
  return stream.str();
}

struct WebSocketTransport::Impl {
  explicit Impl(BridgeConfig bridge_config)
    : config(std::move(bridge_config)),
      base_path(normalize_ws_base_path(config.ws_path)) {}

  BridgeConfig config;
  std::string base_path;
  lws_context *context = nullptr;
  mutable std::mutex mutex;
  std::unordered_map<lws *, SessionState> sessions;
  std::deque<BridgeMessage> inbound_messages;
  bool started = false;
  bool healthy = true;
  std::string error;
  std::size_t accepted_connections = 0;
  std::size_t closed_connections = 0;
  std::size_t inbound_frames = 0;
  std::size_t outbound_enqueued = 0;
  std::size_t outbound_written = 0;
  std::chrono::steady_clock::time_point last_inbound_at{};
  std::chrono::steady_clock::time_point last_outbound_at{};
  std::atomic<bool> stop_requested{false};
  std::thread service_thread;

  static int callback(
    lws *wsi,
    lws_callback_reasons reason,
    void *user,
    void *in,
    size_t len
  ) {
    auto *context = lws_get_context(wsi);
    auto *self =
      static_cast<WebSocketTransport *>(lws_context_user(context));
    if (self == nullptr || self->_impl == nullptr) {
      return 0;
    }
    return self->_impl->handle_callback(wsi, reason, user, in, len);
  }

  int handle_callback(
    lws *wsi,
    lws_callback_reasons reason,
    void *user,
    void *in,
    size_t len
  ) {
    (void)user;

    switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
      auto uri = request_uri(wsi);
      auto topic = uri.has_value() ? topic_from_uri(*uri, base_path) : std::nullopt;
      if (!topic.has_value()) {
        std::lock_guard<std::mutex> lock(mutex);
        error = "Invalid WebSocket path; expected " + base_path + "/<topic>";
        return 1;
      }
      break;
    }
    case LWS_CALLBACK_ESTABLISHED:
      {
        auto uri = request_uri(wsi);
        auto topic = uri.has_value() ? topic_from_uri(*uri, base_path) : std::nullopt;
        if (!topic.has_value()) {
          healthy = false;
          error = "Missing topic for established WebSocket session";
          return -1;
        }

        std::lock_guard<std::mutex> lock(mutex);
        sessions.try_emplace(wsi, SessionState{.topic = *topic});
        ++accepted_connections;
      }
      break;
    case LWS_CALLBACK_CLOSED:
      {
        std::lock_guard<std::mutex> lock(mutex);
        sessions.erase(wsi);
        ++closed_connections;
      }
      break;
    case LWS_CALLBACK_RECEIVE: {
      std::lock_guard<std::mutex> lock(mutex);
      auto iter = sessions.find(wsi);
      if (iter != sessions.end()) {
        iter->second.rx_buffer.append(static_cast<const char *>(in), len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
          try {
            inbound_messages.push_back(BridgeMessage{
              iter->second.topic,
              nlohmann::json::parse(iter->second.rx_buffer)
            });
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
      auto written = lws_write(
        wsi,
        buffer.data() + LWS_PRE,
        payload.size(),
        LWS_WRITE_TEXT
      );
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
};

WebSocketTransport::WebSocketTransport(BridgeConfig config)
  : _impl(std::make_unique<Impl>(std::move(config))) {}

WebSocketTransport::~WebSocketTransport() { stop(); }

bool WebSocketTransport::start() {
  if (_impl == nullptr) {
    return false;
  }

  lws_set_log_level(
    _impl->config.ws_silent ? 0 : (LLL_ERR | LLL_WARN | LLL_NOTICE),
    nullptr
  );

  lws_context_creation_info info{};
  static const lws_protocols protocols[] = {
    {
      kProtocolName,
      &WebSocketTransport::Impl::callback,
      0,
      4096,
      0,
      nullptr,
      0
    },
    LWS_PROTOCOL_LIST_TERM
  };

  info.port = _impl->config.ws_port;
  info.iface = websocket_iface(_impl->config);
  info.protocols = protocols;
  info.user = this;
  info.options = 0;

  _impl->context = lws_create_context(&info);
  if (_impl->context == nullptr) {
    _impl->healthy = false;
    _impl->error = "Failed to create libwebsockets context";
    return false;
  }

  _impl->started = true;
  _impl->stop_requested.store(false);
  _impl->service_thread = std::thread([impl = _impl.get()]() {
    while (!impl->stop_requested.load()) {
      lws_service(impl->context, 0);
    }
  });
  return true;
}

void WebSocketTransport::poll() {
  return;
}

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
  lws_cancel_service(_impl->context);

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
  if (_impl == nullptr || _impl->context == nullptr) {
    return;
  }
  _impl->stop_requested.store(true);
  lws_cancel_service(_impl->context);
  if (_impl->service_thread.joinable()) {
    _impl->service_thread.join();
  }
  lws_context_destroy(_impl->context);
  _impl->context = nullptr;
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

std::string WebSocketTransport::error() const {
  return _impl == nullptr ? "WebSocket transport is not available" : _impl->error;
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

BridgeRuntime::BridgeRuntime(std::string agent_name, std::string settings_uri)
  : _agent_name(std::move(agent_name)), _settings_uri(std::move(settings_uri)) {}

BridgeRuntime::BridgeRuntime(
  std::unique_ptr<IMadsTransport> mads_transport,
  std::unique_ptr<IWebSocketTransport> ws_transport,
  BridgeConfig config
) : _config(std::move(config)),
    _mads_transport(std::move(mads_transport)),
    _ws_transport(std::move(ws_transport)),
    _manual_components(true) {}

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
      if (settings.contains("sub_topic") && settings["sub_topic"].is_array()) {
        agent->set_sub_topic(settings["sub_topic"].get<std::vector<std::string>>());
      }
      if (settings.contains("pub_topic") && settings["pub_topic"].is_string()) {
        agent->set_pub_topic(settings["pub_topic"].get<std::string>());
      }
      agent->connect();
      _mads_transport = std::make_unique<MadsTransport>(std::move(agent), _config);
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
    std::cerr << "mads-websockets: agent=" << _agent_name
              << " settings=" << _settings_uri
              << " websocket_bind=" << websocket_bind_uri(_config)
              << std::endl;
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
    _error = !_mads_transport->healthy() ? _mads_transport->error() : _ws_transport->error();
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

std::string BridgeRuntime::error() const { return _error; }

} // namespace MadsWebsockets
