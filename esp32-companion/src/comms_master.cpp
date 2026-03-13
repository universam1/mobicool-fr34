#include "comms_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Critical-section mutex protecting per-byte bit-bang timing from FreeRTOS
// task preemption.  1 byte ≈ 1 ms critical section — acceptable for WiFi.
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

// ── Initialise ────────────────────────────────────────────────────────────
void CommsMaster::begin(int pin, uint32_t baud) {
    _pin   = pin;
    _bitUs = 1000000UL / baud;  // µs per bit (104 for 9600 baud)
    pinMode(_pin, INPUT_PULLUP);
    delay(5); // settle line
}

// ── CRC8: XOR of all bytes (must match PIC comms.c) ──────────────────────
uint8_t CommsMaster::crc8(const uint8_t* buf, uint8_t len) {
    uint8_t crc = 0;
    while (len--) crc ^= *buf++;
    return crc;
}

// ── Open-drain TX ─────────────────────────────────────────────────────────
// Logic 0: drive LOW (OUTPUT + LOW).  Logic 1: release (INPUT_PULLUP).
// After the stop bit the pin is left in INPUT_PULLUP = receive mode.
void CommsMaster::txByte(uint8_t data) {
    portENTER_CRITICAL(&_mux);

    // Start bit (low)
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
    delayMicroseconds(_bitUs);

    // 8 data bits, LSB first
    for (int i = 0; i < 8; i++) {
        if (data & 0x01) {
            pinMode(_pin, INPUT_PULLUP);       // release → high
        } else {
            pinMode(_pin, OUTPUT);
            digitalWrite(_pin, LOW);           // pull low
        }
        data >>= 1;
        delayMicroseconds(_bitUs);
    }

    // Stop bit: release → high
    pinMode(_pin, INPUT_PULLUP);
    delayMicroseconds(_bitUs);

    portEXIT_CRITICAL(&_mux);
}

// ── Open-drain RX ─────────────────────────────────────────────────────────
// Polls for start bit outside the critical section (may wait up to timeoutUs),
// then reads bits inside a critical section for precise timing.
bool CommsMaster::rxByte(uint8_t* data, uint32_t timeoutUs) {
    pinMode(_pin, INPUT_PULLUP);

    // Wait for falling edge (start bit) with timeout
    uint32_t deadline = micros() + timeoutUs;
    while (digitalRead(_pin) != LOW) {
        if ((int32_t)(micros() - deadline) >= 0) return false;
    }

    // Per-bit section: protect from task preemption
    portENTER_CRITICAL(&_mux);

    // Sample at centre of start bit
    delayMicroseconds(_bitUs / 2);
    if (digitalRead(_pin) != LOW) {
        portEXIT_CRITICAL(&_mux);
        return false; // glitch — not a real start bit
    }

    // Read 8 data bits, LSB first
    uint8_t d = 0;
    for (int i = 0; i < 8; i++) {
        delayMicroseconds(_bitUs);
        d >>= 1;
        if (digitalRead(_pin)) d |= 0x80;
    }

    // Verify stop bit
    delayMicroseconds(_bitUs);
    bool stopOk = (digitalRead(_pin) == HIGH);

    portEXIT_CRITICAL(&_mux);

    if (!stopOk) return false;
    *data = d;
    return true;
}

// ── Full request/response transaction ────────────────────────────────────
// Request frame:  [SYNC=0xAA] [cmd] [len] [payload×len] [crc8]
// Response frame: [len] [payload×len] [crc8]
bool CommsMaster::transact(uint8_t cmd,
                            const uint8_t* txPayload, uint8_t txLen,
                            uint8_t* rxPayload,       uint8_t expectedRxLen)
{
    if (_pin < 0) return false;

    // Build and send request
    uint8_t frame[8]; // SYNC + CMD + LEN + up to 4 payload bytes + CRC
    frame[0] = COMMS_SYNC;
    frame[1] = cmd;
    frame[2] = txLen;
    for (uint8_t i = 0; i < txLen; i++) frame[3 + i] = txPayload[i];
    frame[3 + txLen] = crc8(frame, 3 + txLen);

    for (uint8_t i = 0; i <= 3 + txLen; i++) txByte(frame[i]);

    // Turnaround: pin is already INPUT_PULLUP after last stop bit.
    // PIC waits 4 × 104 µs = 416 µs before transmitting response.
    // We wait 300 µs then start polling — gives ~116 µs margin.
    delayMicroseconds(300);

    // Receive response: [LEN] [PAYLOAD...] [CRC8]
    uint8_t resp_len;
    if (!rxByte(&resp_len, 2000)) return false;
    if (resp_len != expectedRxLen) return false;

    uint8_t resp_payload[10]; // large enough for GET (10 bytes)
    for (uint8_t i = 0; i < resp_len; i++) {
        if (!rxByte(&resp_payload[i], 500)) return false;
    }

    uint8_t resp_crc;
    if (!rxByte(&resp_crc, 500)) return false;

    // Validate CRC: XOR of [LEN] + [PAYLOAD...]
    uint8_t crc_buf[11];
    crc_buf[0] = resp_len;
    for (uint8_t i = 0; i < resp_len; i++) crc_buf[1 + i] = resp_payload[i];
    if (crc8(crc_buf, 1 + resp_len) != resp_crc) return false;

    if (rxPayload) {
        for (uint8_t i = 0; i < resp_len; i++) rxPayload[i] = resp_payload[i];
    }
    return true;
}

// ── Public commands ───────────────────────────────────────────────────────
bool CommsMaster::readAll(CoolerState& state) {
    state.valid = false;
    uint8_t resp[10];
    if (!transact(COMMS_CMD_GET, nullptr, 0, resp, 10)) return false;

    state.currentTemp10    = (int16_t)((uint16_t)resp[0] | ((uint16_t)resp[1] << 8));
    state.targetTemp10     = (int16_t)((uint16_t)resp[2] | ((uint16_t)resp[3] << 8));
    state.voltageMilliV    = (uint16_t)resp[4] | ((uint16_t)resp[5] << 8);
    state.fanCurrentMilliA = (uint16_t)resp[6] | ((uint16_t)resp[7] << 8);
    state.compPower        = resp[8];
    state.compPowerMax     = resp[9];
    state.valid            = true;
    return true;
}

bool CommsMaster::setTargetTemp(int16_t temp10) {
    uint8_t payload[2] = {
        (uint8_t)(temp10),
        (uint8_t)((uint16_t)temp10 >> 8)
    };
    uint8_t resp[1];
    if (!transact(COMMS_CMD_SET_TEMP, payload, 2, resp, 1)) return false;
    return resp[0] == COMMS_ACK;
}

bool CommsMaster::setCompPower(uint8_t power) {
    uint8_t resp[1];
    if (!transact(COMMS_CMD_SET_POWER, &power, 1, resp, 1)) return false;
    return resp[0] == COMMS_ACK;
}

bool CommsMaster::setCompPowerMax(uint8_t powerMax) {
    uint8_t resp[1];
    if (!transact(COMMS_CMD_SET_PMAX, &powerMax, 1, resp, 1)) return false;
    return resp[0] == COMMS_ACK;
}
