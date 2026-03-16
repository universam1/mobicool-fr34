#include "comms_master.h"
#include "driver/gpio.h"

// ── Initialise ────────────────────────────────────────────────────────────
void CommsMaster::begin(int pin, uint32_t baud) {
    _pin = pin;

    // Route both Hardware RX and TX to the same pin.
    // The ESP32 HardwareSerial driver detects (rxPin == txPin) and
    // automatically enables half-duplex single-wire mode (open-drain).
    // ESP32-C3 only has UART0 (Serial) and UART1 (Serial1); Serial1 is used here.
    Serial1.begin(baud, SERIAL_8N1, pin, pin);

    // Explicitly enable the internal pullup (~45kΩ) on this pin using ESP-IDF.
    // We use gpio_pullup_en() instead of pinMode() because pinMode() would
    // inadvertently disconnect the hardware UART from the pin matrix.
    gpio_pullup_en((gpio_num_t)pin);

    // Flush any power-on noise from the wire
    delay(5);
    while (Serial1.available()) Serial1.read();
}

// ── CRC8: XOR of all bytes (must match PIC comms.c) ──────────────────────
uint8_t CommsMaster::crc8(const uint8_t* buf, uint8_t len) {
    uint8_t crc = 0;
    while (len--) crc ^= *buf++;
    return crc;
}

// ── Full request/response transaction ────────────────────────────────────
// Request frame:  [SYNC=0xAA] [cmd] [len] [payload×len] [crc8]
// Response frame: [len] [payload×len] [crc8]
bool CommsMaster::transact(uint8_t cmd,
                            const uint8_t* txPayload, uint8_t txLen,
                            uint8_t* rxPayload,       uint8_t expectedRxLen)
{
    if (_pin < 0) return false;

    // 1. Flush any stale bytes that may have arrived unexpectedly
    while (Serial1.available()) Serial1.read();

    // 2. Build the request frame
    uint8_t reqLen = 4 + txLen;
    uint8_t frame[16];
    frame[0] = COMMS_SYNC;
    frame[1] = cmd;
    frame[2] = txLen;
    for (uint8_t i = 0; i < txLen; i++) frame[3 + i] = txPayload[i];
    frame[3 + txLen] = crc8(frame, 3 + txLen);

    // 3. Transmit (zero CPU blocking thanks to hardware UART)
    Serial1.write(frame, reqLen);
    Serial1.flush(); // Block until the transmission is fully physically complete

    // 4. Discard the hardware loopback.
    // Because RX and TX share the same pin, our own transmission is echoed back.
    uint8_t discarded = 0;
    uint32_t start = millis();
    while (discarded < reqLen) {
        if (Serial1.available()) {
            Serial1.read();
            discarded++;
        }
        if (millis() - start > 100) return false; // Timeout reading our own echo
    }

    // 5. Receive the PIC's response length (with a slightly longer 200ms turnaround timeout)
    start = millis();
    while (!Serial1.available()) {
        if (millis() - start > 200) return false; 
    }
    uint8_t respLen = Serial1.read();
    
    if (respLen != expectedRxLen) return false;

    // 6. Receive the response payload
    uint8_t respPayload[10]; // large enough for GET_TELEMETRY (10 bytes)
    for (uint8_t i = 0; i < respLen; i++) {
        start = millis();
        while (!Serial1.available()) {
            if (millis() - start > 50) return false;
        }
        respPayload[i] = Serial1.read();
    }

    // 7. Receive response CRC
    start = millis();
    while (!Serial1.available()) {
        if (millis() - start > 50) return false;
    }
    uint8_t rxCrc = Serial1.read();

    // 8. Validate CRC: XOR of [LEN] + [PAYLOAD...]
    uint8_t crc_buf[11];
    crc_buf[0] = respLen;
    for (uint8_t i = 0; i < respLen; i++) crc_buf[1 + i] = respPayload[i];
    if (crc8(crc_buf, 1 + respLen) != rxCrc) return false;

    // 9. Deliver payload to caller
    if (rxPayload) {
        for (uint8_t i = 0; i < respLen; i++) rxPayload[i] = respPayload[i];
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
