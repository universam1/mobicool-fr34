#include "modbus_master.h"

// ── CRC-16/IBM (same polynomial as PIC firmware) ─────────────────────────
uint16_t ModbusMaster::crc16(const uint8_t* buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *buf++;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

// ── Initialise UART ───────────────────────────────────────────────────────
void ModbusMaster::begin(HardwareSerial& uart, int rxPin, int txPin, uint32_t baud) {
    _uart = &uart;
    _uart->begin(baud, SERIAL_8N1, rxPin, txPin);
    // Flush any noise on the line
    delay(5);
    while (_uart->available()) _uart->read();
}

// ── Low-level send + receive with timeout ────────────────────────────────
bool ModbusMaster::sendAndReceive(uint8_t* req, uint8_t reqLen,
                                  uint8_t* resp, uint8_t respLen,
                                  uint32_t timeoutMs) {
    if (!_uart) return false;

    // Modbus inter-frame silent interval: 3.5 char × ~1.04 ms @ 9600 baud = ~3.65 ms
    delay(4);
    while (_uart->available()) _uart->read(); // flush stale bytes

    _uart->write(req, reqLen);
    _uart->flush(); // wait for TX complete

    uint32_t start = millis();
    uint8_t  idx   = 0;
    while (idx < respLen) {
        if (millis() - start > timeoutMs) return false;
        if (_uart->available()) {
            resp[idx++] = (uint8_t)_uart->read();
            start = millis(); // reset timeout on each received byte
        }
    }
    return true;
}

// ── FC03: read all registers ──────────────────────────────────────────────
bool ModbusMaster::readAll(CoolerState& state) {
    state.valid = false;

    // Build request: [addr, FC03, startH, startL, qtyH, qtyL, crcL, crcH]
    uint8_t req[8];
    req[0] = MB_SLAVE_ADDR;
    req[1] = 0x03;               // FC03 read holding registers
    req[2] = 0x00;               // start address high
    req[3] = MB_REG_CURRENT_TEMP;// start address low
    req[4] = 0x00;               // quantity high
    req[5] = MB_NUM_REGS;        // quantity low
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    // Expected response: [addr, FC03, byteCount, d0H, d0L, ... d5H, d5L, crcL, crcH]
    const uint8_t respLen = 3 + MB_NUM_REGS * 2 + 2; // = 17 bytes
    uint8_t resp[17];

    if (!sendAndReceive(req, sizeof(req), resp, respLen)) return false;

    // Validate address and function code
    if (resp[0] != MB_SLAVE_ADDR || resp[1] != 0x03) return false;
    if (resp[2] != MB_NUM_REGS * 2) return false;

    // Validate CRC
    uint16_t rxCrc = (uint16_t)resp[respLen - 1] << 8 | resp[respLen - 2];
    if (crc16(resp, respLen - 2) != rxCrc) return false;

    // Parse register values (big-endian)
    auto reg = [&](uint8_t i) -> int16_t {
        return (int16_t)((uint16_t)resp[3 + i * 2] << 8 | resp[4 + i * 2]);
    };

    state.currentTemp10    = reg(0);
    state.targetTemp10     = reg(1);
    state.voltageMilliV    = (uint16_t)reg(2);
    state.fanCurrentMilliA = (uint16_t)reg(3);
    state.compPower        = (uint8_t)constrain(reg(4), 0, 100);
    state.compPowerMax     = (uint8_t)constrain(reg(5), 0, 100);
    state.valid            = true;
    return true;
}

// ── FC06: write single register ───────────────────────────────────────────
bool ModbusMaster::writeRegister(uint16_t reg, uint16_t value) {
    // Build request: [addr, FC06, regH, regL, valH, valL, crcL, crcH]
    uint8_t req[8];
    req[0] = MB_SLAVE_ADDR;
    req[1] = 0x06;           // FC06
    req[2] = (reg >> 8) & 0xFF;
    req[3] = reg & 0xFF;
    req[4] = (value >> 8) & 0xFF;
    req[5] = value & 0xFF;
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    // FC06 response is an echo of the request (8 bytes)
    uint8_t resp[8];
    if (!sendAndReceive(req, sizeof(req), resp, sizeof(resp))) return false;
    if (resp[0] != MB_SLAVE_ADDR || resp[1] != 0x06) return false;
    uint16_t rxCrc = (uint16_t)resp[7] << 8 | resp[6];
    return crc16(resp, 6) == rxCrc;
}
