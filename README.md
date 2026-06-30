# WallArt

ESP32 + FastLED LED sign — a 20×10 grid of 200 WS2812B LEDs (10 rows, one data pin per
row) with a built-in WiFi web UI and over-the-air firmware updates. Personal project.

## Hardware
Classic ESP32-WROOM-32, 200 WS2812B (GRB). Rows 1–10 = GPIO 23, 22, 21, 19, 18, 5, 17, 16, 4, 2.

## Build
- Arduino IDE, board: **ESP32 Dev Module**.
- **ESP32 board package must be 2.0.17** — 3.x reboot-loops the 10 strips (only 8 RMT channels).
- Libraries: FastLED, WiFiManager.
- The web page lives in `index_html.h`, not the `.ino` (the IDE mis-parses JS inside the sketch).

## Use
First boot opens a `WallArt-Setup` WiFi hotspot for network setup; after that, open
`http://wallart.local` to control it. Firmware updates wirelessly (OTA) from the Arduino IDE.
