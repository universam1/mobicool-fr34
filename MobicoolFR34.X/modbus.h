#ifndef MODBUS_H
#define MODBUS_H

#include <xc.h>

// Function codes
#define MODBUS_FC_READ_HOLDING_REGISTERS    0x03
#define MODBUS_FC_WRITE_SINGLE_REGISTER    0x06

// Register addresses
#define REG_CURRENT_TEMP      0x0000  // Current temperature (read-only)
#define REG_TARGET_TEMP       0x0001  // Target temperature (read/write)
#define REG_VOLTAGE          0x0002  // Input voltage (read-only)
#define REG_FAN_CURRENT      0x0003  // Fan current (read-only)
#define REG_COMP_POWER       0x0004  // Compressor power (read/write)
#define REG_COMP_POWER_MAX   0x0005  // Compressor maximum power limit (read/write)

// Modbus settings
#define MODBUS_ADDRESS       0x01    // Slave address
#define MODBUS_BUFFER_SIZE   256     // Maximum buffer size
#define MODBUS_BAUD_RATE    9600    // Baud rate

// Software UART pins
#define MODBUS_TX_PIN       LATAbits.LATA5  // RA5 for TX
#define MODBUS_TX_TRIS      TRISAbits.TRISA5
#define MODBUS_RX_PIN       PORTCbits.RC7   // RC7 for RX
#define MODBUS_RX_TRIS      TRISCbits.TRISC7

void Modbus_Initialize(void);
void Modbus_Process(void);
void Modbus_HandleRequest(void);

// Set target temperature (called by Modbus handler)
void Modbus_SetTargetTemperature(int16_t temp);

// Get target temperature (for main control loop)
int16_t Modbus_GetTargetTemperature(void);

// Get/Set compressor power (0-100%)
uint8_t Modbus_GetCompressorPower(void);
void Modbus_SetCompressorPower(uint8_t power);

// Get/Set compressor maximum power limit (0-100%)
uint8_t Modbus_GetMaxPowerLimit(void);

#endif // MODBUS_H
