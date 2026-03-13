#include "comms.h"
#include "analog.h"
#include <stdbool.h>

// ── Timing ────────────────────────────────────────────────────────────────
// 1 MHz instruction clock, TMR0 1:4 prescaler → 4 µs / tick
// 9600 baud → 104.17 µs / bit → 26 ticks / bit
#define BIT_TIME          26
#define HALF_BIT_TIME     13
// Turnaround guard: PIC waits this many bit-times before responding,
// giving the ESP32 time to switch from TX to INPUT_PULLUP (~300 µs).
#define TURNAROUND_BITS   4   // 4 × 104 µs = 416 µs

// ── State ─────────────────────────────────────────────────────────────────
static int16_t targetTemperature  = 50;   // Default 5.0 °C (tenths)
static uint8_t compressorPower    = 0;    // Default 0 % (auto)
static uint8_t compressorMaxPower = 100;  // Default 100 %

// ── Timer0 initialisation ─────────────────────────────────────────────────
// Same configuration as the former comms code — TMR0 is shared for bit timing.
static void Timer0_Initialize(void) {
    OPTION_REGbits.TMR0CS = 0;      // Internal clock (FOSC/4)
    OPTION_REGbits.PSA    = 0;      // Prescaler assigned to TMR0
    OPTION_REGbits.PS     = 0b001;  // 1:4 prescaler
    TMR0 = 0;
}

// ── CRC8 (simple XOR of all bytes) ────────────────────────────────────────
static uint8_t crc8(const uint8_t *buf, uint8_t len) {
    uint8_t crc = 0;
    while (len--) crc ^= *buf++;
    return crc;
}

// ── Open-drain TX helpers ─────────────────────────────────────────────────
// LATC7 is permanently 0 (set at init).  Direction controls the line level:
//   TRISC7 = 0 → output drives 0 (pull low)
//   TRISC7 = 1 → input/hi-Z, pullup holds line high

// Transmit one byte (8N1).  Leaves TRISC7=1 (hi-Z) after stop bit.
static void comms_tx_byte(uint8_t data) {
    uint8_t t = (uint8_t)TMR0;

    // Start bit (pull low)
    COMMS_TRIS = 0;
    t += BIT_TIME;
    while ((uint8_t)TMR0 != t);

    // 8 data bits, LSB first
    for (uint8_t i = 0; i < 8; i++) {
        COMMS_TRIS = (data & 0x01) ? 1 : 0;
        data >>= 1;
        t += BIT_TIME;
        while ((uint8_t)TMR0 != t);
    }

    // Stop bit (release → high)
    COMMS_TRIS = 1;
    t += BIT_TIME;
    while ((uint8_t)TMR0 != t);
}

// ── RX helper ─────────────────────────────────────────────────────────────
// Block-waits for a start bit up to `timeout_ticks` TMR0 ticks, then
// reads a full 8N1 byte.  Returns false on timeout or framing error.
static bool comms_rx_byte(uint8_t *data, uint8_t timeout_ticks) {
    uint8_t t0 = (uint8_t)TMR0;

    // Wait for falling edge (start bit)
    while (COMMS_PIN != 0) {
        if ((uint8_t)((uint8_t)TMR0 - t0) >= timeout_ticks) return false;
    }

    // Sample at centre of start bit
    uint8_t t = (uint8_t)TMR0 + HALF_BIT_TIME;
    while ((uint8_t)TMR0 != t);
    if (COMMS_PIN != 0) return false; // glitch — not a real start bit

    // Advance to centre of first data bit
    t += BIT_TIME;

    uint8_t d = 0;
    for (uint8_t i = 0; i < 8; i++) {
        while ((uint8_t)TMR0 != t);
        d >>= 1;
        if (COMMS_PIN) d |= 0x80;
        t += BIT_TIME;
    }

    // Verify stop bit
    while ((uint8_t)TMR0 != t);
    if (COMMS_PIN == 0) return false; // framing error

    *data = d;
    return true;
}

// ── Response helpers ──────────────────────────────────────────────────────
static void comms_respond(const uint8_t *payload, uint8_t len) {
    // Frame: [LEN] [PAYLOAD...] [CRC8]
    uint8_t crc = len;
    comms_tx_byte(len);
    for (uint8_t i = 0; i < len; i++) {
        crc ^= payload[i];
        comms_tx_byte(payload[i]);
    }
    comms_tx_byte(crc);
    // Ensure pin left as input (hi-Z) — already done by comms_tx_byte stop bit
    COMMS_TRIS = 1;
}

static void comms_respond_ack(void) {
    uint8_t ack = COMMS_ACK;
    comms_respond(&ack, 1);
}

static void comms_respond_nak(void) {
    uint8_t nak = COMMS_NAK;
    comms_respond(&nak, 1);
}

// ── Command dispatcher ────────────────────────────────────────────────────
static void comms_handle(uint8_t cmd, const uint8_t *payload, uint8_t len) {
    switch (cmd) {

        case COMMS_CMD_GET: {
            int16_t  temp = AnalogGetTemperature10();
            int16_t  setp = targetTemperature;
            uint16_t volt = AnalogGetVoltage();
            uint16_t fanc = AnalogGetFanCurrent();
            uint8_t resp[10] = {
                (uint8_t)(temp),         (uint8_t)((uint16_t)temp >> 8),
                (uint8_t)(setp),         (uint8_t)((uint16_t)setp >> 8),
                (uint8_t)(volt),         (uint8_t)(volt >> 8),
                (uint8_t)(fanc),         (uint8_t)(fanc >> 8),
                compressorPower,
                compressorMaxPower
            };
            comms_respond(resp, sizeof(resp));
            break;
        }

        case COMMS_CMD_SET_TEMP: {
            if (len < 2) { comms_respond_nak(); break; }
            int16_t t = (int16_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
            targetTemperature = t;
            comms_respond_ack();
            break;
        }

        case COMMS_CMD_SET_POWER: {
            if (len < 1 || payload[0] > 100) { comms_respond_nak(); break; }
            Comms_SetCompressorPower(payload[0]);
            comms_respond_ack();
            break;
        }

        case COMMS_CMD_SET_PMAX: {
            if (len < 1 || payload[0] > 100) { comms_respond_nak(); break; }
            compressorMaxPower = payload[0];
            if (compressorPower > compressorMaxPower)
                compressorPower = compressorMaxPower;
            comms_respond_ack();
            break;
        }

        default:
            break; // unknown command — no response (ESP32 will time out)
    }
}

// ── Public API ────────────────────────────────────────────────────────────
void Comms_Initialize(void) {
    Timer0_Initialize();
    COMMS_LAT  = 0;    // always drive 0; direction controls the level
    COMMS_TRIS = 1;    // start as input (hi-Z, line held high by pullup)
}

// Call from main loop every iteration.  Non-blocking when line is idle.
void Comms_Process(void) {
    // Quick idle check — avoids any overhead when no master is talking
    if (COMMS_PIN == 1) return;

    // Potential start bit on line — try to receive SYNC byte
    uint8_t b;
    if (!comms_rx_byte(&b, 255)) return;
    if (b != COMMS_SYNC) return;

    // Receive CMD
    uint8_t cmd;
    if (!comms_rx_byte(&cmd, 255)) return;

    // Receive LEN
    uint8_t len;
    if (!comms_rx_byte(&len, 255)) return;
    if (len > COMMS_MAX_PAYLOAD) return;

    // Receive PAYLOAD
    uint8_t payload[COMMS_MAX_PAYLOAD];
    for (uint8_t i = 0; i < len; i++) {
        if (!comms_rx_byte(&payload[i], 255)) return;
    }

    // Receive CRC
    uint8_t rx_crc;
    if (!comms_rx_byte(&rx_crc, 255)) return;

    // Validate CRC: XOR of SYNC + CMD + LEN + PAYLOAD
    uint8_t frame[3 + COMMS_MAX_PAYLOAD];
    frame[0] = COMMS_SYNC;
    frame[1] = cmd;
    frame[2] = len;
    for (uint8_t i = 0; i < len; i++) frame[3 + i] = payload[i];
    if (crc8(frame, 3 + len) != rx_crc) return;

    // Turnaround guard: wait for ESP32 to switch from TX to INPUT_PULLUP
    uint8_t t = (uint8_t)TMR0 + (uint8_t)(TURNAROUND_BITS * BIT_TIME);
    while ((uint8_t)TMR0 != t);

    comms_handle(cmd, payload, len);
}

int16_t Comms_GetTargetTemperature(void)          { return targetTemperature; }
void    Comms_SetTargetTemperature(int16_t temp)  { targetTemperature = temp; }

uint8_t Comms_GetCompressorPower(void)            { return compressorPower; }
void    Comms_SetCompressorPower(uint8_t power) {
    if (power <= compressorMaxPower) compressorPower = power;
}

uint8_t Comms_GetMaxPowerLimit(void)              { return compressorMaxPower; }
