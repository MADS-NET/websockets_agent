#include "bridge.hpp"

#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using MadsWebsockets::BridgeConfig;
using MadsWebsockets::BridgeCore;
using MadsWebsockets::BridgeMessage;
using MadsWebsockets::BridgeRuntime;
using MadsWebsockets::IMadsTransport;
using MadsWebsockets::IWebSocketTransport;
using MadsWebsockets::RuntimeDecision;

namespace {

class FakeMadsTransport final : public IMadsTransport {
public:
  bool start() override {
    started = true;
    return start_ok;
  }

  void poll() override {
    ++poll_count;
  }

  bool publish(const BridgeMessage &message) override {
    published.push_back(message);
    return publish_ok;
  }

  std::optional<BridgeMessage> pop_incoming() override {
    if (incoming.empty()) {
      return std::nullopt;
    }
    auto value = incoming.front();
    incoming.pop_front();
    return value;
  }

  void stop() override { stopped = true; }
  bool healthy() const override { return transport_healthy; }
  bool should_stop() const override { return stop_requested; }
  bool restart_requested() const override { return restart; }
  std::string error() const override { return error_message; }
  std::string stats() const override { return "fake-mads"; }

  bool start_ok = true;
  bool publish_ok = true;
  bool transport_healthy = true;
  bool stop_requested = false;
  bool restart = false;
  bool started = false;
  bool stopped = false;
  int poll_count = 0;
  std::string error_message;
  std::deque<BridgeMessage> incoming;
  std::deque<BridgeMessage> published;
};

class FakeWebSocketTransport final : public IWebSocketTransport {
public:
  bool start() override {
    started = true;
    return start_ok;
  }

  void poll() override {
    ++poll_count;
  }

  bool broadcast(const BridgeMessage &message) override {
    broadcasted.push_back(message);
    return broadcast_ok;
  }

  std::optional<BridgeMessage> pop_incoming_message() override {
    if (incoming.empty()) {
      return std::nullopt;
    }
    auto value = incoming.front();
    incoming.pop_front();
    return value;
  }

  void stop() override { stopped = true; }
  bool healthy() const override { return transport_healthy; }
  std::size_t connected_clients() const override { return client_count; }
  bool consume_client_count_changed(std::size_t &count) override {
    count = client_count;
    auto changed = client_count_changed;
    client_count_changed = false;
    return changed;
  }
  std::string error() const override { return error_message; }
  std::string stats() const override { return "fake-ws"; }

  bool start_ok = true;
  bool broadcast_ok = true;
  bool transport_healthy = true;
  bool started = false;
  bool stopped = false;
  int poll_count = 0;
  std::size_t client_count = 0;
  bool client_count_changed = false;
  std::string error_message;
  std::deque<BridgeMessage> incoming;
  std::deque<BridgeMessage> broadcasted;
};

void test_bridge_core_routes_messages() {
  auto mads = std::make_unique<FakeMadsTransport>();
  auto ws = std::make_unique<FakeWebSocketTransport>();

  ws->incoming.push_back({"from_ws", {{"x", 1}}});
  mads->incoming.push_back({"from_mads", {{"y", 2}}});

  auto *mads_ptr = mads.get();
  auto *ws_ptr = ws.get();

  BridgeRuntime runtime(std::move(mads), std::move(ws), BridgeConfig{});
  assert(runtime.initialize());
  assert(runtime.idle() == RuntimeDecision::run);
  assert(runtime.run_once() == RuntimeDecision::run);

  assert(mads_ptr->published.size() == 1);
  assert(mads_ptr->published.front().topic == "from_ws");
  assert(ws_ptr->broadcasted.size() == 1);
  assert(ws_ptr->broadcasted.front().topic == "from_mads");

  runtime.shutdown();
  assert(mads_ptr->stopped);
  assert(ws_ptr->stopped);
}

void test_runtime_stops_on_transport_request() {
  auto mads = std::make_unique<FakeMadsTransport>();
  auto ws = std::make_unique<FakeWebSocketTransport>();

  mads->stop_requested = true;

  BridgeRuntime runtime(std::move(mads), std::move(ws), BridgeConfig{});
  assert(runtime.initialize());
  assert(runtime.idle() == RuntimeDecision::stop);
}

} // namespace

int main() {
  test_bridge_core_routes_messages();
  test_runtime_stops_on_transport_request();
  std::cout << "All tests passed\n";
  return 0;
}
