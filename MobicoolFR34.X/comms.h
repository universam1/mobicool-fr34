#ifndef COMMS_H
#define COMMS_H

#include <xc.h>
#include <stdint.h>

// ── Single-wire half-duplex protocol ──────────────────────────────────────
//
// Physical layer: open-drain bit-bang UART, 9600 8N1, RC7 only.
//   TX: LATCbits.LATC7 stays 0.  TRISC7=0 → pull low (bit=0).
//                                 TRISC7=1 → hi-Z, pullup → high (bit=1).
//   RX: TRISC7=1, sample PORTCbits.RC7.
//   Line idles HIGH via ESP32 INPUT_PULLUP (~45 kΩ).  No external resistor.
//
// Protocol (ESP32 always initiates, PIC responds only):
//   Request:  [SYNC=0xAA] [CMD] [LEN] [PAYLOAD×LEN] [CRC8]
//   Response: [LEN] [PAYLOAD×LEN] [CRC8]
//   CRC8: XOR of all preceding bytes in the frame.

#define COMMS_SYNC          0xAA
#define COMMS_ACK           0x06
#define COMMS_NAK           0x15
#define COMMS_MAX_PAYLOAD   4

// Commands
#define COMMS_CMD_GET       0x01  // No payload → 10-byte telemetry response
#define COMMS_CMD_SET_TEMP  0x02  // Payload: int16 LE (tenths °C) → ACK/NAK
#define COMMS_CMD_SET_POWER 0x03  // Payload: uint8 0-100 % → ACK/NAK
#define COMMS_CMD_SET_PMAX  0x04  // Payload: uint8 0-100 % → ACK/NAK

// GET response payload layout (10 bytes, all little-endian)
//   [0-1] current temp  int16 tenths °C
//   [2-3] setpoint      int16 tenths °C
//   [4-5] voltage       uint16 mV
//   [6-7] fan current   uint16 mA
//   [8]   comp power    uint8  0-100 %
//   [9]   comp pmax     uint8  0-100 %

// Pin definitions — RC7 open-drain bidirectional
#define COMMS_PIN           PORTCbits.RC7
#define COMMS_TRIS          TRISCbits.TRISC7
#define COMMS_LAT           LATCbits.LATC7

void    Comms_Initialize(void);
void    Comms_Process(void);

int16_t Comms_GetTargetTemperature(void);
void    Comms_SetTargetTemperature(int16_t temp);
uint8_t Comms_GetCompressorPower(void);
void    Comms_SetCompressorPower(uint8_t power);
uint8_t Comms_GetMaxPowerLimit(void);

#endif // COMMS_H
