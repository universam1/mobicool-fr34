/**
 * ESP32-C3 companion for Mobicool FR34 cooler
 *
 * Single-wire half-duplex protocol over RC7 (PIC pin 9).
 * ESP32-C3 GPIO4 is the single open-drain data line; no level-shifter needed
 * (both sides 3.3 V). The ESP32-C3 INPUT_PULLUP (~45 kΩ) is sufficient for
 * wire lengths up to ~30 cm at 9600 baud; add an external 4.7 kΩ for longer.
 *
 * Uses UART1 (Serial1) — GPIO 11-17 are reserved for internal SPI flash on
 * the ESP32-C3-MINI-1 module, so GPIO 16 is not available.
 * UART0 (Serial) is used for debug output via the integrated USB Serial/JTAG
 * peripheral; no separate USB-UART chip is needed on the DevKitM-1.
 *
 * Hardware connections
 * ─────────────────────────────────────────────────────────────────────
 *  PIC pin   Signal      ESP32-C3 GPIO
 *  RC7  (9)  DATA        4  (open-drain, INPUT_PULLUP)
 *  GND       GND         GND
 *
 * No RA5 connection required.
 * See WIRING.md for soldering details.
 */

#include <Arduino.h>
#include "comms_master.h"
#include "pic_programmer.h"

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
static constexpr int      COMMS_DATA_PIN  = 4;    // open-drain single wire → PIC RA0/ICSPDAT (GPIO 4)
static constexpr int      ICSP_CLK_PIN    = 6;    // new wire → J2 pin 5 (ICSPCLK/RA1)
static constexpr int      ICSP_MCLR_PIN   = 5;    // new wire → J2 pin 1 (MCLR/VPP), open-drain
static constexpr uint32_t COMMS_BAUD      = 9600;
static constexpr uint32_t POLL_MS         = 1000;

// ── Common globals ─────────────────────────────────────────────────────────
CommsMaster  comms;
CoolerState  coolerState;
PicProgrammer picProg;
static uint32_t lastPoll     = 0;
static bool     flashBusy    = false;

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
        doc["pmode"]        = s.pmode;
    } else {
        doc["error"] = "comms_fail";
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

    if      (strcmp(cmd, "setTemp")     == 0) comms.setTargetTemp((int16_t)val);
    else if (strcmp(cmd, "setPower")    == 0) comms.setCompPower((uint8_t)constrain(val, 0, 100));
    else if (strcmp(cmd, "setPowerMax") == 0) comms.setCompPowerMax((uint8_t)constrain(val, 0, 100));
    else if (strcmp(cmd, "setPMode")    == 0) comms.setPowerMode((uint8_t)constrain(val, 0, 2));

    if (!flashBusy && comms.readAll(coolerState)) ws.textAll(buildJson(coolerState));
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

    // Flash endpoint: POST /api/flash with raw Intel HEX body (text/plain or
    // application/octet-stream). Maximum accepted body: 48 KB (covers the full
    // 8 K-word PIC16F1829 flash in ASCII HEX form with margin to spare).
    static constexpr size_t MAX_HEX_SIZE = 48 * 1024;

    // Body-collection handler
    server.on("/api/flash", HTTP_POST,
        // onRequest  — called after full body is assembled
        [](AsyncWebServerRequest* req) {
            // Body is accessed via req->_tempObject set in onBody handler
            if (flashBusy) {
                req->send(503, "application/json", "{\"error\":\"busy\"}");
                return;
            }
            char* hexBuf = (char*)req->getParam("_hexbuf", "_hexbuf");
            // hexBuf is set via _tempObject trick below
            if (!req->_tempObject) {
                req->send(400, "application/json", "{\"error\":\"no body\"}");
                return;
            }
            struct FlashJob { char* buf; size_t len; AsyncWebServerRequest* req; };
            auto* job = (FlashJob*)req->_tempObject;

            flashBusy = true;
            // Broadcast flash-start event to all WebSocket clients
            ws.textAll("{\"flash\":{\"state\":\"erasing\",\"pct\":0}}");

            // Progress callback — broadcast to WebSocket
            auto progressCb = [](uint8_t pct, void* ctx) {
                auto* wss = (AsyncWebSocket*)ctx;
                char msg[64];
                const char* state = pct < 10  ? "parsing"  :
                                    pct < 80  ? "writing"  :
                                    pct < 98  ? "verifying" : "done";
                snprintf(msg, sizeof(msg),
                    "{\"flash\":{\"state\":\"%s\",\"pct\":%u}}", state, pct);
                wss->textAll(msg);
            };

            ProgramResult result = picProg.flash(job->buf, job->len,
                                                  progressCb, &ws);
            flashBusy = false;

            // Re-initialise comms after programmer released the bus
            comms.begin(COMMS_DATA_PIN, COMMS_BAUD);

            // Free HEX buffer
            free(job->buf);
            free(job);
            req->_tempObject = nullptr;

            if (result.ok) {
                char resp[128];
                snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"wordsWritten\":%u,\"wordsVerified\":%u}",
                    result.wordsWritten, result.wordsVerified);
                req->send(200, "application/json", resp);
                ws.textAll("{\"flash\":{\"state\":\"done\",\"pct\":100}}");
            } else {
                char resp[192];
                snprintf(resp, sizeof(resp),
                    "{\"ok\":false,\"error\":\"%s\"}", result.errorMsg);
                req->send(500, "application/json", resp);
                char msg[192];
                snprintf(msg, sizeof(msg),
                    "{\"flash\":{\"state\":\"error\",\"pct\":0,\"error\":\"%s\"}}",
                    result.errorMsg);
                ws.textAll(msg);
            }
        },
        // onUpload — not used (we use body)
        nullptr,
        // onBody — accumulate entire body into a heap buffer
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (total > MAX_HEX_SIZE) {
                // Silently ignore — onRequest will get nullptr
                return;
            }
            if (index == 0) {
                // First chunk: allocate buffer + FlashJob
                struct FlashJob { char* buf; size_t len; AsyncWebServerRequest* req; };
                auto* job = (FlashJob*)malloc(sizeof(FlashJob));
                job->buf = (char*)malloc(total + 1);
                job->len = total;
                job->req = req;
                req->_tempObject = job;
            }
            if (req->_tempObject) {
                struct FlashJob { char* buf; size_t len; AsyncWebServerRequest* req; };
                auto* job = (FlashJob*)req->_tempObject;
                memcpy(job->buf + index, data, len);
                if (index + len == total) {
                    job->buf[total] = '\0';  // null-terminate for string parsing
                }
            }
        }
    );

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
// Status characteristic (READ + NOTIFY): 11-byte little-endian payload
//   [0-1] int16  currentTemp10    (tenths of °C)
//   [2-3] int16  targetTemp10     (tenths of °C)
//   [4-5] uint16 voltageMilliV    (mV)
//   [6-7] uint16 fanCurrentMilliA (mA)
//   [8]   uint8  compPower        (0-100 %)
//   [9]   uint8  compPowerMax     (0-100 %)
//  [10]   uint8  pmode            (0=Eco 1=Normal 2=Hi)
#define BLE_STAT_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
// Command characteristics (WRITE / WRITE_NR): raw little-endian value
#define BLE_CMD_TEMP_UUID  "beb5483f-36e1-4688-b7f5-ea07361b26a8"  // int16 (tenths °C)
#define BLE_CMD_PWR_UUID   "beb54840-36e1-4688-b7f5-ea07361b26a8"  // uint8 (0-100)
#define BLE_CMD_PMAX_UUID  "beb54841-36e1-4688-b7f5-ea07361b26a8"  // uint8 (0-100)
#define BLE_CMD_PMODE_UUID "beb54842-36e1-4688-b7f5-ea07361b26a8"  // uint8 (0-2)

static NimBLECharacteristic* bleStatusChar    = nullptr;
static NimBLECharacteristic* bleCmdTempChar   = nullptr;
static NimBLECharacteristic* bleCmdPwrChar    = nullptr;
static NimBLECharacteristic* bleCmdPMaxChar   = nullptr;
static NimBLECharacteristic* bleCmdPModeChar  = nullptr;

static void blePackAndNotify(const CoolerState& s) {
    if (!s.valid || !bleStatusChar) return;
    uint8_t buf[11];
    memcpy(buf + 0, &s.currentTemp10,    2);
    memcpy(buf + 2, &s.targetTemp10,     2);
    memcpy(buf + 4, &s.voltageMilliV,    2);
    memcpy(buf + 6, &s.fanCurrentMilliA, 2);
    buf[8]  = s.compPower;
    buf[9]  = s.compPowerMax;
    buf[10] = s.pmode;
    bleStatusChar->setValue(buf, sizeof(buf));
    bleStatusChar->notify();
}

class BleCmdCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo&) override {
        auto val = pChar->getValue();
        if (pChar == bleCmdTempChar && val.size() >= 2) {
            int16_t v; memcpy(&v, val.data(), 2);
            comms.setTargetTemp(v);
        } else if (pChar == bleCmdPwrChar && val.size() >= 1) {
            comms.setCompPower((uint8_t)constrain((int)val[0], 0, 100));
        } else if (pChar == bleCmdPMaxChar && val.size() >= 1) {
            comms.setCompPowerMax((uint8_t)constrain((int)val[0], 0, 100));
        } else if (pChar == bleCmdPModeChar && val.size() >= 1) {
            comms.setPowerMode((uint8_t)constrain((int)val[0], 0, 2));
        }
        if (comms.readAll(coolerState)) blePackAndNotify(coolerState);
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

    bleCmdPModeChar = pSvc->createCharacteristic(BLE_CMD_PMODE_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleCmdPModeChar->setCallbacks(&bleCmdCb);

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
#if defined(TRANSPORT_WIFI) && defined(TRANSPORT_BLE)
    Serial.println("[FR34] Transport: WiFi AP + WebSocket + BLE GATT");
#elif defined(TRANSPORT_WIFI)
    Serial.println("[FR34] Transport: WiFi AP + WebSocket");
#else
    Serial.println("[FR34] Transport: BLE GATT");
#endif

    comms.begin(COMMS_DATA_PIN, COMMS_BAUD);
    Serial.println("[FR34] Comms initialised (GPIO4 open-drain, 9600 baud)");

    picProg.configure(COMMS_DATA_PIN, ICSP_CLK_PIN, ICSP_MCLR_PIN);
    Serial.println("[FR34] ICSP programmer configured (DAT=GPIO4, CLK=GPIO6, MCLR=GPIO5)");

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
    if (!flashBusy && now - lastPoll >= POLL_MS) {
        lastPoll = now;
        if (comms.readAll(coolerState)) {
#ifdef TRANSPORT_WIFI
            wifiNotify(coolerState);
#endif
#ifdef TRANSPORT_BLE
            blePackAndNotify(coolerState);
#endif
        } else {
            Serial.println("[FR34] Comms read failed");
            coolerState.valid = false;
#ifdef TRANSPORT_WIFI
            wifiNotify(coolerState);  // broadcast error flag to WebSocket clients
#endif
        }
    }
}
