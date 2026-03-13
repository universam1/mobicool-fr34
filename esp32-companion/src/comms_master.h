#pragma once
#include <Arduino.h>

// ── Protocol constants (must match PIC comms.h) ───────────────────────────
#define COMMS_SYNC          0xAA
#define COMMS_ACK           0x06
#define COMMS_NAK           0x15

#define COMMS_CMD_GET       0x01
#define COMMS_CMD_SET_TEMP  0x02
#define COMMS_CMD_SET_POWER 0x03
#define COMMS_CMD_SET_PMAX  0x04

// GET response layout (10 payload bytes, little-endian signed/unsigned)
//   [0-1] current temp  int16  tenths °C
//   [2-3] setpoint      int16  tenths °C
//   [4-5] voltage       uint16 mV
//   [6-7] fan current   uint16 mA
//   [8]   comp power    uint8  0-100 %
//   [9]   comp pmax     uint8  0-100 %

// ── State snapshot ────────────────────────────────────────────────────────
struct CoolerState {
    int16_t  currentTemp10;    // tenths of °C  (e.g.  123 = 12.3 °C)
    int16_t  targetTemp10;     // tenths of °C
    uint16_t voltageMilliV;    // mV             (e.g. 12400 = 12.4 V)
    uint16_t fanCurrentMilliA; // mA
    uint8_t  compPower;        // 0-100 % (0 = auto)
    uint8_t  compPowerMax;     // 0-100 % hard cap
    bool     valid;            // true if last poll succeeded
};

// ── Single-wire half-duplex master ────────────────────────────────────────
// Uses one GPIO in open-drain mode (INPUT_PULLUP = high, OUTPUT+LOW = low).
// The internal ~45 kΩ pullup is sufficient for wire lengths < 30 cm @9600 baud.
// For longer runs add an external 4.7 kΩ pullup to 3.3 V.

class CommsMaster {
public:
    // pin  : GPIO wired to PIC RC7 (PIC pin 9)
    // baud : must match PIC firmware (default 9600)
    void begin(int pin, uint32_t baud = 9600);

    // Read all telemetry in one shot; returns true on success.
    bool readAll(CoolerState& state);

    // Write commands; return true on ACK from PIC.
    bool setTargetTemp(int16_t temp10);      // tenths of °C
    bool setCompPower(uint8_t power);        // 0-100 %
    bool setCompPowerMax(uint8_t powerMax);  // 0-100 %

private:
    int      _pin    = -1;
    uint32_t _bitUs  = 104;  // µs per bit at 9600 baud

    // Low-level open-drain bit-bang
    void     txByte(uint8_t data);
    bool     rxByte(uint8_t* data, uint32_t timeoutUs);

    // Frame send/receive
    // request:  [SYNC] [cmd] [len] [payload...] [crc8]
    // response: [len] [payload...] [crc8]
    bool transact(uint8_t cmd,
                  const uint8_t* txPayload, uint8_t txLen,
                  uint8_t* rxPayload,       uint8_t expectedRxLen);

    static uint8_t crc8(const uint8_t* buf, uint8_t len);
};
