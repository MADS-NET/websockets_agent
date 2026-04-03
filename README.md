# MADS WebSockets Bridge

`mads-websockets` is a MADS agent that exposes MADS topics over WebSockets.
It subscribes to the MADS broker, forwards matching MADS JSON messages to
WebSocket clients, and accepts JSON published by WebSocket clients back into
the MADS network.

The bridge is implemented as a small layered runtime:

- `MadsTransport` handles broker-side I/O through `Mads::Agent`
- `WebSocketTransport` handles client-side I/O through `libwebsockets`
- `BridgeCore` routes validated messages between the two transports
- the FSM in [`src/fsm_agent_impl.hpp`](src/fsm_agent_impl.hpp) manages agent
  lifecycle

## Protocol

The WebSocket API is topic-oriented.

- A client connects to `ws://<host>:<port>/mads/<topic>`
- The WebSocket path selects the MADS topic
- The WebSocket payload is raw JSON, without any envelope

Examples:

- listen to one topic:
  `ws://localhost:8080/mads/perf_assess`
- listen to all topics:
  `ws://localhost:8080/mads/_all`
- publish JSON to one topic:
  connect to `ws://localhost:8080/mads/mytopic` and send the JSON payload

`_all` is a wildcard subscription for receiving data. A connection on
`/mads/_all` receives all MADS topics forwarded by the bridge.

## Requirements

The project expects these libraries to be available on the system:

- Mads
- OpenSSL
- libwebsockets
- readline
- nlohmann/json
- cxxopts

The current CMake configuration finds Mads with `find_package(Mads CONFIG)` and
finds `libwebsockets` through `pkg-config`.

## Build

Configure and build with CMake and Ninja:

```bash
cmake -B build -G Ninja
cmake --build build
```

This produces two executables:

- `build/src/mads-websockets`
- `build/src/ws_client`

## Test

Run the unit test target with:

```bash
ctest --test-dir build --output-on-failure
```

## Configuration

The bridge reads its settings from the MADS broker or from a local `mads.ini`
file, depending on how the agent is launched.

A typical settings block is:

```toml
[websockets]
ws_port = 8080
ws_silent = true
sub_topic = [""]
```

Relevant settings:

- `ws_host`: interface to bind, default `0.0.0.0`
- `ws_port`: WebSocket server port, default `9002`
- `ws_path`: WebSocket root path, default `/mads`
- `period`: FSM loop period in milliseconds
- `receive_timeout`: MADS receive timeout in milliseconds
- `ws_silent`: if `true`, suppress `libwebsockets` log messages
- `sub_topic`: MADS subscription topics for the bridge agent

`sub_topic = [""]` subscribes the bridge to all MADS topics.

## Running The Bridge

Typical usage with broker-hosted settings:

```bash
./build/src/mads-websockets tcp://localhost:9092
```

You can also override the agent name:

```bash
./build/src/mads-websockets --name websockets tcp://localhost:9092
```

At startup the agent prints the standard MADS info block, then:

- `Websocket address: ws://<host>:<port>/mads`
- `Connected clients: N`

The connected client count is updated in place whenever a client connects or
disconnects.

## `ws_client`

`ws_client` is a simple CLI tool for testing the bridge.

### Options

- `-a, --address`: base WebSocket address, default `ws://localhost:8080`
- `-t, --topic`: repeatable topic name appended as `/mads/<topic>`
- `-l, --listen`: listen and print incoming JSON to stdout
- `-p, --publish`: read JSON from stdin using `readline` and publish it
- `--debug`: enable `libwebsockets` logs

If no `-t` is given in listen mode, the default topic is `_all`.

Publish mode requires exactly one explicit `-t`.

### Listen Examples

Listen to all topics:

```bash
./build/src/ws_client -a ws://localhost:8080 -l
```

Listen to one topic:

```bash
./build/src/ws_client -a ws://localhost:8080 -t perf_assess -l
```

Listen to multiple selected topics:

```bash
./build/src/ws_client \
  -a ws://localhost:8080 \
  -t perf_assess \
  -t agent_event \
  -l
```

Each topic opens its own WebSocket connection under the same client process.

### Publish Example

Publish to one topic:

```bash
./build/src/ws_client -a ws://localhost:8080 -t mytopic -p
```

Then type JSON payloads at the prompt, for example:

```json
{"value": 42}
```

An empty line exits publish mode.

## Behavior Summary

- MADS messages are forwarded to WebSocket clients as raw JSON
- WebSocket client messages are published into MADS as raw JSON
- one WebSocket connection corresponds to one topic path
- `_all` is supported for wildcard receive subscriptions
- multiple topic subscriptions on the client side are implemented as multiple
  WebSocket connections

## Notes

- CURVE-related CLI flags are accepted by `mads-websockets` for interface
  compatibility with other MADS commands, but bridge-specific crypto handling is
  not implemented here.
- `remote_control` is intentionally not used in this application.
