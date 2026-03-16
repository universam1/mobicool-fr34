#ifndef COMMS_H
#define COMMS_H

#include <xc.h>
#include <stdint.h>

// ── Single-wire half-duplex protocol ──────────────────────────────────────
//
// Physical layer: open-drain bit-bang UART, 9600 8N1, RA0 (ICSPDAT) only.
//   RA0 is PIC pin 19, available on the J2 ICSP header — no soldering required.
//   TX: LATAbits.LATA0 stays 0.  TRISA0=0 → pull low (bit=0).
//                                 TRISA0=1 → hi-Z, pullup → high (bit=1).
//   RX: TRISA0=1, sample PORTAbits.RA0.
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
#define COMMS_CMD_SET_PMODE 0x05  // Payload: uint8 (0=ECO 1=NORMAL 2=HI) → ACK/NAK

// GET response payload layout (11 bytes, all little-endian)
//   [0-1] current temp  int16 tenths °C
//   [2-3] setpoint      int16 tenths °C
//   [4-5] voltage       uint16 mV
//   [6-7] fan current   uint16 mA
//   [8]   comp power    uint8  0-100 %
//   [9]   comp pmax     uint8  0-100 %
//  [10]   power mode    uint8  0=ECO 1=NORMAL 2=HI

// Pin definitions — RA0 (ICSPDAT, PIC pin 19, J2 header) open-drain bidirectional
#define COMMS_PIN           PORTAbits.RA0
#define COMMS_TRIS          TRISAbits.TRISA0
#define COMMS_LAT           LATAbits.LATA0

void    Comms_Initialize(void);
void    Comms_Process(void);

int16_t Comms_GetTargetTemperature(void);
void    Comms_SetTargetTemperature(int16_t temp);
uint8_t Comms_GetCompressorPower(void);
void    Comms_SetCompressorPower(uint8_t power);
uint8_t Comms_GetMaxPowerLimit(void);
uint8_t Comms_GetPowerMode(void);
void    Comms_SetPowerMode(uint8_t mode);

#endif // COMMS_H
