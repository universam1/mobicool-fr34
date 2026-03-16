#pragma once
#include <Arduino.h>

// ── PIC16F1829 ICSP Low-Voltage Programming (LVP) over J2 header ─────────
//
// Requires 3 wires from J2 to the ESP32-C3:
//
//   J2 pin  PIC signal      PIC pin  ESP32-C3 GPIO
//   ──────  ───────────     ───────  ─────────────
//      4    ICSPDAT/RA0       19       4  (existing data wire)
//      5    ICSPCLK/RA1       18       6  (new wire)
//      1    MCLR/VPP           4       5  (new wire, open-drain)
//      3    VSS (GND)         20       GND (existing)
//      2    VDD                1       DO NOT CONNECT (PIC powered by cooler)
//
// MCLR is driven open-drain (OUTPUT+LOW / INPUT = high-Z).  The PIC has an
// internal weak pull-up on MCLR so high-Z equals VDD (PIC runs normally).
// Pull low → enters LVP programming mode after the 4-bit key sequence.
//
// NOTE: LVP programming ONLY works if the cooler's firmware already has the 
// LVP fuse enabled (CONFIG2: LVP = ON). The factory stock firmware likely 
// has LVP disabled, requiring an initial flash via an external 9V HVP 
// programmer before this ESP32 LVP flasher can be used.
//
// During programming:
//   - The PIC is held in reset (MCLR = LOW), so the cooler is inactive.
//   - RA1 (ICSPCLK) is also connected to the cooler's internal light. During
//     programming, the light might flicker rapidly, which is normal and safe.
//   - GPIO 4 (ICSPDAT) is temporarily taken from CommsMaster and returned
//     after programming completes. Call PicProgrammer::begin() which calls
//     CommsMaster::suspend(), and PicProgrammer::end() which calls resume().
//
// Protocol summary (DS41397B — PIC16(L)F1829 Programming Spec):
//   - LVP entry: MCLR low, then send 4-bit key 0x4D followed by 32-bit key
//     0x4C4D4350 ('MCHP') while clocking ICSPCLK.
//   - After LVP entry: standard ICSP 6-bit command + 14/16-bit data cycles.
//   - All data LSB-first, clocked on rising edge of ICSPCLK.
//   - Erase: bulk erase via LOAD_CONFIG + BULK_ERASE_PROGRAM + tERASE delay.
//   - Write: load 1 word, begin programming, wait tPROG (2.5 ms typical).
//   - Read: issue READ_DATA_FROM_PROGRAM + clock out 16 bits.
//   - LVP exit: release MCLR (high-Z → pulled high), release bus.

struct ProgramResult {
    bool     ok;
    uint32_t wordsWritten;
    uint32_t wordsVerified;
    char     errorMsg[64];
};

// Callback invoked periodically during flashing to report progress [0..100].
typedef void (*FlashProgressCb)(uint8_t pct, void* ctx);

class PicProgrammer {
public:
    // pin_dat  : GPIO shared with CommsMaster (GPIO 4)
    // pin_clk  : ICSPCLK (GPIO 6)
    // pin_mclr : MCLR    (GPIO 5, open-drain)
    void configure(int pin_dat, int pin_clk, int pin_mclr);

    // Flash Intel HEX data held in memory (null-terminated string).
    // Suspends normal comms, programs the PIC, restores comms.
    // progressCb is optional; called with 0..100.
    ProgramResult flash(const char* hexData, size_t hexLen,
                        FlashProgressCb progressCb = nullptr, void* cbCtx = nullptr);

    // Read the entire program memory back and return word count read,
    // or 0 on failure.  buf must be at least PIC16F1829_FLASH_WORDS entries.
    uint32_t readFlash(uint16_t* buf, uint32_t maxWords);

private:
    int _pinDat  = -1;
    int _pinClk  = -1;
    int _pinMclr = -1;

    // ── Low-level ICSP primitives ─────────────────────────────────────────
    void    enterLvp();
    void    exitLvp();

    // Send a 6-bit ICSP command (LSB first)
    void    sendCmd(uint8_t cmd);

    // Send a 16-bit data word in the 6+16+2-bit packet (LSB first, padded)
    void    sendData(uint16_t data);

    // Clock in a 16-bit data word from the PIC (READ_DATA)
    uint16_t recvData();

    // Increment PC via INCREMENT_ADDRESS command
    void    incAddr();

    // Reset PC to 0 via LOAD_CONFIG (sends dummy 0x0000 word)
    void    resetToAddr0();

    // ── Bulk erase and timed write helpers ───────────────────────────────
    bool    bulkErase();
    bool    writeProgramWord(uint16_t word);  // load + begin_prog + wait

    // ── HEX parsing ───────────────────────────────────────────────────────
    // Parse 'hexData'; write words to outBuf (14-bit words, index = PIC word address).
    // Returns true on success. maxAddr is set to highest word address seen + 1.
    static bool parseHex(const char* hexData, size_t hexLen,
                         uint16_t* wordBuf, uint32_t wordBufSize,
                         uint32_t& maxAddr);

    // GPIO helpers (inline to keep timing tight)
    inline void datHigh()  { pinMode(_pinDat, INPUT); }   // release → pullup = 1
    inline void datLow()   { digitalWrite(_pinDat, LOW); pinMode(_pinDat, OUTPUT); }
    inline int  datRead()  { pinMode(_pinDat, INPUT); return digitalRead(_pinDat); }
    inline void clkHigh()  { digitalWrite(_pinClk, HIGH); }
    inline void clkLow()   { digitalWrite(_pinClk, LOW); }
    inline void mclrLow()  { digitalWrite(_pinMclr, LOW); pinMode(_pinMclr, OUTPUT); }
    inline void mclrHigh() { pinMode(_pinMclr, INPUT); }  // open-drain, release = high-Z

    // Minimum delay between ICSP clock edges (spec: tCLKH/tCLKL ≥ 100 ns)
    // delayMicroseconds(1) is ~1 µs on ESP32-C3, which is well within spec.
    inline void tclk() { delayMicroseconds(1); }
};

// PIC16F1829 constants
static constexpr uint32_t PIC16F1829_FLASH_WORDS  = 8192;   // 0x0000–0x1FFF
static constexpr uint32_t PIC16F1829_CONFIG_BASE  = 0x8000; // word addr of CONFIG words
// ICSP commands (6-bit, DS41397B Table 3-1)
static constexpr uint8_t  ICSP_LOAD_CONFIG         = 0x00;
static constexpr uint8_t  ICSP_LOAD_DATA_PROG      = 0x02;
static constexpr uint8_t  ICSP_READ_DATA_PROG      = 0x04;
static constexpr uint8_t  ICSP_INCREMENT_ADDRESS   = 0x06;
static constexpr uint8_t  ICSP_RESET_ADDRESS       = 0x16;
static constexpr uint8_t  ICSP_BEGIN_INT_PROG      = 0x08;
static constexpr uint8_t  ICSP_END_EXT_PROG        = 0x17;  // not needed for internal timing
static constexpr uint8_t  ICSP_BULK_ERASE_PROG     = 0x09;
// Programming timing (µs) — conservative values from DS41397B Table 4-1
static constexpr uint32_t TPROG_US   = 2500;   // internal write time (2.5 ms max)
static constexpr uint32_t TERASE_US  = 5000;   // bulk erase time (5 ms max)
static constexpr uint32_t TPLH_US    = 1;      // MCLR low → prog mode setup
static constexpr uint32_t TENTH_MS   = 250;    // LVP entry hold
