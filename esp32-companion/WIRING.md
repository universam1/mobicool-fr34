# Wiring — PIC16F1829 Mainboard ↔ ESP32

## Voltage compatibility

Both the cooler mainboard and the ESP32 devkit operate at **3.3 V logic**.
No level-shifter or resistor divider is required on the UART lines.

---

## Solder points on the cooler mainboard

The Modbus software UART uses **RA5** (PIC pin 2, TX) and **RC7** (PIC pin 9,
RX).  These do **not** break out to any named header on the stock PCB.

> **Important:** J4 on the mainboard is the dedicated UART link between the
> PIC and the **IRMCF183 motor controller** — do not use it for Modbus.

| PIC pin | Name | Note |
|:-------:|------|------|
| 2       | RA5  | Modbus TX (PIC transmits).  A ~1 kΩ series resistor sits between this pin and the TM1620B DIO pad — soldering to either side of that resistor is fine; the 1 kΩ acts as a useful series terminator. |
| 9       | RC7  | Modbus RX (PIC receives).  This trace is unconnected on the stock board; solder directly to the PIC pin or its via. |
| any GND | GND  | Several GND vias are available near the board edge. |

Use a **separate 3.3 V or 5 V supply** (ESP32 devkit USB or a dedicated
regulator) to power the ESP32.  Do not attempt to draw power from the
cooler mainboard.

---

## Connection table

| Cooler PCB point | Signal   | ESP32 DevKit GPIO | ESP32 function |
|-----------------|----------|:-----------------:|----------------|
| PIC pin 2 (RA5) | Modbus TX | **GPIO 16**      | UART2 RX       |
| PIC pin 9 (RC7) | Modbus RX | **GPIO 17**      | UART2 TX       |
| GND pad         | GND       | **GND**          | Common ground  |

**Do not connect VCC** — power the ESP32 independently via USB or a regulator.

---

## Wiring diagram (ASCII)

```
 Cooler PCB                   ESP32 DevKit
 ┌───────────────────┐        ┌──────────────────┐
 │ PIC pin2  RA5  TX─┼────────┼─ GPIO16 (RX2)    │
 │ PIC pin9  RC7  RX─┼────────┼─ GPIO17 (TX2)    │
 │ GND ──────────────┼────────┼─ GND             │
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
