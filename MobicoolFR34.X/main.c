/*
 * main.c - Mobicool FR34/FR40 compressor cooler alternate PIC16F1829 firmware
 *          (because I wanted to lower the minimum setpoint from -10C to -18C)
 *
 * Copyright (C) 2018 Werner Johansson, wj@unifiedengineering.se
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcc_generated_files/mcc.h"
#include "analog.h"
#include "irmcf183.h"
#include "modbus.h"
#include "settings.h"
#include "display.h"
#include "tm1620b.h"

typedef enum {
    COMP_LOCKOUT = 0,
    COMP_OFF,
    COMP_STARTING,
    COMP_RUN,
} comp_state_t;

typedef enum {
    BMON_WILDCARD = 0,
    BMON_12V,
    BMON_24V
} bmon_volt_t;

typedef struct {
    bmon_t level;
    bmon_volt_t supply;
    int16_t cutout;
    int16_t restart;
} battlevel_t;

#define THRESH_12V_24V (170) // Over 17.0V == 24V system, below == 12V system

static const battlevel_t levels[] = {
    { BMON_DIS, BMON_WILDCARD, 96, 109 }, // Not quite disabled, but the system won't work at lower levels anyway
    { BMON_LOW, BMON_12V, 101, 111 },
    { BMON_MED, BMON_12V, 114, 122 },
    { BMON_HIGH, BMON_12V, 118, 126 },
    { BMON_LOW, BMON_24V, 215, 230 },
    { BMON_MED, BMON_24V, 241, 253 },
    { BMON_HIGH, BMON_24V, 246, 262 },
};
#define NUM_BMON_LEVELS (sizeof(levels) / sizeof(levels[0]))

void main(void) {
    SYSTEM_Initialize();

    IO_LightEna_SetHigh();
    TM1620B_Init();
    TM1620B_Update((uint8_t[]){0, c_U, c_E, c_o, c_S});

    __delay_ms(200);
    Display_Initialize();
    Compressor_Init();
    Modbus_Initialize();
    __delay_ms(1800);

    // Initialize settings
    settings_t settings;
    Settings_Initialize(&settings);

    // Initialize display context
    display_context_t display = {
        .state = DISP_IDLE,
        .on = settings.on,
        .temp_setpoint = settings.temp_setpoint,
        .fahrenheit = settings.fahrenheit,
        .battmon = settings.battmon,
        .temp_setpoint10 = settings.temp_setpoint * 10
    };
    
    // Initialize control variables
    uint8_t lastkeys = 0;
    uint16_t seconds = 0;
    uint8_t comp_timer = 20;
    uint8_t comp_speed = 0;
    comp_state_t compstate = COMP_LOCKOUT;
    uint8_t longpress = 0;

    // Temperature management
    int16_t temperature10 = display.temperature10;
    int16_t temp_setpoint10 = display.temp_setpoint10;
    int16_t last_temp = display.last_temp;
    int16_t temp_rate = 0;

    // Initialize setting change variables
    display.newon = display.on;
    display.newtemp = display.temp_setpoint;
    display.newfahrenheit = display.fahrenheit;
    display.newbattmon = display.battmon;

    // Average temperature variables
    int16_t tempacc = 0;
    uint8_t numtemps = 0;
    int16_t temp_rate_tick = 0;

    // Average voltage variables
    uint32_t voltacc = 0;
    uint8_t numvolts = 0;
    
    // Initial readings
    AnalogUpdate();
    display.temperature10 = AnalogGetTemperature10();
    display.last_temp = display.temperature10;
    display.battlow = false;
    
    while (1) {
        bool compressor_check = false;

        if (TMR1_HasOverflowOccured()) {
            TMR1_Reload();
            PIR1bits.TMR1IF = 0;
            seconds++;
            compressor_check = true;
            Display_TimerTick(&display);
        }

        // Process Modbus communications
        Modbus_Process();
        
        AnalogUpdate();
        // Average temperature a bit more
        tempacc += AnalogGetTemperature10();
        numtemps++;
        if (numtemps == 64) {
            temperature10 = (tempacc + 32) >> 6;
            tempacc = numtemps = 0;
        }
        uint16_t voltage = AnalogGetVoltage();

        // Average voltage some more for battery monitor
        voltacc += voltage;
        numvolts++;
        if (numvolts == 64) {
            uint16_t volt = (voltacc + 32) >> 6;
            volt = (volt + 50) / 100; // Scale to tenths of Volts
            bmon_volt_t supply = (volt > THRESH_12V_24V) ? BMON_24V : BMON_12V;
            for (uint8_t i = 0; i < NUM_BMON_LEVELS; i++) {
                if (levels[i].level == display.battmon &&
                    (levels[i].supply == BMON_WILDCARD || levels[i].supply == supply)) {
                    if (volt < levels[i].cutout && !display.battlow) {
                        display.battlow = true;
                        Compressor_OnOff(false, false, 0);
                        comp_timer = 20;
                        compstate = COMP_LOCKOUT;
                    } else if (volt > levels[i].restart && display.battlow) {
                        display.battlow = false;
                    }
                    break;
                }
            }
            voltacc = numvolts = 0;
        }

        if (display.battlow) compressor_check = false;

        // Update measurements
        uint16_t fancurrent = AnalogGetFanCurrent();

        uint8_t keys = TM1620B_GetKeys();
        uint8_t pressed_keys = keys & ~lastkeys;

        bool comp_on = Compressor_IsOn();
        
        if (compressor_check) {
            uint8_t min = Compressor_GetMinSpeedIdx();
            uint8_t max = Compressor_GetMaxSpeedIdx();
            uint8_t speedidx = 0;
            static uint8_t fanspin = 0;
            int16_t tempdiff = (temperature10 - temp_setpoint10);
            if (comp_timer > 0) {
                comp_timer--;
                if (comp_timer == 0) compstate++;
            }
            if (fanspin > 0) fanspin--;
            switch (compstate) {
                case COMP_LOCKOUT:
                    // Make sure compressor isn't cycling too fast
                    Compressor_OnOff(false, fanspin > 0, 0); // Stopped
                    break;
                case COMP_OFF:
                    if (tempdiff >= 1 && comp_timer == 0) { // 0.1C above setpoint
                        comp_timer = 2;
                        fanspin = 2;
                    }
                    Compressor_OnOff(false, fanspin > 0, 0); // Stopped
                    break;
                case COMP_STARTING:
                    // Use Modbus compressor power if set, otherwise use temperature-based control
                    uint8_t modbus_power = Modbus_GetCompressorPower();
                    uint8_t max_power = Modbus_GetMaxPowerLimit();
                    if (modbus_power > 0 && max_power > 0) {
                        // Scale speed based on max power limit
                        uint32_t maxSpeed = (20UL * max_power) / 100;
                        speedidx = (uint8_t)((modbus_power * maxSpeed) / 100);
                    } else {
                        speedidx = (temp_setpoint10 > 0) ? Compressor_GetMinSpeedIdx() : Compressor_GetDefaultSpeedIdx();
                    }
                    Compressor_OnOff(true, true, speedidx);
                    if (comp_timer == 0) {
                        temp_rate_tick = 0;
                        temp_rate = 0;
                        last_temp = temperature10;
                        comp_timer = 30;
                    }
                    break;
                case COMP_RUN:
                    // Use Modbus compressor power if set
                    modbus_power = Modbus_GetCompressorPower();
                    max_power = Modbus_GetMaxPowerLimit();
                    if (modbus_power > 0 && max_power > 0) {
                        // Scale speed based on max power limit
                        uint32_t maxSpeed = (20UL * max_power) / 100;
                        speedidx = (uint8_t)((modbus_power * maxSpeed) / 100);
                    } else {
                        // Use normal temperature control if no Modbus power set
                        speedidx = comp_speed;
                        temp_rate_tick++;
                        if (temp_rate_tick == 60) {
                            temp_rate = temperature10 - last_temp;
                            // Original temperature control logic preserved
                if (tempdiff > 100 && AnalogGetCompPower() < 45) {
                                speedidx = max;
                            } else if (tempdiff > 40) {
                                if (temp_rate > -5 && speedidx < max) {
                                    speedidx++;
                                } else if (temp_rate < -5 && speedidx > min) {
                                    speedidx--;
                                }
                            } else {
                                if (temp_rate > -1 && speedidx < max) {
                                    speedidx++;
                                } else if (temp_rate < -1 && speedidx > min) {
                                    speedidx--;
                                }
                            }
                            temp_rate_tick = 0;
                            last_temp = temperature10;
                        }
                    }
                if (AnalogGetCompPower() > 45 && speedidx > min) {
                        speedidx--;
                    }
                    if (tempdiff <= 0) {
                        compstate = COMP_LOCKOUT;
                        comp_timer = 99;
                        fanspin = 120;
                        temp_rate = 0;
                    } else {
                        Compressor_OnOff(true, true, speedidx);
                    }
                    break;
            }
            comp_speed = speedidx;
        }
        
        // Update display context with latest measurements and state
        display.voltage = voltage;
        display.fancurrent = fancurrent;
        display.comppower = AnalogGetCompPower();
        display.comp_timer = comp_timer;
        display.comp_speed = comp_speed;
        display.comp_on = comp_on;
        display.temperature10 = temperature10;
        display.last_temp = last_temp;
        display.temp_rate = temp_rate;

        // Handle key presses and update display
        Display_HandleKeyPress(&display, pressed_keys);
        Display_Update(&display, pressed_keys);

        // Update on/off control
        if (display.on) {
            IO_LightEna_SetHigh();
        } else {
            IO_LightEna_SetLow();
            pressed_keys = 0;
            compressor_check = false;
        }

        // Update long press detection for on/off
        if (keys & KEY_ONOFF) {
            if (longpress <= 20) longpress++;
            if (longpress == 20) {
                display.newon = !display.on;
                display.state = DISP_IDLE;
                if (display.newon) {
                    display.idletimer = 0;
                    display.dimtimer = 0;
                } else {
                    Compressor_OnOff(false, false, 0);
                    comp_timer = 20;
                    compstate = COMP_LOCKOUT;
                }
            }
        } else {
            longpress = 0;
        }

        if (display.state == DISP_IDLE) { // Perform housekeeping if we need to update settings
            if (display.newon != display.on) {
                display.on = display.newon;
                Settings_SaveOnOff(display.on);
            }
            // Update temperature setpoint from Modbus if changed
            int16_t modbus_temp = Modbus_GetTargetTemperature() / 10;
            if (modbus_temp >= MIN_TEMP && modbus_temp <= MAX_TEMP) {
                display.newtemp = modbus_temp;
            }
            if (display.newtemp != display.temp_setpoint) {
                display.temp_setpoint = display.newtemp;
                display.temp_setpoint10 = display.newtemp * 10;
                temp_setpoint10 = display.temp_setpoint10; // Update local copy
                Settings_SaveTemp(display.temp_setpoint);
                Modbus_SetTargetTemperature(display.temp_setpoint10);
            }
            if (display.newfahrenheit != display.fahrenheit) {
                display.fahrenheit = display.newfahrenheit;
                Settings_SaveUnit(display.fahrenheit);
            }
            if (display.newbattmon != display.battmon) {
                display.battmon = display.newbattmon;
                Settings_SaveBattMon(display.battmon);
            }
        }

        lastkeys = keys;
    }
}
