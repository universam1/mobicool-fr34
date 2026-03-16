#include "pic_programmer.h"
#include "driver/gpio.h"

// ── Configuration ─────────────────────────────────────────────────────────
void PicProgrammer::configure(int pin_dat, int pin_clk, int pin_mclr) {
    _pinDat  = pin_dat;
    _pinClk  = pin_clk;
    _pinMclr = pin_mclr;
}

// ── LVP entry sequence ────────────────────────────────────────────────────
// DS41397B §3.0: hold MCLR low, clock in 32-bit key 0x4D434850 ('MCHP')
// LSB first while ICSPCLK is toggled.
void PicProgrammer::enterLvp() {
    // Initialise pins to safe idle state
    pinMode(_pinClk, OUTPUT);
    clkLow();
    datHigh();      // release data
    mclrHigh();     // release MCLR → PIC running
    delay(1);

    // Drive MCLR low — must be done BEFORE clocking the key
    mclrLow();
    delayMicroseconds(TPLH_US);

    // Clock the 32-bit LVP key 0x4D434850 ('MCHP') LSB-first
    // (DS41397B §3.2.1 — send bit 0 first). The 32-bit key does NOT use
    // start or stop bits.
    static const uint32_t KEY = 0x4D434850UL;  // 'M','C','H','P'

    for (int i = 0; i < 32; i++) {
        if (KEY & (1UL << i)) datHigh(); else datLow();
        tclk(); clkHigh(); tclk(); clkLow();
    }

    // Release data line
    datHigh();
    tclk();

    // Hold time after key before first command (spec: tENTRY ≥ 1 µs)
    delayMicroseconds(TENTH_MS);
}

// ── LVP exit ──────────────────────────────────────────────────────────────
void PicProgrammer::exitLvp() {
    clkLow();
    datHigh();
    delay(1);
    mclrHigh();  // release MCLR → PIC runs new firmware
    delay(10);
    // Return GPIO 4 to the state CommsMaster needs (Serial1 will reconfigure)
    // Caller is responsible for re-calling CommsMaster::begin().
}

// ── Send 6-bit command ────────────────────────────────────────────────────
void PicProgrammer::sendCmd(uint8_t cmd) {
    for (int i = 0; i < 6; i++) {
        if (cmd & (1 << i)) datHigh(); else datLow();
        tclk(); clkHigh(); tclk(); clkLow();
    }
    datHigh();
}

// ── Send 16-bit data word ─────────────────────────────────────────────────
// ICSP data payload is 16 bits, framed with a leading 0 and trailing 0 bit.
// Full 18-bit transaction: [0] | D[0..13] | 0x0 padding | [0]
// The 14-bit PIC word occupies bits 1..14; bit 0 and bit 15 are START/STOP=0.
void PicProgrammer::sendData(uint16_t data) {
    // START bit
    datLow();
    tclk(); clkHigh(); tclk(); clkLow();
    // 14 data bits (bit 0 first; top 2 bits of 16-bit frame are 0)
    for (int i = 0; i < 14; i++) {
        if (data & (1 << i)) datHigh(); else datLow();
        tclk(); clkHigh(); tclk(); clkLow();
    }
    // STOP bit
    datLow();
    tclk(); clkHigh(); tclk(); clkLow();
    datHigh();
}

// ── Receive 16-bit data word ──────────────────────────────────────────────
// The PIC drives the data line for all 16 bits of a Read response.
// Bit 0 = Start (0), Bits 1-14 = Data, Bit 15 = Stop (0).
// We provide 16 clocks and sample the 14 data bits.
uint16_t PicProgrammer::recvData() {
    uint16_t val = 0;
    datHigh();  // ensure we are in high-Z mode; PIC will drive the line

    for (int i = 0; i < 16; i++) {
        tclk(); clkHigh(); tclk();
        
        // Sample data on the rising edge / while clock is high
        // Bits 1 through 14 contain the 14-bit payload
        if (i >= 1 && i <= 14) {
            if (datRead()) val |= (1 << (i - 1));
        }

        clkLow();
    }
    return val;
}

// ── Increment address ─────────────────────────────────────────────────────
void PicProgrammer::incAddr() {
    sendCmd(ICSP_INCREMENT_ADDRESS);
    delayMicroseconds(1);
}

// ── Reset PC to word address 0 ────────────────────────────────────────────
void PicProgrammer::resetToAddr0() {
    sendCmd(ICSP_RESET_ADDRESS);
    delayMicroseconds(1);
}

// ── Bulk erase ────────────────────────────────────────────────────────────
// Erases entire flash + config words in one shot.
// Sequence: LOAD_CONFIG(0x0000), then BULK_ERASE_PROGRAM, wait TERASE.
bool PicProgrammer::bulkErase() {
    // Set PC to config space (required for bulk erase to cover all memory)
    sendCmd(ICSP_LOAD_CONFIG);
    sendData(0x0000);  // dummy word → PC moves to 0x8000
    // Bulk erase command
    sendCmd(ICSP_BULK_ERASE_PROG);
    delayMicroseconds(TERASE_US);
    return true;
}

// ── Write one program word at current PC ─────────────────────────────────
bool PicProgrammer::writeProgramWord(uint16_t word) {
    sendCmd(ICSP_LOAD_DATA_PROG);
    sendData(word);
    sendCmd(ICSP_BEGIN_INT_PROG);
    delayMicroseconds(TPROG_US);
    return true;
}

// ── Intel HEX parser ──────────────────────────────────────────────────────
// Parses standard Intel HEX (record types 00=data, 01=EOF, 04=ext linear addr).
// PIC16 tools emit byte addresses; PIC word address = byte address / 2.
// Only words within [0, wordBufSize) are stored. Config words at 0x8000+
// are silently ignored (we do not program config in this flow).
bool PicProgrammer::parseHex(const char* hexData, size_t hexLen,
                              uint16_t* wordBuf, uint32_t wordBufSize,
                              uint32_t& maxAddr) {
    maxAddr = 0;
    memset(wordBuf, 0xFF, wordBufSize * sizeof(uint16_t));  // erased state = 0x3FFF but 0xFFFF is fine to send

    const char* p   = hexData;
    const char* end = hexData + hexLen;
    uint32_t extBase = 0;  // extended linear address (bytes)

    while (p < end) {
        // Skip whitespace / CRLF
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' ')) p++;
        if (p >= end) break;
        if (*p != ':') return false;  // malformed
        p++;  // consume ':'

        // Read one hex record
        auto hexByte = [&]() -> int {
            if (p + 2 > end) return -1;
            char hi = *p++, lo = *p++;
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int h = nibble(hi), l = nibble(lo);
            if (h < 0 || l < 0) return -1;
            return (h << 4) | l;
        };

        int byteCount = hexByte(); if (byteCount < 0) return false;
        int addrHi    = hexByte(); if (addrHi    < 0) return false;
        int addrLo    = hexByte(); if (addrLo    < 0) return false;
        int recType   = hexByte(); if (recType   < 0) return false;

        uint8_t checksum = byteCount + addrHi + addrLo + recType;
        uint8_t data[64];
        if (byteCount > 64) return false;
        for (int i = 0; i < byteCount; i++) {
            int b = hexByte(); if (b < 0) return false;
            data[i] = (uint8_t)b;
            checksum += data[i];
        }
        int cs = hexByte(); if (cs < 0) return false;
        if ((uint8_t)(checksum + cs) != 0) return false;  // checksum fail

        if (recType == 0x01) break;  // EOF record

        if (recType == 0x04) {  // Extended Linear Address
            if (byteCount < 2) return false;
            extBase = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16);
            continue;
        }

        if (recType == 0x00) {  // Data record
            uint32_t byteAddr = extBase + ((addrHi << 8) | addrLo);
            // PIC16 HEX files use byte addresses; convert to word addr
            uint32_t wordAddr = byteAddr / 2;
            // Only store program memory words (skip config at 0x8000+)
            for (int i = 0; i + 1 < byteCount; i += 2) {
                if (wordAddr < wordBufSize) {
                    wordBuf[wordAddr] = (uint16_t)(data[i] | (data[i + 1] << 8));
                    if (wordAddr + 1 > maxAddr) maxAddr = wordAddr + 1;
                }
                wordAddr++;
            }
            continue;
        }
        // Ignore other record types (02, 03, 05)
    }
    return maxAddr > 0;
}

// ── High-level flash ──────────────────────────────────────────────────────
ProgramResult PicProgrammer::flash(const char* hexData, size_t hexLen,
                                   FlashProgressCb progressCb, void* cbCtx) {
    ProgramResult result = {false, 0, 0, ""};

    if (_pinDat < 0 || _pinClk < 0 || _pinMclr < 0) {
        strlcpy(result.errorMsg, "Programmer not configured", sizeof(result.errorMsg));
        return result;
    }

    // 1. Allocate word buffer on heap (8 KB of uint16_t = 16 KB)
    static uint16_t wordBuf[PIC16F1829_FLASH_WORDS];
    uint32_t maxAddr = 0;

    if (progressCb) progressCb(0, cbCtx);

    // 2. Parse HEX
    if (!parseHex(hexData, hexLen, wordBuf, PIC16F1829_FLASH_WORDS, maxAddr)) {
        strlcpy(result.errorMsg, "HEX parse failed", sizeof(result.errorMsg));
        return result;
    }
    Serial.printf("[ICSP] HEX parsed: %u words (up to 0x%04X)\n", maxAddr, maxAddr - 1);

    if (progressCb) progressCb(5, cbCtx);

    // 3. Enter LVP — from this point the PIC is in reset
    // CommsMaster has already been stopped by the caller before invoking flash()
    Serial1.end();  // Release GPIO 4 from hardware UART
    enterLvp();

    // Verify LVP entry by reading the Device ID at 0x8006
    sendCmd(ICSP_LOAD_CONFIG);
    sendData(0x0000);  // PC -> 0x8000
    for(int i = 0; i < 6; i++) incAddr();  // PC -> 0x8006
    sendCmd(ICSP_READ_DATA_PROG);
    uint16_t devId = recvData() & 0x3FFF;
    Serial.printf("[ICSP] Device ID: 0x%04X\n", devId);

    if (devId == 0x0000 || devId == 0x3FFF) {
        strlcpy(result.errorMsg, "LVP entry failed (HVP needed? Bad wiring?)", sizeof(result.errorMsg));
        exitLvp();
        Serial1.begin(9600, SERIAL_8N1, _pinDat, _pinDat);
        gpio_pullup_en((gpio_num_t)_pinDat);
        return result;
    }

    // 4. Bulk erase
    bulkErase();
    Serial.println("[ICSP] Bulk erase done");
    if (progressCb) progressCb(10, cbCtx);

    // 5. Reset PC to 0 (LOAD_CONFIG moved PC to config space)
    resetToAddr0();

    // 6. Write words
    uint32_t totalWords = maxAddr;
    for (uint32_t addr = 0; addr < totalWords; addr++) {
        writeProgramWord(wordBuf[addr]);
        if (addr + 1 < totalWords) incAddr();

        if (progressCb && (addr % 64 == 0)) {
            // Progress from 10..80 during write phase
            uint8_t pct = 10 + (uint8_t)(70UL * addr / totalWords);
            progressCb(pct, cbCtx);
        }
    }
    result.wordsWritten = totalWords;
    Serial.printf("[ICSP] Wrote %u words\n", totalWords);
    if (progressCb) progressCb(80, cbCtx);

    // 7. Verify
    resetToAddr0();
    uint32_t mismatches = 0;
    for (uint32_t addr = 0; addr < totalWords; addr++) {
        sendCmd(ICSP_READ_DATA_PROG);
        uint16_t read = recvData();
        // Mask to 14 bits (PIC14 instruction width)
        if ((read & 0x3FFF) != (wordBuf[addr] & 0x3FFF)) {
            mismatches++;
            if (mismatches == 1) {
                snprintf(result.errorMsg, sizeof(result.errorMsg),
                         "Verify mismatch at 0x%04X: wrote 0x%04X read 0x%04X",
                         addr, wordBuf[addr] & 0x3FFF, read & 0x3FFF);
            }
        }
        if (addr + 1 < totalWords) incAddr();

        if (progressCb && (addr % 64 == 0)) {
            uint8_t pct = 80 + (uint8_t)(15UL * addr / totalWords);
            progressCb(pct, cbCtx);
        }
    }
    result.wordsVerified = totalWords;

    // 8. Exit LVP
    exitLvp();

    if (progressCb) progressCb(98, cbCtx);

    // 9. Re-initialise Serial1 so CommsMaster can resume
    // The caller must call comms.begin() after this returns.
    Serial1.begin(9600, SERIAL_8N1, _pinDat, _pinDat);
    gpio_pullup_en((gpio_num_t)_pinDat);

    if (mismatches > 0) {
        // errorMsg already set on first mismatch
        Serial.printf("[ICSP] Verify FAILED: %u mismatches\n", mismatches);
        result.ok = false;
    } else {
        result.ok = true;
        Serial.println("[ICSP] Verify OK — flash complete");
    }

    if (progressCb) progressCb(100, cbCtx);
    return result;
}

// ── Read-back ─────────────────────────────────────────────────────────────
uint32_t PicProgrammer::readFlash(uint16_t* buf, uint32_t maxWords) {
    if (_pinDat < 0 || _pinClk < 0 || _pinMclr < 0) return 0;

    Serial1.end();
    enterLvp();
    resetToAddr0();

    uint32_t count = 0;
    for (uint32_t addr = 0; addr < maxWords; addr++) {
        sendCmd(ICSP_READ_DATA_PROG);
        buf[addr] = recvData() & 0x3FFF;
        count++;
        if (addr + 1 < maxWords) incAddr();
    }

    exitLvp();
    Serial1.begin(9600, SERIAL_8N1, _pinDat, _pinDat);
    gpio_pullup_en((gpio_num_t)_pinDat);
    return count;
}
