# React native websocket client

## Purpose

We build a React Native client for the MADS WebSocket gateway provided by the main project. Keep it simple and effective.

## GUI storyboard

The GUI has to be split in three screens (accessible as a bar on the bottom):

* **Connect**: This has a button that scans the QR provied by the `mads-websocket` in its banner (that defines a JSON)
  - Then there is an address:port input line, where the user can digit hostname or IP, or select one of those proposed in the QR from a drop down menu (also with port). If the QR is scanned, default the address field with the first address in the QR
  - Then there is a text field for entering the subscription topics, separated by commas with optional spaces
  - finally there is a "Connect" button (conditionally disabled): when pressed opens the connection to the broker, one per selected topic, unless the topic field contains `_all` or it is empty, in which case it uses a single ws connection
* **Listen**: this presents a list of the last messages received, one per topic. Each message shall be shown as a collapsible tree, with the topic name as the root element. Only show the last message. On top there is a toggle button "Expand/Collapse all". Listening must stop when the user switches to another panel, without cleaning the messages list. Under the list there is a "Clear" button that clears the list of topics/messages
* **Publish**: this presents a top text field for inputting the topic to use, with a drop down menu for selecting the topics used in the past and a button to clean that list; then there is a text area for entering the JSON message, possibly with syntax coloring and checking. When the JSON is valid, the "Send" button below the text area is enabled, and when pressed publishes the message. Keep one persistent publish connection open for the selected topic and reconnect if the topic changes.

Ensure that values entered bu the user in the text fields and in the JSON text area are persistent upon relaunch