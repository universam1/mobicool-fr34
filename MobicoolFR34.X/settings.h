#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// EEPROM data locations
typedef enum {
    EE_MAGIC = 0,
    EE_ONOFF,
    EE_TEMP,
    EE_UNIT,
    EE_BATTMON,
} eedata_t;

// Temperature limits
#define MAX_TEMP (10)
#define MIN_TEMP (-18)
#define DEFAULT_TEMP MAX_TEMP
// System constants
#define AVERAGING_SAMPLES 64
#define TEMPERATURE_OFFSET 32
#define MIN_VALID_TEMP (-150)  // -15.0°C
#define MAX_VALID_TEMP 500     // 50.0°C
#define VOLTAGE_HYSTERESIS 5   // 0.5V hysteresis for battery protection
#define COMP_START_DELAY 2     // Compressor start delay in seconds
#define COMP_MIN_RUN_TIME 30   // Minimum compressor run time in seconds
#define COMP_LOCKOUT_TIME 99   // Compressor lockout time in seconds
#define FAN_SPINDOWN_TIME 120  // Fan spindown time in seconds
#define LONG_PRESS_TIME 20     // Long press detection time in 100ms units
#define HIGH_POWER_THRESHOLD 45 // High power threshold for compressor speed reduction


typedef enum {
    BMON_DIS = 0,
    BMON_LOW,
    BMON_MED,
    BMON_HIGH
} bmon_t;

// Settings structure
typedef struct {
    bool on;
    int8_t temp_setpoint;
    bool fahrenheit;
    bmon_t battmon;
} settings_t;

// Initialize settings from EEPROM
void Settings_Initialize(settings_t* settings);

// Save individual settings to EEPROM
void Settings_SaveOnOff(bool on);
void Settings_SaveTemp(int8_t temp);
void Settings_SaveUnit(bool fahrenheit);
void Settings_SaveBattMon(bmon_t level);

#endif /* SETTINGS_H */
