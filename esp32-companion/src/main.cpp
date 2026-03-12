/**
 * ESP32 companion for Mobicool FR34 cooler
 *
 * Modbus RTU over the 3-wire UART interface on the PIC16F1829 mainboard.
 * Two transport modes are supported — build with the matching environment:
 *
 *   pio run -e fr34-wifi  →  WiFi AP + WebSocket (http://192.168.4.1/)
 *   pio run -e fr34-ble   →  BLE GATT server (pair with Web Bluetooth PWA)
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
 */

#include <Arduino.h>
#include "modbus_master.h"

#if !defined(TRANSPORT_WIFI) && !defined(TRANSPORT_BLE)
#  error "Define either TRANSPORT_WIFI or TRANSPORT_BLE via build flags"
#endif

// ── WiFi transport ─────────────────────────────────────────────────────────
#ifdef TRANSPORT_WIFI
#  include <WiFi.h>
#  include <ESPAsyncWebServer.h>
#  include <AsyncWebSocket.h>
#  include <ArduinoJson.h>
#  include "web_ui.h"
#  include "vue_js.h"
#endif

// ── BLE transport ──────────────────────────────────────────────────────────
#ifdef TRANSPORT_BLE
#  include <NimBLEDevice.h>
#endif

// ── Common configuration ───────────────────────────────────────────────────
static constexpr char     DEVICE_NAME[] = "FR34-Cooler";
static constexpr int      MB_RX_PIN     = 16;
static constexpr int      MB_TX_PIN     = 17;
static constexpr uint32_t MB_BAUD       = 9600;
static constexpr uint32_t POLL_MS       = 1000;

// ── Common globals ─────────────────────────────────────────────────────────
ModbusMaster modbus;
CoolerState  coolerState;
static uint32_t lastPoll = 0;

// ══════════════════════════════════════════════════════════════════════════════
// WiFi transport
// ══════════════════════════════════════════════════════════════════════════════
#ifdef TRANSPORT_WIFI

static const IPAddress AP_IP    (192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

static String buildJson(const CoolerState& s) {
    JsonDocument doc;
    if (s.valid) {
        doc["temp"]         = s.currentTemp10     / 10.0f;
        doc["setpoint"]     = s.targetTemp10      / 10.0f;
        doc["voltage"]      = s.voltageMilliV     / 1000.0f;
        doc["fanCurrent"]   = s.fanCurrentMilliA  / 1000.0f;
        doc["compPower"]    = s.compPower;
        doc["compPowerMax"] = s.compPowerMax;
    } else {
        doc["error"] = "modbus_fail";
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*,
                      AwsEventType type, void* arg,
                      uint8_t* data, size_t len)
{
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;

    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;

    const char* cmd = doc["cmd"] | "";
    int16_t     val = doc["value"] | 0;

    if      (strcmp(cmd, "setTemp")     == 0) modbus.writeRegister(MB_REG_TARGET_TEMP,   (uint16_t)val);
    else if (strcmp(cmd, "setPower")    == 0) modbus.writeRegister(MB_REG_COMP_POWER,    (uint16_t)constrain(val, 0, 100));
    else if (strcmp(cmd, "setPowerMax") == 0) modbus.writeRegister(MB_REG_COMP_POWER_MAX,(uint16_t)constrain(val, 0, 100));

    if (modbus.readAll(coolerState)) ws.textAll(buildJson(coolerState));
}

static void wifiSetup() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    WiFi.softAP(DEVICE_NAME, nullptr);  // open network
    Serial.printf("[WiFi] AP: %s → %s\n", DEVICE_NAME, WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", WEB_UI);
    });
    server.on("/vue.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        auto* resp = req->beginResponse_P(200, "application/javascript",
                         (const uint8_t*)VUE_JS, sizeof(VUE_JS) - 1);
        resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
        req->send(resp);
    });
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildJson(coolerState));
    });
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
    Serial.println("[WiFi] HTTP server started");
}

static inline void wifiNotify(const CoolerState& s) {
    if (ws.count() > 0) ws.textAll(buildJson(s));
}

#endif  // TRANSPORT_WIFI

// ══════════════════════════════════════════════════════════════════════════════
// BLE transport
// ══════════════════════════════════════════════════════════════════════════════
#ifdef TRANSPORT_BLE

// Custom 128-bit UUIDs for the FR34 cooler GATT service
#define BLE_SVC_UUID       "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
// Status characteristic (READ + NOTIFY): 10-byte little-endian payload
//   [0-1] int16  currentTemp10    (tenths of °C)
//   [2-3] int16  targetTemp10     (tenths of °C)
//   [4-5] uint16 voltageMilliV    (mV)
//   [6-7] uint16 fanCurrentMilliA (mA)
//   [8]   uint8  compPower        (0-100 %)
//   [9]   uint8  compPowerMax     (0-100 %)
#define BLE_STAT_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
// Command characteristics (WRITE / WRITE_NR): raw little-endian value
#define BLE_CMD_TEMP_UUID  "beb5483f-36e1-4688-b7f5-ea07361b26a8"  // int16 (tenths °C)
#define BLE_CMD_PWR_UUID   "beb54840-36e1-4688-b7f5-ea07361b26a8"  // uint8 (0-100)
#define BLE_CMD_PMAX_UUID  "beb54841-36e1-4688-b7f5-ea07361b26a8"  // uint8 (0-100)

static NimBLECharacteristic* bleStatusChar   = nullptr;
static NimBLECharacteristic* bleCmdTempChar  = nullptr;
static NimBLECharacteristic* bleCmdPwrChar   = nullptr;
static NimBLECharacteristic* bleCmdPMaxChar  = nullptr;

static void blePackAndNotify(const CoolerState& s) {
    if (!s.valid || !bleStatusChar) return;
    uint8_t buf[10];
    memcpy(buf + 0, &s.currentTemp10,    2);
    memcpy(buf + 2, &s.targetTemp10,     2);
    memcpy(buf + 4, &s.voltageMilliV,    2);
    memcpy(buf + 6, &s.fanCurrentMilliA, 2);
    buf[8] = s.compPower;
    buf[9] = s.compPowerMax;
    bleStatusChar->setValue(buf, sizeof(buf));
    bleStatusChar->notify();
}

class BleCmdCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo&) override {
        auto val = pChar->getValue();
        if (pChar == bleCmdTempChar && val.size() >= 2) {
            int16_t v; memcpy(&v, val.data(), 2);
            modbus.writeRegister(MB_REG_TARGET_TEMP, (uint16_t)v);
        } else if (pChar == bleCmdPwrChar && val.size() >= 1) {
            modbus.writeRegister(MB_REG_COMP_POWER, (uint16_t)constrain((int)val[0], 0, 100));
        } else if (pChar == bleCmdPMaxChar && val.size() >= 1) {
            modbus.writeRegister(MB_REG_COMP_POWER_MAX, (uint16_t)constrain((int)val[0], 0, 100));
        }
        if (modbus.readAll(coolerState)) blePackAndNotify(coolerState);
    }
};
static BleCmdCallback bleCmdCb;

class BleServerCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
        Serial.printf("[BLE] Connected: %s\n", connInfo.getAddress().toString().c_str());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[BLE] Disconnected (reason %d); restarting advertising\n", reason);
        NimBLEDevice::startAdvertising();
    }
};
static BleServerCallback bleServerCb;

static void bleSetup() {
    NimBLEDevice::init(DEVICE_NAME);

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&bleServerCb);

    NimBLEService* pSvc = pServer->createService(BLE_SVC_UUID);

    bleStatusChar = pSvc->createCharacteristic(BLE_STAT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    bleCmdTempChar = pSvc->createCharacteristic(BLE_CMD_TEMP_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleCmdTempChar->setCallbacks(&bleCmdCb);

    bleCmdPwrChar = pSvc->createCharacteristic(BLE_CMD_PWR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleCmdPwrChar->setCallbacks(&bleCmdCb);

    bleCmdPMaxChar = pSvc->createCharacteristic(BLE_CMD_PMAX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleCmdPMaxChar->setCallbacks(&bleCmdCb);

    pSvc->start();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(BLE_SVC_UUID);
    pAdv->setScanResponseData(NimBLEAdvertisementData{});  // enable scan response
    NimBLEDevice::startAdvertising();
    Serial.printf("[BLE] Advertising as \"%s\"\n", DEVICE_NAME);
}

#endif  // TRANSPORT_BLE

// ── Arduino setup / loop ───────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[FR34] Booting...");
#ifdef TRANSPORT_WIFI
    Serial.println("[FR34] Transport: WiFi AP + WebSocket");
#else
    Serial.println("[FR34] Transport: BLE GATT");
#endif

    modbus.begin(Serial2, MB_RX_PIN, MB_TX_PIN, MB_BAUD);
    Serial.println("[FR34] Modbus UART initialised (GPIO16 RX, GPIO17 TX, 9600 baud)");

#ifdef TRANSPORT_WIFI
    wifiSetup();
#endif
#ifdef TRANSPORT_BLE
    bleSetup();
#endif
}

void loop() {
#ifdef TRANSPORT_WIFI
    ws.cleanupClients();
#endif

    uint32_t now = millis();
    if (now - lastPoll >= POLL_MS) {
        lastPoll = now;
        if (modbus.readAll(coolerState)) {
#ifdef TRANSPORT_WIFI
            wifiNotify(coolerState);
#endif
#ifdef TRANSPORT_BLE
            blePackAndNotify(coolerState);
#endif
        } else {
            Serial.println("[FR34] Modbus read failed");
            coolerState.valid = false;
#ifdef TRANSPORT_WIFI
            wifiNotify(coolerState);  // broadcast error flag to WebSocket clients
#endif
        }
    }
}
