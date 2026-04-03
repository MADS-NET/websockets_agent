# React Client

`react_client` is a standalone Expo/React Native workspace for the future
mobile-side UI of the MADS WebSockets bridge.

This directory is intentionally independent from the native CMake build.

## Current scope

- Expo managed app
- TypeScript enabled
- QR scanning for bridge bootstrap JSON
- Connect, Listen, and Publish tabs
- Persistent form values and publish topic history
- Live listener sockets active only while the Listen tab is open
- Persistent publish socket that reconnects when the selected topic changes

## Expected bootstrap flow

The native bridge prints a terminal QR code that encodes a JSON payload like:

```json
{
  "scheme": "ws",
  "port": 9002,
  "path": "/mads",
  "addresses": ["192.168.1.20", "10.0.0.14"]
}
```

The client scans that payload, lets the user pick a reachable LAN endpoint, and
uses the advertised `path` and `port` to build `ws://` URLs for the bridge.

## Behaviour notes

- Connect opens one listener socket per selected topic.
- If the topic field is empty or contains `_all`, the client uses one wildcard
  listener socket.
- The Listen tab stores only the latest JSON payload per topic.
- Publish keeps one persistent socket open for the selected topic and rotates it
  whenever the publish topic changes.

## Local development

Install dependencies:

```bash
npm install
```

Start Expo:

```bash
npm start
```

Then open the project in Expo Go on a device connected to the same LAN as the
bridge.
