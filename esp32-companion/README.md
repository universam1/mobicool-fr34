# ESP32 Companion for Mobicool FR34 Cooler

Adds wireless monitoring and control to the Mobicool FR34 compressor cooler.
The ESP32 speaks **Modbus RTU** over the 3-wire UART exposed on the PIC16F1829
mainboard, then exposes two selectable transport modes:

| Transport | How to access |
|-----------|---------------|
| **WiFi AP + BLE** (`fr34-dual`, default) | Both interfaces active simultaneously |
| **WiFi AP** (`fr34-wifi`) | Connect to the `FR34-Cooler` open WiFi network, open `http://192.168.4.1/` in any browser |
| **BLE GATT** (`fr34-ble`) | Open the [Web Bluetooth PWA](#ble--web-bluetooth-pwa) in Chrome on Android |

Both transports expose the same data and controls:
- Cabinet temperature (read)
- Target temperature / setpoint (read + write)
- Supply voltage (read)
- Fan current (read)
- Compressor override duty cycle 0–100 % (read + write, 0 = auto)
- Compressor power cap 0–100 % (read + write)

---

## Hardware

See [WIRING.md](WIRING.md) for full solder-point details.

**Summary:**

| Cooler PCB point   | ESP32 GPIO | Direction |
|--------------------|:----------:|-----------|
| PIC pin 2 — RA5 TX | GPIO 16 (RX2) | Cooler → ESP32 |
| PIC pin 9 — RC7 RX | GPIO 17 (TX2) | ESP32 → Cooler |
| GND                | GND        | Common ground |

Power the ESP32 from USB or a dedicated regulator — do **not** draw power from
the cooler mainboard.

---

## Building & Flashing

[PlatformIO](https://platformio.org/) is required.

```sh
# WiFi + BLE simultaneously (default)
pio run -e fr34-dual -t upload

# WiFi + WebSocket dashboard only
pio run -e fr34-wifi -t upload

# BLE GATT server only
pio run -e fr34-ble -t upload

# Serial monitor
pio device monitor
```

The `default_envs` in `platformio.ini` is `fr34-dual`, so a plain `pio run`
builds both transports. Use the single-transport targets to save flash space
(the dual build uses ~96% of the default 1.25 MB partition).

### Dependencies (managed automatically by PlatformIO)

| Environment | Libraries |
|-------------|-----------|| `fr34-dual` | `esphome/AsyncTCP-esphome`, `esphome/ESPAsyncWebServer-esphome`, `bblanchon/ArduinoJson`, `h2zero/NimBLE-Arduino` || `fr34-wifi` | `esphome/AsyncTCP-esphome`, `esphome/ESPAsyncWebServer-esphome`, `bblanchon/ArduinoJson` |
| `fr34-ble`  | `h2zero/NimBLE-Arduino` |

---

## WiFi / WebSocket Dashboard

Firmware: build with `fr34-wifi`.

1. Flash and power on the ESP32.
2. Connect your phone or laptop to the **`FR34-Cooler`** open WiFi network.
3. Open **`http://192.168.4.1/`** in any browser.

The Vue 3 single-page app (embedded in `src/web_ui.h`, Vue runtime in
`src/vue_js.h`) is served entirely from ESP32 flash — no internet access is
needed. The page opens a WebSocket to `ws://192.168.4.1/ws` and receives
live updates every second.

A lightweight REST endpoint is also available for polling:

```
GET http://192.168.4.1/api/state
```

Returns a JSON object with the same fields as the WebSocket messages.

---

## BLE / Web Bluetooth PWA

Firmware: build with `fr34-ble`.

The BLE variant exposes a custom GATT service. Pair it with the installable
Progressive Web App hosted in `docs/` (served via GitHub Pages).

### Enabling GitHub Pages

In the repository Settings → **Pages** → Source: **Deploy from a branch**,
branch `master`, folder `/docs`.

The app will be available at:

```
https://<owner>.github.io/esp32-companion/
```

> Web Bluetooth requires **HTTPS** — GitHub Pages provides this automatically.

### Using the PWA on Android

1. Open the GitHub Pages URL in **Chrome on Android**.
2. Tap **Connect** in the app header.
3. Select **FR34-Cooler** from the Bluetooth device picker.
4. The app connects and starts receiving live updates via BLE notifications.
5. Tap the browser menu → **Add to Home Screen** to install as a standalone
   app (no Play Store required).

Once installed, the app shell is cached by the service worker and launches
offline. It reconnects to the cooler automatically when Bluetooth is
available — no extra tap needed after the first pairing.

> **Browser support:** Chrome on Android (v56+), Chrome desktop (with
> `chrome://flags/#enable-web-bluetooth`).  
> Firefox and Safari do **not** support the Web Bluetooth API.

### GATT Service Layout

Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

| Characteristic | UUID | Properties | Payload |
|----------------|------|------------|---------|
| Status         | `beb5483e-…` | READ, NOTIFY | 10 bytes — see below |
| Set temperature | `beb5483f-…` | WRITE | `int16` little-endian, tenths of °C |
| Set compressor power | `beb54840-…` | WRITE | `uint8`, 0–100 % |
| Set power cap  | `beb54841-…` | WRITE | `uint8`, 0–100 % |

**Status payload (10 bytes, little-endian):**

| Offset | Type | Field | Unit |
|--------|------|-------|------|
| 0–1 | `int16` | Cabinet temperature | 0.1 °C |
| 2–3 | `int16` | Target temperature | 0.1 °C |
| 4–5 | `uint16` | Voltage | mV |
| 6–7 | `uint16` | Fan current | mA |
| 8 | `uint8` | Compressor duty | 0–100 % (0 = auto) |
| 9 | `uint8` | Compressor power cap | 0–100 % |

The payload fits within the default 20-byte ATT MTU; no MTU negotiation is
required.

---

## Project Structure

```
platformio.ini          Build configuration (fr34-wifi / fr34-ble targets)
WIRING.md               Solder points, wiring diagram, register map
src/
  main.cpp              Application entry point; transport selection via #ifdef
  modbus_master.h/.cpp  Modbus RTU driver (FC03 read, FC06 write)
  web_ui.h              WiFi dashboard HTML (Vue 3 SPA, raw-string literal)
  vue_js.h              Vue 3 production build embedded for offline serving
docs/                   Web Bluetooth PWA (served via GitHub Pages)
  index.html            Vue 3 SPA using Web Bluetooth API
  manifest.json         PWA manifest (standalone display, icons)
  sw.js                 Service worker — cache-first, offline support
  icon.svg              App icon (SVG, any size)
  icon-192.png          App icon 192×192 (home screen install prompt)
  icon-512.png          App icon 512×512 (splash screen)
```

---

## Modbus Register Map

See [WIRING.md](WIRING.md#register-map) for the full table. Quick reference:

| Address | Name | Access | Unit |
|---------|------|--------|------|
| `0x0000` | Current temperature | R | 0.1 °C |
| `0x0001` | Target temperature | R/W | 0.1 °C |
| `0x0002` | Voltage | R | mV |
| `0x0003` | Fan current | R | mA |
| `0x0004` | Compressor power override | R/W | 0–100 % (0 = auto) |
| `0x0005` | Compressor power cap | R/W | 0–100 % |

Protocol: 9600 baud, 8N1, slave address `0x01`.
