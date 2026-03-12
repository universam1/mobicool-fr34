# Wiring — PIC16F1829 Mainboard ↔ ESP32

## Voltage compatibility

Both the cooler mainboard and the ESP32 devkit operate at **3.3 V logic**.
No level-shifter or resistor divider is required on the UART lines.

---

## Connector on the cooler mainboard

On many FR34 variants the Modbus/UART test-pad header is labelled **J4** and
sits near the edge of the PCB.  It is a 4-pin 2.54 mm (0.1 in) pitch header.

| J4 pin | Signal label | Direction (PIC) |
|--------|-------------|-----------------|
| 1      | VCC 3.3 V   | — (do **not** power the ESP32 from here; current limit unknown) |
| 2      | TX (RA5)    | PIC transmits → |
| 3      | RX (RC7)    | PIC receives ←  |
| 4      | GND         | —               |

> **Note:** Pin 1 numbering may differ on board revisions.  Verify with a
> multimeter before connecting.  Use a **separate 3.3 V or 5 V supply** (ESP32
> devkit USB or a dedicated regulator) to power the ESP32.

---

## Connection table

| Cooler J4 pin | Signal  | ESP32 DevKit GPIO | ESP32 function |
|:---:|---------|:-----------------:|----------------|
| 2   | TX (RA5) | **GPIO 16**      | UART2 RX       |
| 3   | RX (RC7) | **GPIO 17**      | UART2 TX       |
| 4   | GND      | **GND**          | Common ground  |

**Do not connect VCC** — the ESP32 must be powered independently.

---

## Wiring diagram (ASCII)

```
 Cooler mainboard J4          ESP32 DevKit
 ┌───────────────────┐        ┌──────────────────┐
 │ pin2  RA5  TX ────┼────────┼─ GPIO16 (RX2)    │
 │ pin3  RC7  RX ────┼────────┼─ GPIO17 (TX2)    │
 │ pin4  GND  ───────┼────────┼─ GND             │
 └───────────────────┘        │                  │
                              │  USB (power)     │
                              └──────────────────┘
```

---

## Modbus parameters

| Parameter     | Value          |
|---------------|----------------|
| Baud rate     | 9 600          |
| Data bits     | 8              |
| Parity        | None           |
| Stop bits     | 1 (8N1)        |
| Slave address | 0x01           |

---

## Register map

| Address | Name           | Type    | Unit       | Access     |
|---------|----------------|---------|------------|------------|
| 0x0000  | CURRENT_TEMP   | int16   | 0.1 °C     | Read-only  |
| 0x0001  | TARGET_TEMP    | int16   | 0.1 °C     | Read/Write |
| 0x0002  | VOLTAGE        | uint16  | mV         | Read-only  |
| 0x0003  | FAN_CURRENT    | uint16  | mA         | Read-only  |
| 0x0004  | COMP_POWER     | uint8   | 0–100 %    | Read/Write |
| 0x0005  | COMP_POWER_MAX | uint8   | 0–100 %    | Read/Write |

`COMP_POWER = 0` → automatic temperature control  
`COMP_POWER > 0` → forced duty cycle (overrides thermostat)  
`COMP_POWER_MAX` → hard ceiling on compressor duty cycle regardless of other settings

---

## Notes

* Keep the wires **as short as reasonably possible** (under ~30 cm is ideal)
  to avoid picking up noise from the compressor motor drive circuitry.
* If you experience communication errors, add a small series resistor
  (100 Ω – 470 Ω) on each UART line and/or a 100 nF bypass capacitor
  from each line to GND close to the ESP32 pins.
