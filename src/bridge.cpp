#include "bridge.hpp"

#include <deque>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libwebsockets.h>

namespace MadsWebsockets {

namespace {

constexpr char kControlTopic[] = "control";
constexpr char kProtocolName[] = "mads-websockets";

std::string json_dump(const nlohmann::json &value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string websocket_bind_uri(const BridgeConfig &config) {
  return "ws://" + config.ws_host + ":" + std::to_string(config.ws_port) +
         config.ws_path;
}

const char *websocket_iface(const BridgeConfig &config) {
  if (config.ws_host.empty() || config.ws_host == "0.0.0.0" ||
      config.ws_host == "::") {
    return nullptr;
  }
  return config.ws_host.c_str();
}

struct SessionState {
  std::string rx_buffer;
  std::deque<std::string> tx_queue;
  bool close_after_write = false;
  bool writable_requested = false;
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

  return config;
}

BridgeCore::BridgeCore(IMadsTransport &mads_transport, IWebSocketTransport &ws_transport)
  : _mads_transport(mads_transport), _ws_transport(ws_transport) {}

bool BridgeCore::tick() {
  _mads_transport.poll();
  _ws_transport.poll();

  while (true) {
    auto payload = _ws_transport.pop_incoming_payload();
    if (!payload.has_value()) {
      break;
    }

    std::string parse_error;
    auto message = parse_client_payload(*payload, parse_error);
    if (!message.has_value()) {
      continue;
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

std::optional<BridgeMessage> BridgeCore::parse_client_payload(
  std::string_view payload,
  std::string &error
) {
  try {
    auto json = nlohmann::json::parse(payload);
    if (!json.is_object()) {
      error = "WebSocket payload must be a JSON object";
      return std::nullopt;
    }
    if (!json.contains("topic") || !json["topic"].is_string()) {
      error = "WebSocket payload must contain a string topic";
      return std::nullopt;
    }
    if (!json.contains("message")) {
      error = "WebSocket payload must contain a message field";
      return std::nullopt;
    }

    return BridgeMessage{
      json["topic"].get<std::string>(),
      json["message"]
    };
  } catch (const std::exception &exc) {
    error = exc.what();
    return std::nullopt;
  }
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
    auto type = _agent->receive(_config.non_blocking);
    if (type != Mads::message_type::json) {
      if (!Mads::running.load()) {
        _should_stop = true;
      }
      return;
    }

    auto [topic, payload_str] = _agent->last_message();

    if (topic != kControlTopic) {
      _incoming = BridgeMessage{
        topic,
        nlohmann::json::parse(payload_str)
      };
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
    return true;
  } catch (const std::exception &exc) {
    _healthy = false;
    _error = exc.what();
    return false;
  }
}

std::optional<BridgeMessage> MadsTransport::pop_incoming() {
  auto incoming = _incoming;
  _incoming.reset();
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

struct WebSocketTransport::Impl {
  explicit Impl(BridgeConfig bridge_config) : config(std::move(bridge_config)) {}

  BridgeConfig config;
  lws_context *context = nullptr;
  std::unordered_map<lws *, SessionState> sessions;
  std::deque<std::string> inbound_payloads;
  bool started = false;
  bool healthy = true;
  std::string error;

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
    case LWS_CALLBACK_ESTABLISHED:
      sessions.try_emplace(wsi);
      break;
    case LWS_CALLBACK_CLOSED:
      sessions.erase(wsi);
      break;
    case LWS_CALLBACK_RECEIVE: {
      auto iter = sessions.find(wsi);
      if (iter == sessions.end()) {
        break;
      }
      iter->second.rx_buffer.append(static_cast<const char *>(in), len);
      if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
        inbound_payloads.push_back(iter->second.rx_buffer);
        iter->second.rx_buffer.clear();
      }
      break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      auto iter = sessions.find(wsi);
      if (iter == sessions.end() || iter->second.tx_queue.empty()) {
        break;
      }

      auto payload = iter->second.tx_queue.front();
      std::vector<unsigned char> buffer(LWS_PRE + payload.size());
      std::copy(payload.begin(), payload.end(), buffer.begin() + LWS_PRE);
      auto written = lws_write(
        wsi,
        buffer.data() + LWS_PRE,
        payload.size(),
        LWS_WRITE_TEXT
      );
      if (written < static_cast<int>(payload.size())) {
        healthy = false;
        error = "Failed to write full WebSocket frame";
        return -1;
      }

      iter->second.tx_queue.pop_front();
      if (!iter->second.tx_queue.empty()) {
        lws_callback_on_writable(wsi);
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
  return true;
}

void WebSocketTransport::poll() {
  if (_impl == nullptr || !_impl->started || !_impl->healthy) {
    return;
  }
  lws_service(_impl->context, 0);
}

bool WebSocketTransport::broadcast(const BridgeMessage &message) {
  if (_impl == nullptr || !_impl->started || !_impl->healthy) {
    return false;
  }

  nlohmann::json envelope{
    {"topic", message.topic},
    {"message", message.message}
  };
  auto payload = json_dump(envelope);

  for (auto &[wsi, state] : _impl->sessions) {
    state.tx_queue.push_back(payload);
    lws_callback_on_writable(wsi);
  }

  return true;
}

std::optional<std::string> WebSocketTransport::pop_incoming_payload() {
  if (_impl == nullptr || _impl->inbound_payloads.empty()) {
    return std::nullopt;
  }

  auto payload = _impl->inbound_payloads.front();
  _impl->inbound_payloads.pop_front();
  return payload;
}

void WebSocketTransport::stop() {
  if (_impl == nullptr || _impl->context == nullptr) {
    return;
  }
  lws_context_destroy(_impl->context);
  _impl->context = nullptr;
  _impl->sessions.clear();
  _impl->started = false;
}

bool WebSocketTransport::healthy() const {
  return _impl != nullptr && _impl->healthy;
}

std::string WebSocketTransport::error() const {
  return _impl == nullptr ? "WebSocket transport is not available" : _impl->error;
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
      agent->connect();
      _config = BridgeConfig::from_settings(agent->get_settings());
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
