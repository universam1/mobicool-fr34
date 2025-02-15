#include "settings.h"
#include "mcc_generated_files/memory.h"

#define MAGIC ('W')

void Settings_Initialize(settings_t* settings) {
    bool eeinvalid = false;

    // Check if EEPROM data is valid
    if (DATAEE_ReadByte(EE_MAGIC) != MAGIC) {
        eeinvalid = true;
    }
    
    // Read settings
    settings->on = DATAEE_ReadByte(EE_ONOFF);
    settings->temp_setpoint = DATAEE_ReadByte(EE_TEMP);
    if (settings->temp_setpoint < MIN_TEMP || settings->temp_setpoint > MAX_TEMP) {
        eeinvalid = true;
    }
    settings->fahrenheit = DATAEE_ReadByte(EE_UNIT);
    settings->battmon = DATAEE_ReadByte(EE_BATTMON);
    if (settings->battmon > BMON_HIGH) {
        eeinvalid = true;
    }

    // Initialize with defaults if invalid
    if (eeinvalid) {
        settings->on = true;
        DATAEE_WriteByte(EE_ONOFF, settings->on);
        
        settings->temp_setpoint = DEFAULT_TEMP;
        DATAEE_WriteByte(EE_TEMP, settings->temp_setpoint);
        
        settings->fahrenheit = false;
        DATAEE_WriteByte(EE_UNIT, settings->fahrenheit);
        
        settings->battmon = BMON_LOW;
        DATAEE_WriteByte(EE_BATTMON, settings->battmon);
        
        DATAEE_WriteByte(EE_MAGIC, MAGIC);
    }
}

void Settings_SaveOnOff(bool on) {
    DATAEE_WriteByte(EE_ONOFF, on);
}

void Settings_SaveTemp(int8_t temp) {
    DATAEE_WriteByte(EE_TEMP, temp);
}

void Settings_SaveUnit(bool fahrenheit) {
    DATAEE_WriteByte(EE_UNIT, fahrenheit);
}

void Settings_SaveBattMon(bmon_t level) {
    DATAEE_WriteByte(EE_BATTMON, level);
}
