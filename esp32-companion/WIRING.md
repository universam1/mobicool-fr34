# Wiring — PIC16F1829 Mainboard ↔ ESP32-C3-DevKitM-1

## Overview

The ESP32-C3 companion communicates over a **single wire** using an open-drain
half-duplex protocol (similar to Dallas 1-Wire but at standard UART framing).
Only **two connections** are needed: one data wire to PIC pin 9 (RC7) and a
shared GND.  RA5 (PIC pin 2) is **not** used.

---

## Voltage compatibility

Both the cooler mainboard and the ESP32-C3-DevKitM-1 operate at **3.3 V logic**.
No level-shifter required.

---

## Open-drain bus explained

```
3.3 V (from ESP32-C3 VDD via INPUT_PULLUP ~45 kΩ)
  │
  ├──── GPIO 4   (ESP32-C3) INPUT_PULLUP idle / OUTPUT+LOW to transmit 0
  │
  └──── RC7 / PIC pin 9     TRISC7=1 idle / TRISC7=0 to transmit 0

       Both sides only ever PULL LOW.
       The pullup is the only driver of the HIGH state.
       Collision-free because protocol is strictly request/response.
```

> **ESP32-C3-MINI-1 pin note:** GPIO 11–17 are reserved for internal SPI flash
> and are **not** available on the DevKitM-1 headers. GPIO 4 is used instead.

The ESP32-C3 internal pullup (~45 kΩ) gives a rise time of ~4.5 µs on a 10 cm
wire — well within the 104 µs bit period at 9600 baud.

---

## Solder point on the cooler mainboard

> **Important:** J4 is the dedicated UART link between the PIC and the
> **IRMCF183 motor controller** — do not use it.

| PIC pin | Name | Note |
|:-------:|------|------|
| 9       | RC7  | Data line.  Unconnected on the stock board; solder directly to the PIC pin or its via. |
| any GND | GND  | Several GND vias are available near the board edge. |

Use a **separate supply** (ESP32-C3 devkit USB or a dedicated 3.3 V/5 V regulator)
to power the ESP32-C3.  Do not draw power from the cooler mainboard.

---

## Connection table

| Cooler PCB point | Signal            | ESP32-C3 GPIO                 |
|:----------------:|-------------------|:-----------------------------:|
| PIC pin 9 (RC7)  | Data (open-drain) | **GPIO 4** (`INPUT_PULLUP`)   |
| GND pad          | GND               | **GND**                       |

**Only 2 wires.**  Do not connect VCC.

---

## Wiring diagram (ASCII)

```
 Cooler PCB                   ESP32-C3-DevKitM-1
 ┌───────────────────┐        ┌──────────────────┐
 │ PIC pin9  RC7    ─┼────────┼─ GPIO4            │
 │ GND ──────────────┼────────┼─ GND             │
 └───────────────────┘        │                  │
                              │  USB (power)     │
                              └──────────────────┘
```

---

## Protocol parameters

| Parameter  | Value               |
|------------|---------------------|
| Baud rate  | 9 600               |
| Framing    | 8N1                 |
| Topology   | Open-drain, 1 wire  |
| Direction  | ESP32 initiates; PIC responds only |
| Turnaround | ~416 µs after last request byte    |

---

## Command / telemetry summary

| Command        | ESP32 → PIC payload | PIC → ESP32 response   |
|----------------|---------------------|------------------------|
| GET_TELEMETRY  | — (0 bytes)         | 10 bytes (see below)   |
| SET_SETPOINT   | int16 LE (tenths °C)| ACK (0x06) or NAK      |
| SET_COMP_POWER | uint8 0–100 %       | ACK (0x06) or NAK      |
| SET_POWER_CAP  | uint8 0–100 %       | ACK (0x06) or NAK      |

**GET_TELEMETRY response layout** (10 bytes, all little-endian):

| Bytes | Field          | Type   | Unit    |
|-------|----------------|--------|---------|
| 0–1   | Current temp   | int16  | 0.1 °C  |
| 2–3   | Setpoint       | int16  | 0.1 °C  |
| 4–5   | Voltage        | uint16 | mV      |
| 6–7   | Fan current    | uint16 | mA      |
| 8     | Comp power     | uint8  | 0–100 % |
| 9     | Comp power cap | uint8  | 0–100 % |

`Comp power = 0` → automatic temperature control  
`Comp power > 0` → forced duty cycle (overrides thermostat)  
`Comp power cap` → hard ceiling regardless of other settings

---

## Notes

* Keep the data wire **as short as possible** (under ~30 cm is ideal) to
  avoid noise pickup from the compressor motor drive circuitry.
* For wire lengths > 30 cm add an **external 4.7 kΩ pullup** from the data
  line to 3.3 V close to the PIC pin.
* A 100 nF bypass capacitor from the data line to GND near the ESP32 pin
  can help if you see intermittent framing errors on long wires.
* **Do not** add a series resistor on the data line — it would increase the
  from each line to GND close to the ESP32 pins.
