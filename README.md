# mobicool-fr34
Alternate firmware for Mobicool FR34/FR40 compressor cooler

There are two reasons for this firmware: First I wanted to explore the possibility to run the cooler down to deep-freeze -18C temperature (as the more expensive Waeco/Dometic units can). I also wanted to be able to run the cooler on an 18V Li-Ion battery pack without the battery monitor getting all freaked out.

Mainboard top and bottom (notice the ground plane in the board easily visible by J2, not a simple 2-layer job):
![Main board top](Images/MainBoardTop.JPG "Mainboard Top")
![Main board bot](Images/MainBoardBottom.JPG "Mainboard Bottom")

Display and buttons board top and bottom:
![Disp board top](Images/DisplayBoardTop.JPG "Display board Top")
![Disp board bot](Images/DisplayBoardBottom.JPG "Display board Bottom")

The input is protected from reverse voltage, the 3.3V powering the logic is provided by U3, an LDO (!), yes even from 27+V as it receives when plugged into mains. There's a 12V DC/DC converter powering the cooling fan and some part of the compressor motor driver. The interior LED light is powered directly from the input voltage with a load switch and a series resistor, so intensity will vary depending on input voltage. 

PIC16F1829 pins as used on this board:

Pin | Function
--- | ---
1  VDD |
2  RA5 | TM1620B DIO through ~1k resistor (not used by this firmware, pin 3 is used for both input and output)
3  RA4 | TM1620B DIO
4  MCLR |
5  RC5 | TM1620B CLK
6  RC4 | TM1620B STB
7  RC3 AN7 | Fan current sense (150mOhm to ground)
8  RC6 AN8 | Compressor current analog input (pin 3 of mcp6002e opamp)
9  RC7 | not connected
10 RB7 | TX to IRMCF183, also connected to INT0!
11 RB6 | Output controlling a load switch (fan 12V enable from DC/DC)
12 RB5 | RX from IRMCF183
13 RB4 AN10 | Analog input (1.77V constant, likely 1.8V from IRMCF)
14 RC2 | Output controlling a load switch (12V DC/DC enable)
15 RC1 AN5 | 10k NTC input with 10k to 3v3 (cooler compartment temperature)
16 RC0 | Output controlling a load switch (MMUN2232), needs to be on for compressor to start/run
17 RA2 AN2 | Input voltage monitor (10V in == 598mV)
18 ICSP clk RA1 | (also connected to load switch for internal light)
19 ICSP dat RA0 | ESP32 companion single-wire data (J2 header pin)
20 GND | 

The display/buttons board uses an interesting chip I've never seen before, the TM1620B from Shenzhen Titan Micro Electronics (http://www.titanmec.com/index.php/en/product/view/id/285.html) with a Chinese-language-only datasheet. Luckily it is a very straight-forward chip to program, take a look at the tm1620b.c code, where the segment mapping is also described for this particular application. 

The motor controller for the brushless DC-motor driving the compressor is an IRMCF183 - this has pre-flashed firmware inside that directly understands very primitive UART commands of 8 bytes: 0xe1, 0xeb, 0x90, motor run (1) or stop(0), then revolutions per second, 0x00, 0x00, checksum (which is a simple addition of the first 7 bytes). The response seems to be 0xe1 (only?). I couldn't find any example project from Infineon matching this packet structure, maybe someone recognizes it from somewhere else? This firmware may very well be used in other Dometic coolers using the Wancool AMV13JZ compressor. I have not tried to access the JTAG port on the IRMCF183 chip, but there's a nice space for an unpopulated connector (J1) right at the board edge :) J4 is the UART interface between PIC and IRMCF183.

This firmware was originally developed using the MPLAB X IDE v4.20 and the free XC8 C compiler v2.00.

## Cooler UI & Menu Guide

The physical buttons on the cooler operate as follows:

### ON/OFF Button
* **Long Press (Hold for a few seconds):** Powers the cooler ON or OFF.
* **Short Press:** Cycles through the informational **Status Menu**:
  1. `XX.XV` - Battery/Supply Voltage
  2. `XX W` - Compressor Power Consumption (Approximate)
  3. Compressor Timer (Countdown to next run/lockout)
  4. `%` - Compressor Speed (Duty cycle percentage)
  5. `A` - Fan Current (mA)
  6. Temperature Rate of Change

### SET Button
* **Short Press:** Enters the **Settings Menu** and cycles through configuration options:
  1. **Target Temperature:** Use `PLUS (+)` and `MINUS (-)` to adjust the target setpoint (down to -18°C).
  2. **Power Mode:** Use `+` or `-` to select `ECo`, `Std`, or `Hi` compressor behavior.
  3. **Battery Monitor Level:** Use `+` or `-` to select `HI`, `MEd`, or `Lo` for battery voltage cut-off thresholds.
* The menu times out automatically after a few seconds of inactivity, permanently saving your changes to EEPROM.

### Power Modes

The cooler offers three compressor control strategies:

* **Eco:** Prioritizes low power consumption and battery longevity.
  * Speed is capped to approximately 30% of the hardware maximum.
  * Speed backs off when compressor power exceeds **30 %** (vs. 45 % in Std).
  * Uses a **2.0 °C restart hysteresis**: once the compressor stops, the cabinet is allowed to warm 2 °C above the setpoint before it starts again, reducing cycling frequency.
* **Std:** Balanced automatic speed control.
  * Speed is capped slightly below the hardware maximum for longevity.
  * Speed backs off when compressor power exceeds **45 %**.
  * Uses a **1.0 °C restart hysteresis**.
* **Hi:** Prioritizes pull-down speed and sustained hold performance.
  * Speed is uncapped (full hardware maximum).
  * Speed ramps directly to maximum on every 60-second tick — no gradual step-up.
  * The power throttle is **disabled** entirely; the compressor runs as hard as it can.
  * After reaching the target temperature, the compressor stays on at minimum speed instead of shutting off immediately, and only turns off after the cabinet cools **2.0 °C below the setpoint**.

## Building

The firmware can be compiled entirely from Docker — no MPLAB X IDE or local toolchain installation required. The build downloads XC8 v3.10 and the PIC12-16F1xxx Device Family Pack automatically.

**Prerequisites:** Docker (or Podman) installed and running.

```bash
./build.sh
```

Output: `MobicoolFR34.X/dist/default/production/MobicoolFR34.X.production.hex`

### What the build does

1. Pulls an `ubuntu:22.04` base image
2. Downloads the XC8 v3.10 compiler directly from Microchip's servers and installs it in free/evaluation mode
3. Downloads the `PIC12-16F1xxx_DFP` device support pack from Microchip's pack server (required by XC8 v3.x)
4. Compiles all sources with `xc8-cc -mcpu=16F1829 -O2` and produces a `.hex` file

### Memory usage (as of last successful build)

```
Program space   used 1C33h (7219) of 2000h words  (88.1%)
Data space      used  2FAh ( 762) of  400h bytes  (74.4%)
EEPROM space    used    0h (   0) of  100h bytes   (0.0%)
Configuration bits              2 of    2 words  (100.0%)
```

### Flashing

Program the resulting `.hex` file using any PIC programmer (e.g. PICkit 3) via the ICSP connector (J2, square pin = MCLR). The system voltage is 3.3V. If the LVP fuse was disabled in the factory-programmed parts, 9V (not 12V) must be applied to MCLR for programming.

The ICSP connector (J2) is a standard pinout one where pin 1 (MCLR) being the square one. Note that the system voltage is 3.3V and the LVP program fuse most likely was disabled in the pre-programmed parts, so 9V (not 12V!) has to be applied to MCLR to program.


## WiFi Remote Control (ESP32 Companion)

The `esp32-companion/` directory contains a self-contained [PlatformIO](https://platformio.org/) project for an **ESP32** module that acts as a WiFi access point and web dashboard, allowing you to monitor and control the cooler from any phone or browser — no router required.

### Features

| Feature | Detail |
|---------|--------|
| WiFi AP | SSID `FR34-Cooler`, open network, IP `192.168.4.1` |
| Web UI  | Vue 3 SPA with live metrics, setpoint ±0.5 °C buttons, compressor override & power-cap sliders |
| Protocol | WebSocket for real-time push updates (1 s interval) |
| Comms   | Single-wire half-duplex, 9600 baud, open-drain on RA0/ICSPDAT (PIC pin 19, J2 header) — **RA5 not needed** |
| REST API | `GET /api/state` returns current state as JSON |

### Wiring

See [`esp32-companion/WIRING.md`](esp32-companion/WIRING.md) for the full wiring table and ASCII diagram.  
**TL;DR** — **two wires only** (one data + GND), connected to J2 header pin 19 (RA0/ICSPDAT), no level-shifter, no external resistor:

| Cooler PCB point              | Signal            | ESP32 GPIO |
|:-----------------------------:|-------------------|:----------:|
| PIC pin 19 (RA0/ICSPDAT, J2) | Data (open-drain) | GPIO 4 (`INPUT_PULLUP`) |
| GND pad                       | GND               | GND |

> **Note:** J4 on the mainboard is the PIC↔IRMCF183 motor-controller link — do not use it. RA5 (PIC pin 2) is not used by the ESP32 companion. Connect the data wire to the J2 ICSP header pin 19 (RA0/ICSPDAT). The ESP32 internal pullup (~45 kΩ) is sufficient for wire lengths up to ~30 cm; add an external 4.7 kΩ pullup to 3.3 V for longer runs.

### Building & flashing the ESP32

**Prerequisites:** [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code.

```bash
pio run -e fr34-dual --target upload
pio device monitor -e fr34-dual
```

PlatformIO will automatically fetch all library dependencies on first build. The repository root now contains the `platformio.ini`, while the ESP32 sources remain under `esp32-companion/src/`.

### Using the dashboard

1. Connect your phone or laptop to the `FR34-Cooler` WiFi network.
2. Open a browser and navigate to `http://192.168.4.1/`.
3. The dashboard shows live temperature, voltage, fan current, and compressor duty cycle.
4. Use the **+0.5 / −0.5** buttons to change the target temperature.
5. Drag the **Compressor Override** slider to force a fixed duty cycle (0 = auto).
6. Drag the **Power Limit** slider to set a hard cap on the compressor (useful for battery management).

### Project structure

```
esp32-companion/
└── src/
    ├── main.cpp            # WiFi AP, HTTP server, WebSocket, poll loop
    ├── comms_master.h      # CoolerState struct + CommsMaster declaration
    ├── comms_master.cpp    # Single-wire communication implementation
    └── web_ui.h            # Vue 3 SPA embedded as C string literal
```

Root files:

```
platformio.ini              # Root-level PlatformIO project configuration for the ESP32 companion
```

---

Here are two final images showing the remaining parts inside and the exterior:
![Interior](Images/WancoolCompressor.JPG "Compressor and power supply")
![Exterior](Images/MobicoolFR34ExteriorOriginalFirmware.JPG "Exterior of Mobicool FR34, with original firmware")
(the leading zero in the exterior shot reveals that it is running the original firmware :))
