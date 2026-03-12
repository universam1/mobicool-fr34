#pragma once
#include <Arduino.h>

// ── Modbus register map (mirrors the PIC firmware's modbus.h) ─────────────
#define MB_REG_CURRENT_TEMP   0x0000  // int16, tenths of °C, read-only
#define MB_REG_TARGET_TEMP    0x0001  // int16, tenths of °C, read/write
#define MB_REG_VOLTAGE        0x0002  // uint16, mV, read-only
#define MB_REG_FAN_CURRENT    0x0003  // uint16, mA, read-only
#define MB_REG_COMP_POWER     0x0004  // uint8, 0-100 % override (0=auto), read/write
#define MB_REG_COMP_POWER_MAX 0x0005  // uint8, 0-100 % hard cap, read/write

#define MB_SLAVE_ADDR         0x01
#define MB_NUM_REGS           6       // read all registers in one FC03 burst

// ── Parsed snapshot of all cooler registers ───────────────────────────────
struct CoolerState {
    int16_t  currentTemp10;   // tenths of °C (e.g. 123 = 12.3 °C)
    int16_t  targetTemp10;    // tenths of °C
    uint16_t voltageMilliV;   // mV (e.g. 12400 = 12.4 V)
    uint16_t fanCurrentMilliA;// mA
    uint8_t  compPower;       // 0-100 % (0 = temperature-auto mode)
    uint8_t  compPowerMax;    // 0-100 % hard cap
    bool     valid;           // true if last poll succeeded
};

// ── ModbusMaster ──────────────────────────────────────────────────────────
class ModbusMaster {
public:
    // uart   – HardwareSerial instance (e.g. Serial2)
    // rxPin  – GPIO wired to PIC RA5 (PIC TX)
    // txPin  – GPIO wired to PIC RC7 (PIC RX)
    // baud   – must match PIC firmware (9600)
    void begin(HardwareSerial& uart, int rxPin, int txPin, uint32_t baud = 9600);

    // Read all 6 registers in one shot; returns true on success.
    bool readAll(CoolerState& state);

    // Write a single register (FC06); returns true on success.
    bool writeRegister(uint16_t reg, uint16_t value);

private:
    HardwareSerial* _uart = nullptr;

    static uint16_t crc16(const uint8_t* buf, uint8_t len);
    bool sendAndReceive(uint8_t* req, uint8_t reqLen,
                        uint8_t* resp, uint8_t respLen,
                        uint32_t timeoutMs = 100);
};
