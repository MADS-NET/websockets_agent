#include "banner.hpp"
#include "bridge.hpp"
#include "fsm_agent.hpp"

#include <agent.hpp>
#include <cxxopts.hpp>
#include <mads.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace std;
using namespace cxxopts;
using namespace Mads;

struct FsmData {
  MadsWebsockets::BannerController banner;
  MadsWebsockets::BridgeRuntime runtime;

  explicit FsmData(MadsWebsockets::BridgeRuntime runtime_in)
      : runtime(std::move(runtime_in)) {}
};

int main(int argc, char *argv[]) {
  std::filesystem::path exec = argv[0];
  std::string agent_name = exec.stem().string();
  std::string settings_uri = SETTINGS_URI;

  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("n,name", "Agent name override", value<string>())
    ("w,webserver", "Also start the embedded webserver for hosting the web UI");
  SETUP_OPTIONS(options, Agent);
  // clang-format on

  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  }

  if (options_parsed.count("crypto") != 0 ||
      options_parsed.count("keys_dir") != 0 ||
      options_parsed.count("key_broker") != 0 ||
      options_parsed.count("key_client") != 0 ||
      options_parsed.count("auth_verbose") != 0) {
    cerr << fg::yellow
         << "Warning: CLI crypto options are accepted for interface "
            "compatibility "
            "but are not implemented by mads-websockets yet."
         << fg::reset << endl;
  }

  if (argc > 1) {
    if (options_parsed.unmatched().size() > 1) {
      cerr << fg::red << "Unexpected positional arguments" << fg::reset << endl;
      cerr << options.help() << endl;
      return EXIT_FAILURE;
    }
    if (!options_parsed.unmatched().empty()) {
      settings_uri = options_parsed.unmatched().front();
    }
  }

  auto webserver_enabled = options_parsed.count("webserver") != 0;

  FsmData data{MadsWebsockets::BridgeRuntime(agent_name, settings_uri,
                                             webserver_enabled)};

  auto fsm = FSM::FiniteStateMachine(&data);
  fsm.set_timing_function(
      [&]() { std::this_thread::sleep_for(data.runtime.config().period); });

  try {
    fsm.run();
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << std::endl;
    data.banner.shutdown();
    data.runtime.shutdown();
    return EXIT_FAILURE;
  }

  data.banner.shutdown();
  return data.runtime.restart_requested() ? EXIT_SUCCESS + 1 : EXIT_SUCCESS;
}
