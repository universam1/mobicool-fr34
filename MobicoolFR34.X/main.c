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

// Temperature averaging buffer size
#define AVERAGING_SAMPLES 64
#define TEMPERATURE_OFFSET 32

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

typedef struct {
    comp_state_t state;
    uint8_t timer;
    uint8_t speed;
    uint8_t fanspin;
    bool running;
} compressor_context_t;

typedef struct {
    int16_t temperature10;
    int16_t temp_setpoint10;
    int16_t last_temp;
    int16_t temp_rate;
    int16_t tempacc;
    uint8_t numtemps;
    int16_t temp_rate_tick;
} temp_context_t;

typedef struct {
    uint32_t voltacc;
    uint8_t numvolts;
    bool battlow;
} battery_context_t;

#define THRESH_12V_24V (170) // Over 17.0V == 24V system, below == 12V system

static const battlevel_t levels[] = {
    { BMON_DIS, BMON_WILDCARD, 96, 109 }, // Not quite disabled, but system won't work at lower levels
    { BMON_LOW, BMON_12V, 101, 111 },
    { BMON_MED, BMON_12V, 114, 122 },
    { BMON_HIGH, BMON_12V, 118, 126 },
    { BMON_LOW, BMON_24V, 215, 230 },
    { BMON_MED, BMON_24V, 241, 253 },
    { BMON_HIGH, BMON_24V, 246, 262 },
};
#define NUM_BMON_LEVELS (sizeof(levels) / sizeof(levels[0]))

// Function declarations
static void system_init(display_context_t* display);
static void update_temperature(temp_context_t* temp);
static bool update_battery(battery_context_t* battery, display_context_t* display, compressor_context_t* comp);
static uint8_t calculate_compressor_speed(compressor_context_t* comp, temp_context_t* temp);
static void update_compressor_state(compressor_context_t* comp, temp_context_t* temp, bool check_enabled);
static void handle_key_press(uint8_t keys, uint8_t* lastkeys, uint8_t* longpress, display_context_t* display, compressor_context_t* comp);
static void update_settings(display_context_t* display, int16_t* temp_setpoint10);

static void system_init(display_context_t* display) {
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
    display->state = DISP_IDLE;
    display->on = settings.on;
    display->temp_setpoint = settings.temp_setpoint;
    display->fahrenheit = settings.fahrenheit;
    display->battmon = settings.battmon;
    display->temp_setpoint10 = settings.temp_setpoint * 10;
    display->newon = display->on;
    display->newtemp = display->temp_setpoint;
    display->newfahrenheit = display->fahrenheit;
    display->newbattmon = display->battmon;
    
    // Initial readings
    AnalogUpdate();
    display->temperature10 = AnalogGetTemperature10();
    display->last_temp = display->temperature10;
    display->battlow = false;
}

static void update_temperature(temp_context_t* temp) {
    temp->tempacc += AnalogGetTemperature10();
    temp->numtemps++;
    if (temp->numtemps == AVERAGING_SAMPLES) {
        temp->temperature10 = (temp->tempacc + TEMPERATURE_OFFSET) >> 6;
        temp->tempacc = temp->numtemps = 0;
    }
}

static bool update_battery(battery_context_t* battery, display_context_t* display, compressor_context_t* comp) {
    uint16_t voltage = AnalogGetVoltage();
    battery->voltacc += voltage;
    battery->numvolts++;
    
    if (battery->numvolts == AVERAGING_SAMPLES) {
        uint16_t volt = (uint16_t)((battery->voltacc + TEMPERATURE_OFFSET) >> 6);
        volt = (volt + 50) / 100; // Scale to tenths of Volts
        bmon_volt_t supply = (volt > THRESH_12V_24V) ? BMON_24V : BMON_12V;
        
        for (uint8_t i = 0; i < NUM_BMON_LEVELS; i++) {
            if (levels[i].level == display->battmon &&
                (levels[i].supply == BMON_WILDCARD || levels[i].supply == supply)) {
                if (volt < levels[i].cutout && !display->battlow) {
                    display->battlow = true;
                    Compressor_OnOff(false, false, 0);
                    comp->timer = 20;
                    comp->state = COMP_LOCKOUT;
                } else if (volt > levels[i].restart && display->battlow) {
                    display->battlow = false;
                }
                break;
            }
        }
        battery->voltacc = battery->numvolts = 0;
        return true;
    }
    return false;
}

static uint8_t calculate_compressor_speed(compressor_context_t* comp, temp_context_t* temp) {
    uint8_t min = Compressor_GetMinSpeedIdx();
    uint8_t max = Compressor_GetMaxSpeedIdx();
    uint8_t speedidx = comp->speed;
    
    uint8_t modbus_power = Modbus_GetCompressorPower();
    uint8_t max_power = Modbus_GetMaxPowerLimit();
    
    if (modbus_power > 0 && max_power > 0) {
        // Scale speed based on max power limit
        uint32_t maxSpeed = (20UL * max_power) / 100;
        speedidx = (uint8_t)((modbus_power * maxSpeed) / 100);
    } else {
        int16_t tempdiff = (temp->temperature10 - temp->temp_setpoint10);
        
        if (comp->state == COMP_STARTING) {
            speedidx = (temp->temp_setpoint10 > 0) ? min : Compressor_GetDefaultSpeedIdx();
        } else if (comp->state == COMP_RUN) {
            temp->temp_rate_tick++;
            if (temp->temp_rate_tick == 60) {
                temp->temp_rate = temp->temperature10 - temp->last_temp;
                
                if (tempdiff > 100 && AnalogGetCompPower() < 45) {
                    speedidx = max;
                } else if (tempdiff > 40) {
                    if (temp->temp_rate > -5 && speedidx < max) speedidx++;
                    else if (temp->temp_rate < -5 && speedidx > min) speedidx--;
                } else {
                    if (temp->temp_rate > -1 && speedidx < max) speedidx++;
                    else if (temp->temp_rate < -1 && speedidx > min) speedidx--;
                }
                
                temp->temp_rate_tick = 0;
                temp->last_temp = temp->temperature10;
            }
            
            if (AnalogGetCompPower() > 45 && speedidx > min) {
                speedidx--;
            }
        }
    }
    
    return speedidx;
}

static void update_compressor_state(compressor_context_t* comp, temp_context_t* temp, bool check_enabled) {
    if (!check_enabled) return;

    int16_t tempdiff = (temp->temperature10 - temp->temp_setpoint10);
    
    if (comp->timer > 0) {
        comp->timer--;
        if (comp->timer == 0) comp->state++;
    }
    
    if (comp->fanspin > 0) comp->fanspin--;
    
    switch (comp->state) {
        case COMP_LOCKOUT:
            Compressor_OnOff(false, comp->fanspin > 0, 0);
            break;
            
        case COMP_OFF:
            if (tempdiff >= 1 && comp->timer == 0) {
                comp->timer = 2;
                comp->fanspin = 2;
            }
            Compressor_OnOff(false, comp->fanspin > 0, 0);
            break;
            
        case COMP_STARTING:
            comp->speed = calculate_compressor_speed(comp, temp);
            Compressor_OnOff(true, true, comp->speed);
            if (comp->timer == 0) {
                temp->temp_rate_tick = 0;
                temp->temp_rate = 0;
                temp->last_temp = temp->temperature10;
                comp->timer = 30;
            }
            break;
            
        case COMP_RUN:
            comp->speed = calculate_compressor_speed(comp, temp);
            if (tempdiff <= 0) {
                comp->state = COMP_LOCKOUT;
                comp->timer = 99;
                comp->fanspin = 120;
                temp->temp_rate = 0;
            } else {
                Compressor_OnOff(true, true, comp->speed);
            }
            break;
    }
}

static void handle_key_press(uint8_t keys, uint8_t* lastkeys, uint8_t* longpress, display_context_t* display, compressor_context_t* comp) {
    uint8_t pressed_keys = keys & ~(*lastkeys);
    
    if (keys & KEY_ONOFF) {
        if (*longpress <= 20) (*longpress)++;
        if (*longpress == 20) {
            display->newon = !display->on;
            display->state = DISP_IDLE;
            if (display->newon) {
                display->idletimer = 0;
                display->dimtimer = 0;
            } else {
                Compressor_OnOff(false, false, 0);
                comp->timer = 20;
                comp->state = COMP_LOCKOUT;
            }
        }
    } else {
        *longpress = 0;
    }
    
    Display_HandleKeyPress(display, pressed_keys);
    Display_Update(display, pressed_keys);
    *lastkeys = keys;
}

static void update_settings(display_context_t* display, int16_t* temp_setpoint10) {
    if (display->state != DISP_IDLE) return;
    
    if (display->newon != display->on) {
        display->on = display->newon;
        Settings_SaveOnOff(display->on);
    }
    
    int16_t modbus_temp = Modbus_GetTargetTemperature() / 10;
    if (modbus_temp >= MIN_TEMP && modbus_temp <= MAX_TEMP) {
        display->newtemp = (int8_t)modbus_temp;
    }
    
    if (display->newtemp != display->temp_setpoint) {
        display->temp_setpoint = display->newtemp;
        display->temp_setpoint10 = display->newtemp * 10;
        *temp_setpoint10 = display->temp_setpoint10;
        Settings_SaveTemp(display->temp_setpoint);
        Modbus_SetTargetTemperature(display->temp_setpoint10);
    }
    
    if (display->newfahrenheit != display->fahrenheit) {
        display->fahrenheit = display->newfahrenheit;
        Settings_SaveUnit(display->fahrenheit);
    }
    
    if (display->newbattmon != display->battmon) {
        display->battmon = display->newbattmon;
        Settings_SaveBattMon(display->battmon);
    }
}

void main(void) {
    display_context_t display = {0};
    temp_context_t temp = {0};
    battery_context_t battery = {0};
    compressor_context_t comp = {
        .state = COMP_LOCKOUT,
        .timer = 20,
        .speed = 0,
        .fanspin = 0,
        .running = false
    };
    
    uint8_t lastkeys = 0;
    uint8_t longpress = 0;
    uint16_t seconds = 0;
    
    system_init(&display);
    
    temp.temperature10 = display.temperature10;
    temp.temp_setpoint10 = display.temp_setpoint10;
    temp.last_temp = display.last_temp;
    
    while (1) {
        bool compressor_check = false;

        if (TMR1_HasOverflowOccured()) {
            TMR1_Reload();
            PIR1bits.TMR1IF = 0;
            seconds++;
            compressor_check = true;
            Display_TimerTick(&display);
        }

        Modbus_Process();
        AnalogUpdate();
        
        update_temperature(&temp);
        if (update_battery(&battery, &display, &comp)) {
            compressor_check &= !display.battlow;
        }

        uint8_t keys = TM1620B_GetKeys();
        handle_key_press(keys, &lastkeys, &longpress, &display, &comp);
        
        comp.running = Compressor_IsOn();
        update_compressor_state(&comp, &temp, compressor_check);
        
        // Update display context with latest measurements and state
        display.voltage = AnalogGetVoltage();
        display.fancurrent = AnalogGetFanCurrent();
        display.comppower = AnalogGetCompPower();
        display.comp_timer = comp.timer;
        display.comp_speed = comp.speed;
        display.comp_on = comp.running;
        display.temperature10 = temp.temperature10;
        display.last_temp = temp.last_temp;
        display.temp_rate = temp.temp_rate;

        // Update on/off control
        if (display.on) {
            IO_LightEna_SetHigh();
        } else {
            IO_LightEna_SetLow();
            compressor_check = false;
        }

        update_settings(&display, &temp.temp_setpoint10);
    }
}
