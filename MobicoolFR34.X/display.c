#include "display.h"
#include "tm1620b.h"
#include "settings.h"
#include "mcc_generated_files/pin_manager.h"
#include "mcc_generated_files/mcc.h"
#include <stddef.h>
#include <xc.h>

void Display_Initialize(void) {
    IO_LightEna_SetHigh();
    TM1620B_SetBrightness(true, DISPLAY_DEFAULT_BRIGHT);
}

uint8_t Display_GetLEDs(display_context_t* ctx) {
    return (uint8_t)((!ctx->comp_on << 7) | (ctx->battlow << 6) | (ctx->comp_on << 4));
}

void Display_TimerTick(display_context_t* ctx) {
    // Handle display timers
    if (ctx->idletimer < 10) {
        ctx->idletimer++;
        if (ctx->idletimer == 10) {
            ctx->state = DISP_IDLE;
        }
    }
    
    if (ctx->dimtimer < 20) {
        ctx->dimtimer++;
        if (ctx->dimtimer == 20) {
            TM1620B_SetBrightness(true, DISPLAY_DIM_BRIGHT);
        }
    }
    
    ctx->flashtimer++;
}

void Display_HandleKeyPress(display_context_t* ctx, uint8_t pressed_keys) {
    if (pressed_keys) {
        ctx->flashtimer = 0; // restart flash timer on every keypress
        ctx->idletimer = 0;
        ctx->dimtimer = 0;
        TM1620B_SetBrightness(true, DISPLAY_DEFAULT_BRIGHT);
    }

    // Handle state transitions
    if (pressed_keys & KEY_ONOFF) {
        if (ctx->state < DISP_STATUS_BEGIN || ctx->state > DISP_STATUS_END) {
            ctx->state = DISP_STATUS_BEGIN;
        }
        ctx->state++;
        if (ctx->state == DISP_STATUS_END) {
            ctx->state = DISP_IDLE;
        }
    }
    
    if (pressed_keys & KEY_SET) {
        if (ctx->state < DISP_SET_BEGIN || ctx->state > DISP_SET_END) {
            ctx->state = DISP_SET_BEGIN;
            ctx->newtemp = ctx->temp_setpoint;
            ctx->newfahrenheit = ctx->fahrenheit;
            ctx->newbattmon = ctx->battmon;
        }
        ctx->state++;
        if (ctx->state == DISP_SET_END) {
            ctx->state = DISP_IDLE;
        }
    }
    
    // Handle setting adjustments
    if (pressed_keys & KEY_MINUS && ctx->state == DISP_SET_TEMP && ctx->newtemp > MIN_TEMP) ctx->newtemp--;
    if (pressed_keys & KEY_PLUS && ctx->state == DISP_SET_TEMP && ctx->newtemp < MAX_TEMP) ctx->newtemp++;
    
    if (pressed_keys & (KEY_PLUS | KEY_MINUS) && ctx->state == DISP_SET_UNIT) {
        ctx->newfahrenheit = !ctx->fahrenheit;
    }
    
    if (ctx->state == DISP_SET_BATTMON) {
        if (pressed_keys & KEY_MINUS && ctx->newbattmon > BMON_DIS) ctx->newbattmon--;
        if (pressed_keys & KEY_PLUS && ctx->newbattmon < BMON_HIGH) ctx->newbattmon++;
    }
}

void Display_Update(display_context_t* ctx, uint8_t pressed_keys) {
    uint8_t leds = Display_GetLEDs(ctx);
    uint8_t buf[5] = {0};
    
    switch (ctx->state) {
        case DISP_VOLT: {
            uint16_t dispvolt = (ctx->voltage + 50) / 100; // decivolt
            uint8_t num = FormatDigits(NULL, (int16_t)dispvolt, 2);
            buf[0] = leds;
            buf[1] = 0;
            FormatDigits(&buf[4 - num], (int16_t)dispvolt, 2);
            buf[3] |= ADD_DOT;
            buf[4] = c_V;
            break;
        }
        case DISP_COMPPOWER: {
            uint8_t tmp[3] = {0};
            FormatDigits(tmp, ctx->comppower, 0);
            buf[0] = leds;
            buf[1] = c_C;
            buf[3] = tmp[0];
            buf[4] = tmp[1];
            break;
        }
        case DISP_COMPTIMER: {
            uint8_t tmp[3] = {0};
            FormatDigits(tmp, ctx->comp_timer, 0);
            buf[0] = leds;
            buf[1] = c_t;
            buf[3] = tmp[0];
            buf[4] = tmp[1];
            break;
        }
        case DISP_COMPSPEED: {
            uint8_t tmp[3] = {0};
            FormatDigits(tmp, ctx->comp_speed * 5, 3); // In percent
            buf[0] = leds;
            buf[1] = c_r;
            buf[2] = tmp[0];
            buf[3] = tmp[1];
            buf[4] = tmp[2];
            break;
        }
        case DISP_FANCURRENT: {
            uint16_t dispamp = (ctx->fancurrent + 50) / 100; // deciamp
            uint8_t num = FormatDigits(NULL, (int16_t)dispamp, 2);
            buf[0] = leds;
            buf[1] = c_F;
            FormatDigits(&buf[4 - num], (int16_t)dispamp, 2);
            buf[3] |= ADD_DOT;
            buf[4] = c_A;
            break;
        }
        case DISP_TEMPRATE: {
            uint8_t num = FormatDigits(NULL, ctx->temp_rate, 2);
            buf[0] = leds;
            buf[1] = c_d;
            buf[2] = 0;
            FormatDigits(&buf[5 - num], ctx->temp_rate, 2);
            buf[4] |= ADD_DOT;
            break;
        }
        case DISP_SET_TEMP: {
            buf[0] = leds;
            buf[4] = ctx->fahrenheit ? c_F : c_C | ADD_DOT;
            if (!(ctx->flashtimer & 0x08)) {
                int8_t disptemp = ctx->fahrenheit ? ((((ctx->newtemp * 9) + 2) / 5) + 32) : ctx->newtemp;
                uint8_t num = FormatDigits(NULL, disptemp, 0);
                FormatDigits(&buf[4 - num], disptemp, 0); // Right justified
            }
            break;
        }
        case DISP_SET_UNIT:
            buf[0] = leds;
            if (!(ctx->flashtimer & 0x08)) {
                buf[4] = (ctx->newfahrenheit ? c_F : c_C) | ADD_DOT;
            }
            break;
        case DISP_SET_BATTMON: {
            buf[0] = leds;
            if (!(ctx->flashtimer & 0x08)) {
                switch (ctx->newbattmon) {
                    case BMON_DIS:
                        buf[2] = c_d;
                        buf[3] = c_i;
                        buf[4] = c_S;
                        break;
                    case BMON_LOW:
                        buf[2] = c_L;
                        buf[3] = c_o;
                        break;
                    case BMON_MED:
                        buf[2] = c_M;
                        buf[3] = c_E;
                        buf[4] = c_d;
                        break;
                    case BMON_HIGH:
                        buf[2] = c_H;
                        buf[3] = c_i;
                        break;
                }
            }
            break;
        }
        case DISP_IDLE: {
            buf[0] = leds;
            buf[4] = ctx->fahrenheit ? c_F : c_C | ADD_DOT;
            bool tenths = true;
            if (ctx->fahrenheit && ctx->temperature10 > 377) tenths = false;
            int16_t disptemp;
            if (tenths) {
                disptemp = ctx->fahrenheit ? ((((ctx->temperature10 * 9) + 2) / 5) + 320) : ctx->temperature10;
            } else {
                int16_t temperature = (ctx->temperature10 + 5) / 10;
                disptemp = ctx->fahrenheit ? ((((temperature * 9) + 2) / 5) + 32) : temperature;
            }
            uint8_t num = FormatDigits(NULL, disptemp, tenths ? 2 : 0);
            FormatDigits(&buf[4 - num], disptemp, tenths ? 2 : 0);
            if (tenths) buf[3] |= ADD_DOT;

            if (!ctx->on) {
                if ((ctx->flashtimer & 0x0f) < 0xa) {
                    buf[1] = buf[2] = buf[3] = buf[4] = 0;
                } else if (ctx->flashtimer & 0x10) {
                    buf[1] = c_o;
                    buf[2] = c_F;
                    buf[3] = c_F;
                    buf[4] = 0;
                }
            } else if (ctx->battlow) {
                if ((ctx->flashtimer & 0x0f) < 0xa) {
                    buf[1] = buf[2] = buf[3] = buf[4] = 0;
                } else if (ctx->flashtimer & 0x10) {
                    buf[1] = c_b;
                    buf[2] = c_A;
                    buf[3] = buf[4] = c_t;
                }
            }
            break;
        }
        default:
            break;
    }
    
    TM1620B_Update(buf);
}
