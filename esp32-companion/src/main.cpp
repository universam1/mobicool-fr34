/**
 * ESP32 companion for Mobicool FR34 cooler
 *
 * Provides a WiFi access point + web dashboard (Vue 3) to remotely
 * monitor and control the cooler via Modbus RTU over the 3-wire UART
 * interface exposed on the PIC16F1829 mainboard.
 *
 * Hardware connections
 * ─────────────────────────────────────────────────────────────────────
 *  PIC pin   Signal      ESP32 GPIO
 *  RA5       MODBUS TX   16  (UART2 RX)
 *  RC7       MODBUS RX   17  (UART2 TX)
 *  GND       GND         GND
 *
 * Both devices operate at 3.3 V; no level-shifter required.
 * See WIRING.md for connector details.
 *
 * WiFi: SSID "FR34-Cooler", open network (no password)
 * Web UI: http://192.168.4.1/
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>

#include "modbus_master.h"
#include "web_ui.h"
#include "vue_js.h"

// ── Configuration ─────────────────────────────────────────────────────────
static constexpr char     AP_SSID[]   = "FR34-Cooler";
static constexpr char     AP_PASS[]   = "";          // open network
static const IPAddress AP_IP      (192, 168, 4, 1);
static const IPAddress AP_SUBNET  (255, 255, 255, 0);

static constexpr int      MB_RX_PIN   = 16;
static constexpr int      MB_TX_PIN   = 17;
static constexpr uint32_t MB_BAUD     = 9600;

static constexpr uint32_t POLL_MS     = 1000;  // Modbus poll interval

// ── Globals ───────────────────────────────────────────────────────────────
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");
ModbusMaster    modbus;
CoolerState     coolerState;

static uint32_t lastPoll = 0;

// ── Build JSON broadcast payload ──────────────────────────────────────────
static String buildJson(const CoolerState& s) {
    JsonDocument doc;
    if (s.valid) {
        // Temperatures converted from tenths-of-°C to float °C
        doc["temp"]        = s.currentTemp10  / 10.0f;
        doc["setpoint"]    = s.targetTemp10   / 10.0f;
        // Voltage: mV → V
        doc["voltage"]     = s.voltageMilliV  / 1000.0f;
        // Fan current: mA → A
        doc["fanCurrent"]  = s.fanCurrentMilliA / 1000.0f;
        doc["compPower"]   = s.compPower;
        doc["compPowerMax"]= s.compPowerMax;
    } else {
        doc["error"] = "modbus_fail";
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ── WebSocket event handler ───────────────────────────────────────────────
static void onWsEvent(AsyncWebSocket* server_ws,
                      AsyncWebSocketClient* client,
                      AwsEventType type,
                      void* arg,
                      uint8_t* data,
                      size_t len)
{
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;

    // Parse command JSON
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) return;

    const char* cmd = doc["cmd"] | "";
    int16_t     val = doc["value"] | 0;

    if      (strcmp(cmd, "setTemp")     == 0) modbus.writeRegister(MB_REG_TARGET_TEMP,  (uint16_t)val);
    else if (strcmp(cmd, "setPower")    == 0) modbus.writeRegister(MB_REG_COMP_POWER,   (uint16_t)constrain(val, 0, 100));
    else if (strcmp(cmd, "setPowerMax") == 0) modbus.writeRegister(MB_REG_COMP_POWER_MAX,(uint16_t)constrain(val, 0, 100));

    // Re-read immediately so the UI reflects the change without waiting a full poll
    if (modbus.readAll(coolerState)) {
        ws.textAll(buildJson(coolerState));
    }
}

// ── Arduino setup ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[FR34] Booting...");

    // ── Modbus UART ──
    modbus.begin(Serial2, MB_RX_PIN, MB_TX_PIN, MB_BAUD);
    Serial.println("[FR34] Modbus UART initialised (GPIO16 RX, GPIO17 TX, 9600 baud)");

    // ── WiFi access point ──
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    WiFi.softAP(AP_SSID, (strlen(AP_PASS) > 0) ? AP_PASS : nullptr);
    Serial.print("[FR34] AP started: ");
    Serial.print(AP_SSID);
    Serial.print(" → ");
    Serial.println(WiFi.softAPIP());

    // ── HTTP routes ──
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", WEB_UI);
    });

    server.on("/vue.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse_P(200, "application/javascript", (const uint8_t*)VUE_JS, sizeof(VUE_JS) - 1);
        resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
        req->send(resp);
    });

    // Minimal API endpoint for health checks / ESPHome/HomeAssistant polling
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson(coolerState));
    });

    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    // ── WebSocket ──
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.begin();
    Serial.println("[FR34] HTTP server started");
}

// ── Arduino loop ──────────────────────────────────────────────────────────
void loop() {
    // Clean up disconnected WebSocket clients
    ws.cleanupClients();

    // Poll Modbus at POLL_MS interval
    uint32_t now = millis();
    if (now - lastPoll >= POLL_MS) {
        lastPoll = now;

        if (modbus.readAll(coolerState)) {
            if (ws.count() > 0) {
                ws.textAll(buildJson(coolerState));
            }
        } else {
            Serial.println("[FR34] Modbus read failed");
            coolerState.valid = false;
            if (ws.count() > 0) {
                ws.textAll(buildJson(coolerState)); // broadcasts error flag
            }
        }
    }
}
