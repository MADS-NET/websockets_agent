#ifndef MADS_WEBSOCKETS_BRIDGE_HPP
#define MADS_WEBSOCKETS_BRIDGE_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <agent.hpp>

namespace MadsWebsockets {

struct BridgeMessage {
  std::string topic;
  nlohmann::json message;
};

struct BridgeConfig {
  std::string ws_host = "0.0.0.0";
  int ws_port = 9002;
  std::string ws_path = "/";
  std::chrono::milliseconds period{100};
  std::chrono::milliseconds receive_timeout{50};
  bool non_blocking = false;

  static BridgeConfig from_settings(const nlohmann::json &settings);
};

class IMadsTransport {
public:
  virtual ~IMadsTransport() = default;

  virtual bool start() = 0;
  virtual void poll() = 0;
  virtual bool publish(const BridgeMessage &message) = 0;
  virtual std::optional<BridgeMessage> pop_incoming() = 0;
  virtual void stop() = 0;
  virtual bool healthy() const = 0;
  virtual bool should_stop() const = 0;
  virtual bool restart_requested() const = 0;
  virtual std::string error() const = 0;
};

class IWebSocketTransport {
public:
  virtual ~IWebSocketTransport() = default;

  virtual bool start() = 0;
  virtual void poll() = 0;
  virtual bool broadcast(const BridgeMessage &message) = 0;
  virtual std::optional<std::string> pop_incoming_payload() = 0;
  virtual void stop() = 0;
  virtual bool healthy() const = 0;
  virtual std::string error() const = 0;
};

class BridgeCore {
public:
  BridgeCore(IMadsTransport &mads_transport, IWebSocketTransport &ws_transport);

  bool tick();
  void shutdown();

  bool healthy() const;
  bool should_stop() const;
  std::string error() const;

  static std::optional<BridgeMessage> parse_client_payload(
    std::string_view payload,
    std::string &error
  );

private:
  IMadsTransport &_mads_transport;
  IWebSocketTransport &_ws_transport;
  bool _healthy = true;
  bool _should_stop = false;
  std::string _error;
};

enum class RuntimeDecision {
  stay,
  idle,
  run,
  stop,
};

class MadsTransport final : public IMadsTransport {
public:
  MadsTransport(std::unique_ptr<Mads::Agent> agent, BridgeConfig config);

  bool start() override;
  void poll() override;
  bool publish(const BridgeMessage &message) override;
  std::optional<BridgeMessage> pop_incoming() override;
  void stop() override;
  bool healthy() const override;
  bool should_stop() const override;
  bool restart_requested() const override;
  std::string error() const override;

private:
  std::unique_ptr<Mads::Agent> _agent;
  BridgeConfig _config;
  std::optional<BridgeMessage> _incoming;
  bool _started = false;
  bool _healthy = true;
  bool _should_stop = false;
  bool _restart_requested = false;
  std::string _error;
};

class WebSocketTransport final : public IWebSocketTransport {
public:
  explicit WebSocketTransport(BridgeConfig config);
  ~WebSocketTransport() override;

  bool start() override;
  void poll() override;
  bool broadcast(const BridgeMessage &message) override;
  std::optional<std::string> pop_incoming_payload() override;
  void stop() override;
  bool healthy() const override;
  std::string error() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

class BridgeRuntime {
public:
  BridgeRuntime(std::string agent_name, std::string settings_uri);
  BridgeRuntime(
    std::unique_ptr<IMadsTransport> mads_transport,
    std::unique_ptr<IWebSocketTransport> ws_transport,
    BridgeConfig config = {}
  );

  bool initialize();
  RuntimeDecision idle();
  RuntimeDecision run_once();
  void shutdown();
  void reset();

  bool restart_requested() const;
  bool ready() const;
  const BridgeConfig &config() const;
  std::string error() const;

private:
  std::string _agent_name;
  std::string _settings_uri;
  BridgeConfig _config;
  std::unique_ptr<IMadsTransport> _mads_transport;
  std::unique_ptr<IWebSocketTransport> _ws_transport;
  std::unique_ptr<BridgeCore> _bridge_core;
  bool _initialized = false;
  bool _manual_components = false;
  bool _shutdown = false;
  std::string _error;
};

} // namespace MadsWebsockets

#endif
