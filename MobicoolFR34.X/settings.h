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
