# Agent Context: Mobicool FR34/FR40 Custom Firmware & ESP32 Companion

This file contains accumulated knowledge, architectural decisions, and technical details to quickly onboard AI agents working on this repository, preventing the need to re-analyze historical changes.

## 1. Project Overview
A two-part solution to upgrade and remotely manage a Mobicool FR34/FR40 compressor cooler.
1. **PIC Firmware (`MobicoolFR34.X/`)**: Custom bare-metal C firmware for the cooler's internal PIC16F1829 microcontroller. Fixes hardware quirks, enables -18°C deep freeze, handles battery voltage monitoring cleanly, and interfaces with the internal IRMCF183 inverter board that runs the compressor.
2. **ESP32-C3 Companion (`esp32-companion/`)**: A modular PlatformIO-based ESP32-C3 node that acts as a wireless bridge. It serves a Vue 3 web application over Wi-Fi and/or operates as a BLE GATT server to control the cooler via Web Bluetooth.

## 2. Hardware Interface & Wiring
- **Voltages**: Both the PIC logic and ESP32-C3 operate at **3.3 V**. No level-shifters are required.
- **Connection**: Only 2 wires are used (Data + GND) using a single-wire half-duplex topology.
  - **Data Pin**: Connect to **PIC pin 19 (RA0/ICSPDAT)** via the **J2 ICSP header** — no soldering to a PIC pin required. Do **NOT** use the J4 header (J4 is the PIC ↔ IRMCF183 proprietary motor-controller link). Disconnect the ESP32 before ICSP programming.
  - **ESP32-C3 Pin**: **GPIO 4**. (GPIO 11–17 are reserved for the ESP32-C3-MINI-1 internal SPI flash and are not accessible on the DevKitM-1 headers.)
- **Power**: The ESP32-C3 must be powered by its own separate 5V/3.3V supply. The cooler's internal LDO cannot reliably handle the ESP32's current spikes.
- **Electrical Topology**: Open-drain on both sides. The ESP32-C3 uses an internal `INPUT_PULLUP` (~45 kΩ), which is electrically sufficient for wires < 30 cm at 9600 baud. No external pull-up is needed unless the wire is long or the environment is excessively noisy.

## 3. Communication Protocol (`comms.c` / `comms_master.cpp`)
The protocol is a lightweight custom 1-wire architecture designed for efficiency, reducing memory usage on the PIC and freeing up PIC Pin 2 (RA5).

### Layer 1: Hardware/Physical
- **Topology**: Single-wire half-duplex (9600 baud, 8N1). 
- **ESP32-C3 Side (Master)**: Uses hardware UART (`Serial1`, UART1). The GPIO Matrix routes both TX and RX hardware signals to the same physical pin (GPIO 4). The pad is configured as `OUTPUT_OPEN_DRAIN | INPUT_PULLUP`. Because TX & RX share a pin, the hardware UART hears its own echoes — the code explicitly discards this hardware loopback by throwing away `1 + payload_len` bytes immediately after transmission. This achieves a 0% blocking latency on the ESP32-C3 CPU (crucial for Wi-Fi/BLE stability).
  - `Serial` (UART0) is used for debug output through the integrated USB Serial/JTAG peripheral; the DevKitM-1 has no separate USB-UART chip. Do **not** add `-DARDUINO_USB_CDC_ON_BOOT=1` to build flags — the board.json handles USB configuration and adding this flag breaks `Serial` symbol resolution (USB-JTAG is not USB OTG CDC).
- **PIC Side (Slave)**: Uses a software bit-banged UART. The single hardware EUSART is occupied by the IRMCF183. Interrupts are briefly disabled (for ~1ms) during byte reception/transmission. Since this is just a super-loop thermostat, this jitter has no negative impact.

### Layer 2: Framing
- strict Master-Slave request/response. The ESP32 always initiates.
- *Request format*: `[SYNC 0xAA] [CMD] [LEN] [PAYLOAD × LEN] [CRC8]`
- *Response format*: `[LEN] [PAYLOAD × LEN] [CRC8]`
- *CRC8 block*: Simple XOR of all preceding bytes.

### Commands
- `0x01` (`GET`): Reads 10 bytes mapping to: current temp (int16), setpoint (int16), voltage (uint16), fan current (uint16), comp power (uint8), comp cap (uint8). All multi-byte integers are Little-Endian.
- `0x02` (`SET_TEMP`): Payload 2 bytes (int16 tenths °C). Returns `ACK (0x06)`
- `0x03` (`SET_POWER`): Payload 1 byte (uint8 %). Forces compressor duty cycle.
- `0x04` (`SET_PMAX`): Payload 1 byte (uint8 %). Hard cap for battery management.

## 4. Sub-Project Details

### PIC Firmware (`MobicoolFR34.X/`)
- **Compilation**: Completely Dockerized. Run `./build.sh` from the repo root.
- **Toolchain**: Uses Microchip `xc8-cc v3.10` with `PIC12-16F1xxx_DFP`. The Dockerfile downloads these securely from Microchip servers (FreeMode license).
- **Timers**:
  - `TMR1`: main 1-second system tick.
  - `TMR0`: fast polling timer for bit-banging and IRMCF timeout recovery.
- **Historical Fixes to remember**:
  - Replaced 16-bit comparisons on 8-bit timer values (`uint8_t tmpTMR0 = (uint8_t)TMR0`) to prevent infinite looping wrap-around bugs.
  - Corrected `MIN_VALID_TEMP` from `-150` to `-200` to allow the cooler to reach its hardware minimum of -18°C.
  - Decoupled payload/comms inter-frame delays from `TMR1` (which caused jitter), utilizing `TMR0IF` manual overflow polling instead.

### ESP32-C3 Companion (`esp32-companion/`)
- **Board**: ESP32-C3-DevKitM-1 (ESP32-C3-MINI-1 module, integrated USB Serial/JTAG, no separate USB-UART chip).
- **Framework**: Arduino core via PlatformIO.
- **Transports / Environments**:
  - `pio run -e fr34-wifi`: Hosts an Access Point (`FR34-Cooler`, `192.168.4.1`) serving an embedded Vue 3 SPA over an `AsyncWebServer`. Real-time metrics pushed via `AsyncWebSocket`. Needs no router.
  - `pio run -e fr34-ble`: Hosts a BLE GATT server (`NimBLE`). Designed to interoperate with a Web Bluetooth application hosted on GitHub Pages (`docs/`).
  - `pio run -e fr34-dual`: Runs Wi-Fi and BLE concurrently.
- **Architecture Notes**:
  - Vue 3 HTML, CSS (Tailwind CDN), and JS are bundled as C++ `R"rawhtml(...)rawhtml"` raw string literals to avoid SPIFFS/LittleFS flashing overhead. The build relies exclusively on the firmware binary.
  - The `CommsMaster::transact` function waits >400 µs before expecting a PIC response, catering to the strict instruction turnaround guard baked into the PIC bit-bang state machine.

## 5. Typical Workflows for AI Agents
To introduce new telemetry to the dashboard:
1. Identify the metric in `MobicoolFR34.X/main.c`.
2. Expand the `GET` payload length in `MobicoolFR34.X/comms.h` & `.c`. Update the `crc8`.
3. Expand the `CoolerState` struct in `esp32-companion/src/comms_master.h`.
4. Parse the new offset in `comms_master.cpp`.
5. Update the JSON serialization in `esp32-companion/src/main.cpp`.
6. Add UI elements to `esp32-companion/src/web_ui.h`.
7. Rebuild both:
   `./build.sh`
   `pio run -e fr34-dual`