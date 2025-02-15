#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

// Display states
typedef enum {
    DISP_IDLE = 0,

    DISP_SET_BEGIN,
    DISP_SET_TEMP,     // Set temperature set point
    DISP_SET_UNIT,     // Set temperature unit (C/F)
    DISP_SET_BATTMON,  // Set battery monitor level
    DISP_SET_END,

    DISP_STATUS_BEGIN,
    DISP_VOLT,         // Battery voltage
    DISP_COMPPOWER,    // Compressor power consumption
    DISP_COMPTIMER,    // Compressor timer
    DISP_COMPSPEED,    // Compressor speed %
    DISP_FANCURRENT,   // Fan current
    DISP_TEMPRATE,     // Temperature rate of change
    DISP_STATUS_END,
} display_state_t;

extern uint8_t FormatDigits(uint8_t* buf, int16_t value, uint8_t decimals);

// Display context structure
typedef struct {
    // Display state
    display_state_t state;
    uint8_t flashtimer;
    uint8_t idletimer;
    uint8_t dimtimer;

    // Settings
    bool on;
    int8_t temp_setpoint;
    bool fahrenheit;
    bmon_t battmon;

    // Temperature management
    int16_t temp_setpoint10;
    int16_t temperature10;
    int16_t last_temp;
    int16_t temp_rate;

    // System measurements
    uint16_t voltage;
    uint16_t fancurrent;
    uint8_t comppower;
    uint8_t comp_timer;
    uint8_t comp_speed;
    bool comp_on;
    bool battlow;

    // New settings (to be applied)
    bool newon;
    int8_t newtemp;
    bool newfahrenheit;
    bmon_t newbattmon;
} display_context_t;

// Display brightness levels
#define DISPLAY_DEFAULT_BRIGHT (4)
#define DISPLAY_DIM_BRIGHT (0)

// Initialize display module
void Display_Initialize(void);

// Update display based on current state and context
void Display_Update(display_context_t* ctx, uint8_t pressed_keys);

// Timer-based display updates (dimming, idle timeout)
void Display_TimerTick(display_context_t* ctx);

// Handle key press events
void Display_HandleKeyPress(display_context_t* ctx, uint8_t pressed_keys);

// Get LED status
uint8_t Display_GetLEDs(display_context_t* ctx);

#endif /* DISPLAY_H */
