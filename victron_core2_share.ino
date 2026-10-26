/*
 * Victron GX  ->  M5Stack Core2 wall display
 * ------------------------------------------------------------------
 * A small wall-mounted monitor for a Victron ESS. Reads live data from
 * the GX device's LOCAL MQTT broker (no internet / VRM cloud needed) and
 * shows it on an M5Stack Core2.
 *
 *   TOP (persistent):  Battery SOC, large, colour-coded.
 *   LOWER (cycles, tap to jump):
 *     SOLAR - combined PV total + per-MPPT (e.g. East/West) + today's kWh
 *     LOAD  - total AC consumption
 *
 * Flicker-free (off-screen M5Canvas), seven-segment digits.
 *
 * HARDWARE:  M5Stack Core2 (ESP32).  BOARD: "M5Core2".
 * LIBRARIES: M5Unified, PubSubClient, ArduinoJson.
 *
 * GX SETUP:  enable MQTT on LAN (plaintext) in the GX settings. The broker
 *            stops publishing unless it receives a keepalive, so this sketch
 *            publishes an empty message to R/<portal>/keepalive every 30s.
 *            Values arrive as JSON: {"value": 12.3} -> we read "value".
 *
 * Provided as-is, no warranty. Tested on one 2-MPPT single-phase system.
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ========================= CONFIGURE ME =========================
// --- Your WiFi ---
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// --- Your Victron GX device (Cerbo / Venus) local MQTT broker ---
const char* MQTT_HOST = "192.168.1.50";        // your GX device's LAN IP
const int   MQTT_PORT = 1883;                  // plaintext MQTT on the LAN
const char* MQTT_USER = "root";                // GX local MQTT user (often "root")
const char* MQTT_PASS = "YOUR_MQTT_PASSWORD";  // leave blank "" if your broker needs no auth

// --- Your VRM portal ID (GX: Settings > VRM online portal) ---
const char* PORTAL_ID = "YOUR_VRM_PORTAL_ID";

// --- Your two solar-charger instances ---
// Find them in MQTT Explorer under  N/<portal>/solarcharger/  (the numeric
// children). They are specific to each system - change to match yours.
const char* MPPT_W = "277";   // first MPPT  (labelled "W" below)
const char* MPPT_E = "278";   // second MPPT (labelled "E" below)

// --- Splash text (cosmetic) ---
const char* SPLASH_TITLE = "SOLAR MONITOR";
const char* SPLASH_SUB   = "live system data";

// SOC colour thresholds (%)
const float SOC_LOW = 20.0, SOC_MID = 50.0;
// ================================================================

String base = String("N/") + PORTAL_ID + "/";
String T_KEEPALIVE = String("R/") + PORTAL_ID + "/keepalive";
String T_SOC  = base + "system/0/Dc/Battery/Soc";
String T_LOAD = base + "system/0/Ac/Consumption/L1/Power";   // single-phase: L1 only
String T_PVW  = base + "solarcharger/" + MPPT_W + "/Yield/Power";
String T_PVE  = base + "solarcharger/" + MPPT_E + "/Yield/Power";
String T_YLDW = base + "solarcharger/" + MPPT_W + "/History/Daily/0/Yield";
String T_YLDE = base + "solarcharger/" + MPPT_E + "/History/Daily/0/Yield";

WiFiClient   net;
PubSubClient mqtt(net);
M5Canvas     canvas(&M5.Display);

float vSoc = NAN, vLoad = NAN, vPvW = NAN, vPvE = NAN, vYldW = NAN, vYldE = NAN;
uint32_t tSoc = 0;

int      pg = 0;                         // 0=SOLAR 1=LOAD
const int PAGES = 2;
uint32_t lastRotate = 0, lastKeep = 0, lastDraw = 0, lastWifiTry = 0, lastMqttTry = 0;

const uint32_t ROTATE_MS    = 6000;
const uint32_t KEEPALIVE_MS = 30000;
const uint32_t STALE_MS     = 30000;

void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;                       // ArduinoJson v7. v6: StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, len)) return;
  if (doc["value"].isNull()) return;
  float v = doc["value"].as<float>(); String t(topic);
  if      (t == T_SOC)  { vSoc = v; tSoc = millis(); }
  else if (t == T_LOAD) vLoad = v;
  else if (t == T_PVW)  vPvW  = v;
  else if (t == T_PVE)  vPvE  = v;
  else if (t == T_YLDW) vYldW = v;
  else if (t == T_YLDE) vYldE = v;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < 10000) return;
  lastWifiTry = millis(); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
}
void ensureMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < 3000) return;
  lastMqttTry = millis();
  String cid = "core2-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(T_SOC.c_str());  mqtt.subscribe(T_LOAD.c_str());
    mqtt.subscribe(T_PVW.c_str());  mqtt.subscribe(T_PVE.c_str());
    mqtt.subscribe(T_YLDW.c_str()); mqtt.subscribe(T_YLDE.c_str());
    mqtt.publish(T_KEEPALIVE.c_str(), ""); lastKeep = millis();
  }
}

uint16_t socColour(float s) {
  if (isnan(s))    return TFT_DARKGREY;
  if (s < SOC_LOW) return TFT_RED;
  if (s < SOC_MID) return M5.Display.color565(255, 150, 0);
  return TFT_GREEN;
}
void fmtPower(float w, char* out, size_t n, const char** unit) {
  if (isnan(w)) { snprintf(out, n, "--"); *unit = "W"; return; }
  if (fabs(w) >= 1000) { snprintf(out, n, "%.1f", w / 1000.0); *unit = "kW"; }
  else                 { snprintf(out, n, "%.0f", w);          *unit = "W";  }
}
float sum2(float a, float b) {
  if (isnan(a) && isnan(b)) return NAN;
  return (isnan(a) ? 0 : a) + (isnan(b) ? 0 : b);
}

void banner() {
  int W = canvas.width();
  bool stale = isnan(vSoc) || (millis() - tSoc > STALE_MS);
  uint16_t col = stale ? TFT_DARKGREY : socColour(vSoc);

  canvas.fillRect(0, 0, W, 6, col);
  canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
  canvas.setTextDatum(top_left);  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("BATTERY", 14, 10);

  char b[8];
  if (isnan(vSoc)) snprintf(b, sizeof(b), "--");
  else             snprintf(b, sizeof(b), "%.0f", vSoc);
  canvas.setFont(&fonts::Font8);                     // ~75px. Fallback: &fonts::Font7
  canvas.setTextSize(1); canvas.setTextDatum(top_left); canvas.setTextColor(col);
  canvas.drawString(b, 14, 28);
  int w = canvas.textWidth(b);
  canvas.setFont(&fonts::Font4); canvas.setTextSize(1);
  canvas.setTextColor(TFT_LIGHTGREY); canvas.setTextDatum(bottom_left);
  canvas.drawString("%", 14 + w + 10, 100);

  canvas.drawFastHLine(0, 108, W, TFT_DARKGREY);
}

void pageSolar() {
  char big[16]; const char* unit;
  fmtPower(sum2(vPvE, vPvW), big, sizeof(big), &unit);

  canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("SOLAR", 14, 114);

  canvas.setFont(&fonts::Font7); canvas.setTextSize(1);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_YELLOW);
  canvas.drawString(big, 14, 134);
  int bw = canvas.textWidth(big);
  canvas.setFont(&fonts::Font4); canvas.setTextSize(1);
  canvas.setTextColor(TFT_LIGHTGREY); canvas.setTextDatum(top_left);
  canvas.drawString(unit, 14 + bw + 8, 150);

  char e[8], w[8], line[40];
  if (isnan(vPvE)) snprintf(e, sizeof(e), "--"); else snprintf(e, sizeof(e), "%.0f", vPvE);
  if (isnan(vPvW)) snprintf(w, sizeof(w), "--"); else snprintf(w, sizeof(w), "%.0f", vPvW);
  snprintf(line, sizeof(line), "E %sW    W %sW", e, w);
  canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE); canvas.setTextDatum(top_left);
  canvas.drawString(line, 14, 188);

  float today = sum2(vYldE, vYldW);
  char tline[32];
  if (isnan(today)) snprintf(tline, sizeof(tline), "Today  --");
  else              snprintf(tline, sizeof(tline), "Today  %.1f kWh", today);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(tline, 14, 210);
}

void pageLoad() {
  char big[16]; const char* unit;
  fmtPower(vLoad, big, sizeof(big), &unit);

  canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("LOAD", 14, 114);

  canvas.setFont(&fonts::Font7); canvas.setTextSize(1);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_CYAN);
  canvas.drawString(big, 14, 150);
  int bw = canvas.textWidth(big);
  canvas.setFont(&fonts::Font4); canvas.setTextSize(1);
  canvas.setTextColor(TFT_LIGHTGREY); canvas.setTextDatum(top_left);
  canvas.drawString(unit, 14 + bw + 8, 166);
}

void drawFrame() {
  int W = canvas.width(), H = canvas.height();
  canvas.fillSprite(TFT_BLACK);
  banner();
  if (pg == 0) pageSolar(); else pageLoad();

  uint16_t accent = (pg == 0) ? TFT_YELLOW : TFT_CYAN;
  for (int i = 0; i < PAGES; i++) {
    int dx = W - 20 - (PAGES - 1 - i) * 20;
    if (i == pg) canvas.fillCircle(dx, H - 12, 5, accent);
    else         canvas.drawCircle(dx, H - 12, 5, TFT_DARKGREY);
  }
  canvas.pushSprite(0, 0);
}

void splash() {
  canvas.fillSprite(TFT_BLACK); canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font4); canvas.setTextSize(1); canvas.setTextColor(TFT_WHITE);
  canvas.drawString(SPLASH_TITLE, canvas.width()/2, canvas.height()/2 - 14);
  canvas.setFont(&fonts::Font2); canvas.setTextColor(TFT_DARKGREY);
  canvas.drawString(SPLASH_SUB, canvas.width()/2, canvas.height()/2 + 22);
  canvas.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config(); M5.begin(cfg);
  M5.Display.setRotation(1);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  splash();
  ensureWifi();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  mqtt.setServer(MQTT_HOST, MQTT_PORT); mqtt.setCallback(onMessage); ensureMqtt();
  delay(1500); lastRotate = millis(); drawFrame();
}

void loop() {
  M5.update(); ensureWifi(); ensureMqtt(); mqtt.loop();
  uint32_t now = millis();
  if (now - lastKeep > KEEPALIVE_MS && mqtt.connected()) {
    mqtt.publish(T_KEEPALIVE.c_str(), ""); lastKeep = now;
  }
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) { pg = (pg + 1) % PAGES; lastRotate = now; drawFrame(); }
  if (now - lastRotate > ROTATE_MS) { pg = (pg + 1) % PAGES; lastRotate = now; drawFrame(); }
  if (now - lastDraw > 1000) { drawFrame(); lastDraw = now; }
}
