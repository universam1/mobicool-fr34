#include "modbus.h"
#include "mcc_generated_files/tmr1.h"
#include "analog.h"

// Timer0 is used for bit timing in software UART
// For 9600 baud with 1MHz clock:
// Bit time = 1000000/9600 = ~104µs
// With 1MHz clock and 1:4 prescaler, each TMR0 tick is 4µs
// So we need 26 TMR0 ticks per bit
#define BIT_TIME 26
#define HALF_BIT_TIME 13

static void Timer0_Initialize(void) {
    // Configure Timer0 for software UART timing
    // Clock source is FOSC/4
    OPTION_REGbits.TMR0CS = 0;
    // Assign prescaler to Timer0
    OPTION_REGbits.PSA = 0;
    // 1:4 prescaler
    OPTION_REGbits.PS = 0b001;
    TMR0 = 0;
}

static volatile uint8_t rxBuffer[MODBUS_BUFFER_SIZE];
static uint8_t txBuffer[MODBUS_BUFFER_SIZE];
static uint8_t rxIndex = 0;
static int16_t targetTemperature = 50;  // Default 5.0°C
static uint8_t compressorPower = 0;     // Default 0%
static uint8_t compressorMaxPower = 100;// Default 100%

// CRC lookup table
static const uint16_t crcTable[] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440
};

static uint16_t ModbusCRC16(uint8_t *buffer, uint8_t length) {
    uint16_t crc = 0xFFFF;
    while (length--) {
        crc = (crc >> 4) ^ crcTable[(crc ^ *buffer) & 0x0F];
        crc = (crc >> 4) ^ crcTable[(crc ^ (*buffer++ >> 4)) & 0x0F];
    }
    return crc;
}

// Software UART functions
static void ModbusUART_Initialize(void) {
    MODBUS_TX_TRIS = 0;  // TX pin as output
    MODBUS_RX_TRIS = 1;  // RX pin as input
    MODBUS_TX_PIN = 1;   // TX idle state is high
}

static void ModbusUART_TransmitByte(uint8_t data) {
    uint16_t tmpTMR0 = TMR0;
    
    // Start bit
    MODBUS_TX_PIN = 0;
    tmpTMR0 += BIT_TIME;
    while(TMR0 < tmpTMR0);
    
    // Data bits
    for(uint8_t i = 0; i < 8; i++) {
        MODBUS_TX_PIN = (data & 0x01) ? 1 : 0;
        data >>= 1;
        tmpTMR0 += BIT_TIME;
        while(TMR0 < tmpTMR0);
    }
    
    // Stop bit
    MODBUS_TX_PIN = 1;
    tmpTMR0 += BIT_TIME;
    while(TMR0 < tmpTMR0);
}

static bool ModbusUART_ReceiveByte(uint8_t *data) {
    uint8_t tmpData = 0;
    uint16_t tmpTMR0;
    
    // Wait for start bit
    if(MODBUS_RX_PIN == 1) return false;
    
    tmpTMR0 = TMR0 + HALF_BIT_TIME; // Move to middle of start bit
    while(TMR0 < tmpTMR0);
    
    if(MODBUS_RX_PIN == 1) return false; // Verify start bit
    
    tmpTMR0 += BIT_TIME; // Move to middle of first data bit
    
    // Read 8 data bits
    for(uint8_t i = 0; i < 8; i++) {
        while(TMR0 < tmpTMR0);
        tmpData >>= 1;
        if(MODBUS_RX_PIN) tmpData |= 0x80;
        tmpTMR0 += BIT_TIME;
    }
    
    // Wait for stop bit
    while(TMR0 < tmpTMR0);
    if(MODBUS_RX_PIN == 0) return false; // Invalid stop bit
    
    *data = tmpData;
    return true;
}

void Modbus_Initialize(void) {
    Timer0_Initialize();
    ModbusUART_Initialize();
    rxIndex = 0;
}

void Modbus_Process(void) {
    uint8_t rcvByte;
    if (ModbusUART_ReceiveByte(&rcvByte)) {
        rxBuffer[rxIndex] = rcvByte;
        
        // Basic Modbus RTU framing - wait for 3.5 character times between frames
        // At 9600 baud, one character is ~1ms, so we'll use TMR1 for timeout
        TMR1_Reload();
        
        if (rxIndex < MODBUS_BUFFER_SIZE - 1) {
            rxIndex++;
        }
    }
    
    // Check for frame timeout (3.5 char times)
    if (rxIndex > 0 && TMR1_HasOverflowOccured()) {
        Modbus_HandleRequest();
        rxIndex = 0;
    }
}

static void Modbus_SendResponse(uint8_t *buffer, uint8_t length) {
    uint16_t crc = ModbusCRC16(buffer, length);
    buffer[length] = crc & 0xFF;
    buffer[length + 1] = (crc >> 8) & 0xFF;
    
    for (uint8_t i = 0; i < length + 2; i++) {
        ModbusUART_TransmitByte(buffer[i]);
    }
}

void Modbus_HandleRequest(void) {
    if (rxIndex < 4) return; // Too short for valid frame
    
    // Verify CRC
    uint16_t receivedCrc = (rxBuffer[rxIndex - 1] << 8) | rxBuffer[rxIndex - 2];
    uint16_t calculatedCrc = ModbusCRC16(rxBuffer, rxIndex - 2);
    if (receivedCrc != calculatedCrc) return;
    
    // Check if message is for us
    if (rxBuffer[0] != MODBUS_ADDRESS) return;
    
    uint8_t function = rxBuffer[1];
    uint16_t address = (rxBuffer[2] << 8) | rxBuffer[3];
    
    switch (function) {
        case MODBUS_FC_READ_HOLDING_REGISTERS: {
            uint16_t quantity = (rxBuffer[4] << 8) | rxBuffer[5];
            if (quantity > 5) break; // Maximum 5 registers
            
            txBuffer[0] = MODBUS_ADDRESS;
            txBuffer[1] = MODBUS_FC_READ_HOLDING_REGISTERS;
            txBuffer[2] = quantity * 2; // Number of bytes
            
            uint8_t byteIndex = 3;
            for (uint16_t i = 0; i < quantity; i++) {
                int16_t value = 0;
                switch (address + i) {
                    case REG_CURRENT_TEMP:
                        value = AnalogGetTemperature10();
                        break;
                    case REG_TARGET_TEMP:
                        value = targetTemperature;
                        break;
                    case REG_VOLTAGE:
                         value = (int16_t)AnalogGetVoltage();
                        break;
                    case REG_FAN_CURRENT:
                         value = (int16_t)AnalogGetFanCurrent();
                        break;
                    case REG_COMP_POWER:
                        value = Modbus_GetCompressorPower();
                        break;
                    case REG_COMP_POWER_MAX:
                        value = compressorMaxPower;
                        break;
                }
                txBuffer[byteIndex++] = (value >> 8) & 0xFF;
                txBuffer[byteIndex++] = value & 0xFF;
            }
            
            Modbus_SendResponse(txBuffer, byteIndex);
            break;
        }
        
case MODBUS_FC_WRITE_SINGLE_REGISTER: {
            if (address == REG_TARGET_TEMP) {
                int16_t newTemp = (rxBuffer[4] << 8) | rxBuffer[5];
                targetTemperature = newTemp;
                
                // Echo request as response
                for (uint8_t i = 0; i < 6; i++) {
                    txBuffer[i] = rxBuffer[i];
                }
                Modbus_SendResponse(txBuffer, 6);
            }
            else if (address == REG_COMP_POWER || address == REG_COMP_POWER_MAX) {
                uint16_t newPower = (rxBuffer[4] << 8) | rxBuffer[5];
                if (newPower <= 100) { // Validate 0-100%
                    if (address == REG_COMP_POWER) {
                        Modbus_SetCompressorPower((uint8_t)newPower);
                    } else {
                        compressorMaxPower = (uint8_t)newPower;
                        // Ensure current power doesn't exceed new max
                        if (compressorPower > compressorMaxPower) {
                            compressorPower = compressorMaxPower;
                        }
                    }
                    
                    // Echo request as response
                    for (uint8_t i = 0; i < 6; i++) {
                        txBuffer[i] = rxBuffer[i];
                    }
                    Modbus_SendResponse(txBuffer, 6);
                }
            }
            break;
        }
    }
}

void Modbus_SetTargetTemperature(int16_t temp) {
    targetTemperature = temp;
}

int16_t Modbus_GetTargetTemperature(void) {
    return targetTemperature;
}

uint8_t Modbus_GetCompressorPower(void) {
    return compressorPower;
}

void Modbus_SetCompressorPower(uint8_t power) {
    if (power <= compressorMaxPower) {
        compressorPower = power;
    }
}

uint8_t Modbus_GetMaxPowerLimit(void) {
    return compressorMaxPower;
}
