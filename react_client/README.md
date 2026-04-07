# React Client

`react_client` is the Expo/React Native workspace for the MADS WebSockets UI.
It still runs as a standalone Expo app for development, and the native CMake
build now exports it for the web and embeds the generated static files into the
`mads-websockets` binary.

## Current scope

- Expo managed app
- TypeScript enabled
- QR scanning for bridge bootstrap JSON
- Listen and Publish tabs
- Persistent form values and publish topic history
- Live listener sockets active only while the Listen tab is open
- Persistent publish socket that reconnects when the selected topic changes

## Browser bootstrap flow

When served by `mads-websockets`, the browser page loads from a QR-linked HTTP
URL such as:

```text
http://192.168.1.20:8080/mads
```

The page then fetches:

```text
http://192.168.1.20:8080/bootstrap.json
```

That JSON contains the WebSocket settings used by the browser client.

## Native bootstrap flow

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

- Listen keeps the subscription topics field and a Subscribe button at the top of the pane.
- Subscribe opens one listener socket per selected topic and reconnects when the subscription set changes.
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

Then open the project in Expo Go or a simulator connected to the same LAN as
the bridge.

To export the web bundle manually:

```bash
npm run export:web
```
