/*
 * SmartFarm ESP32 — Nhà kính thông minh (AUTO/MANUAL + MQTT)
 *
 * PINOUT (ESP32 DevKit — tránh strapping GPIO0/2/12/15):
 *  DHT11 GPIO14 | MQ135 AO GPIO34 | LDR GPIO33
 *  Đất AO GPIO35 | Mực nước AO GPIO32 | Buzzer GPIO25
 *  Relay đèn GPIO13
 *  L298 (chỉ ON/OFF, không PWM %):
 *    ENA GPIO16 — 2 quạt cùng 1 kênh | ENB GPIO17 — bơm
 *    (IN1..IN4 cố định chiều quay trên module — ESP không điều khiển)
 *  Nút BOOT (GPIO0): giữ 5 giây → portal cấu hình WiFi (AP SmartFarm)
 *
 * SPI TFT ST7789 2.4" 240x320 (CS nối tắt phần cứng):
 *  SCLK 18 | MOSI 23 | MISO 19 | DC 4 | RST 22
 *
 * MQTT (giữ pattern cmnd/tele):
 *  cmnd/123_mode/POWER | cmnd/123_rules/SET|/GET
 *  cmnd/123_pump|/light|/fan1|/fan2/POWER   ON|OFF (MANUAL)
 *    (fan1 & fan2 cùng kênh L298 — bật/tắt cả 2 quạt)
 *  cmnd/123_co2_cal/SET                 calibrate R0 ngoài trời
 *  AUTO: CO2 >= co2FanOn → quạt; đất khô → bơm (L298)
 *  tele/... status + vitals/rules
 *
 * Thư viện Arduino IDE (ESP32):
 *  DHT, Adafruit GFX, Adafruit ST7735/ST7789, WebSockets, MQTTPubSubClient
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <math.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WebSocketsClient.h>
#include <MQTTPubSubClient.h>
#include "DHT.h"

using MqttClient512 = arduino::mqtt::PubSubClient<512>;

// ─── Pins ────────────────────────────────────────────────────
#define PIN_DHT        14
#define PIN_CO2        34   // MQ135 AO — ADC1
#define PIN_LDR        33
#define PIN_SOIL       35   // độ ẩm đất AO — ADC1
#define PIN_WATER      32   // mực nước AO — ADC1
#define PIN_BUZZER     25
#define PIN_LIGHT      13
#define PIN_L298_FAN   16   // L298 ENA — 2 quạt cùng kênh ON/OFF
#define PIN_L298_PUMP  17   // L298 ENB — bơm ON/OFF
#define PIN_BOOT        0   // nút BOOT trên DevKit — active LOW
#define BOOT_HOLD_MS 5000UL // giữ 5s → portal WiFi
// Còi active-LOW (LOW = kêu, HIGH = tắt)
#define BUZZER_ACTIVE_LOW 1
// L298 ENx: HIGH = chạy, LOW = dừng
#define L298_ON        HIGH
#define L298_OFF       LOW

// SPI TFT ST7789 240x320 — CS nối tắt phần cứng (không điều khiển)
#define PIN_TFT_SCLK   18
#define PIN_TFT_MOSI   23
#define PIN_TFT_MISO   19
#define PIN_TFT_DC      4
#define PIN_TFT_RST    22
#define PIN_TFT_CS     -1
// Nhiều module ST7789 rẻ cần SPI_MODE3; nếu màn trắng/đen thử SPI_MODE0
#define TFT_SPI_MODE   SPI_MODE3
#define TFT_SPI_HZ     8000000UL    // 8 MHz — CS nối tắt dễ lệch nếu SPI nhanh
#define TFT_W          240
#define TFT_H          320
#define UI_MS          400
#define UI_TFT_REINIT_MS   45000UL  // reset cứng + init lại (tránh màn trắng)

#define DHTTYPE        DHT11
// Đèn: module relay active-HIGH (HIGH = ON)
#define LIGHT_ON       HIGH
#define LIGHT_OFF      LOW

#define AP_SSID        "SmartFarm"
#define AP_PASSWORD    ""
IPAddress apIP(192, 168, 4, 1);
const byte DNS_PORT = 53;

#define MQTT_HOST      "mqtt.duynguyen.io.vn"
#define MQTT_PORT      443
#define MQTT_PATH      "/mqtt"
#define CHIP_ID        "123"

#define DEV_TEMP       "123_temp_greenhouse"
#define DEV_HUMI       "123_humi_greenhouse"
#define DEV_SOIL       "123_soil_greenhouse"
#define DEV_CO2        "123_co2_greenhouse"
#define DEV_LDR        "123_ldr_greenhouse"
#define DEV_WATER      "123_water_tank"
#define DEV_PUMP       "123_pump"
#define DEV_LIGHT      "123_light"
#define DEV_FAN1       "123_fan1"
#define DEV_FAN2       "123_fan2"
#define DEV_MODE       "123_mode"
#define DEV_RULES      "123_rules"
#define DEV_VITALS     "123_vitals"
#define DEV_CO2_CAL    "123_co2_cal"

#define EEPROM_SIZE    512
#define MAX_WIFI       5
#define RULES_EEPROM_ADDR 400
#define RULES_MAGIC    0xBB   // bump: tưới chuẩn khô→bơm; relay active-LOW

#define TELEMETRY_MS   5000
#define SENSOR_FAST_MS  400
#define HEARTBEAT_MS  30000
#define RECONNECT_MS  10000

// Màu phụ (RGB565)
#define COL_BG         ST77XX_BLACK
#define COL_HEADER     0x1A4C   // xanh đậm
#define COL_MUTED      0x8410
#define COL_OK         ST77XX_GREEN
#define COL_WARN       ST77XX_YELLOW
#define COL_ALERT      ST77XX_RED
#define COL_ACCENT     ST77XX_CYAN
#define COL_WATER      0x041F   // xanh dương (dễ nhìn hơn cyan/xanh lá)

#define ADC_RAW_MAX    4095
#define ADC_SCALE_MAX  1023

// MQ135: RL trên module thường ~10k–20k; dùng 10k mặc định
#define MQ135_RL_OHM   10000.0f
#define MQ135_VCC      5.0f
// ppm = A * (Rs/R0)^B  (Gironi / datasheet CO2 curve)
#define MQ135_A        116.6020682f
#define MQ135_B        (-2.769034857f)
#define MQ135_ATM_PPM  400.0f   // CO2 ngoài trời khi calibrate

struct WifiEntry { char ssid[32]; char pass[32]; };
WifiEntry wifiList[MAX_WIFI];
int wifiCount = 0;

enum ControlMode : uint8_t { MODE_MANUAL = 0, MODE_AUTO = 1 };

struct AutoRules {
  uint8_t  magic;
  uint8_t  mode;
  uint16_t soilDryOn;       // ADC >= → bật bơm (đất khô — cần tưới)
  uint16_t soilWetOff;      // ADC <= → tắt bơm (đất đủ ẩm)
  uint16_t waterEmptyThr;   // ADC thấp hơn (hoặc cao hơn nếu invert) → hết nước
  uint8_t  waterEmptyInvert;// 0: ADC thấp = hết (module thường); 1: ADC cao = hết
  uint16_t co2FanOn;        // chỉ số CO2 >= → bật quạt (mặc định 500)
  uint16_t co2FanOff;       // chỉ số CO2 <= → tắt quạt (hysteresis)
  uint8_t  co2AdcInvert;    // 1: đảo ADC (AO giảm khi có khí)
  uint8_t  darkLightOn;     // 1: tối → đèn
  uint8_t  buzzerOnEmpty;
  uint8_t  pumpLockOnEmpty; // 1: hết nước → không bơm
  float    mq135R0;         // Ω — calibrate
};

AutoRules rules;

DHT dht(PIN_DHT, DHTTYPE);
WebServer webServer(80);
DNSServer dnsServer;
WebSocketsClient wsClient;
MqttClient512 mqttClient;
Adafruit_ST7789 tft = Adafruit_ST7789(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

bool portalActive = false;
bool tftOk = false;
bool portalUiDirty = true;
bool homeLayoutOk = false;
unsigned long lastUi = 0;
unsigned long lastTftReinit = 0;

// Cache UI — chỉ vẽ lại ô thay đổi (tránh fillScreen → hết chớp)
struct UiSnap {
  uint8_t mode;
  bool wifi;
  bool mqtt;
  float temp;
  float humi;
  int soil;
  int water;
  int co2Raw;
  float co2Ppm;
  bool dark;
  bool waterEmpty;
  bool fan1;
  bool fan2;
  char pump[4];
  char light[4];
  char ip[20];
};
UiSnap uiPrev;
bool uiPrevAlert = true;
String pumpState = "OFF";
String lightState = "OFF";
String fan1State = "OFF";
String fan2State = "OFF";

float lastTemp = NAN;
float lastHumi = NAN;
int lastSoilRaw = 0;
int lastWaterRaw = 0;
int lastCo2AdcHw = 0;   // ADC phần cứng thật
int lastCo2Raw = 0;     // chỉ số dùng UI/quạt (đã đảo nếu co2AdcInvert)
float lastCo2Ppm = NAN;
bool isDark = false;
bool isWaterEmpty = false;
uint8_t waterEmptyHits = 0;   // debounce trước khi báo hết nước
uint8_t waterOkHits = 0;
bool isSoilDry = false;

unsigned long lastTelemetry = 0;
unsigned long lastFastRead = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastReconnect = 0;
unsigned long lastBuzzerPulse = 0;
unsigned long bootPressStart = 0;
bool bootHoldDone = false;
int wifiRetries = 0;

void saveRules();
void loadRules();
void pubMode();
void pubRules();
void pubVitals();
void pubPump();
void pubLight();
void pubFan1();
void pubFan2();
void pubSoil();
void pubWater();
void pubCo2();
void pubLdr();
void applyAutoLogic();
void mqttCallback(const String& topicStr, const String& payload);

// ─── Actuators (L298: quạt ENA + bơm ENB; relay chỉ còn đèn) ─
void setPump(bool on) {
  digitalWrite(PIN_L298_PUMP, on ? L298_ON : L298_OFF);
  pumpState = on ? "ON" : "OFF";
}

void setLight(bool on) {
  digitalWrite(PIN_LIGHT, on ? LIGHT_ON : LIGHT_OFF);
  lightState = on ? "ON" : "OFF";
}

String lightRead() {
  return (digitalRead(PIN_LIGHT) == LIGHT_ON) ? "ON" : "OFF";
}

String pumpRead() {
  return (digitalRead(PIN_L298_PUMP) == L298_ON) ? "ON" : "OFF";
}

void l298Init() {
  pinMode(PIN_L298_FAN, OUTPUT);
  pinMode(PIN_L298_PUMP, OUTPUT);
  digitalWrite(PIN_L298_FAN, L298_OFF);
  digitalWrite(PIN_L298_PUMP, L298_OFF);
  fan1State = "OFF";
  fan2State = "OFF";
  pumpState = "OFF";
}

/** 2 quạt chung 1 kênh L298 — fan1/fan2 MQTT luôn đồng bộ. */
void setBothFans(bool on) {
  digitalWrite(PIN_L298_FAN, on ? L298_ON : L298_OFF);
  fan1State = on ? "ON" : "OFF";
  fan2State = on ? "ON" : "OFF";
}

void setFan(uint8_t /*fanId*/, bool on) {
  setBothFans(on);
}

void buzzerWrite(bool on) {
#if BUZZER_ACTIVE_LOW
  digitalWrite(PIN_BUZZER, on ? LOW : HIGH);  // LOW = kêu
#else
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#endif
}

void beep(uint16_t ms = 80) {
  buzzerWrite(true);
  delay(ms);
  buzzerWrite(false);
}

/**
 * Mẫu hết nước (không sticky). Có nước ~360–500+, hết thường ADC thấp.
 * invert=0: hết khi ADC thấp. invert=1: hết khi ADC cao.
 */
bool waterSampleEmpty(int adc) {
  int thr = (int)rules.waterEmptyThr;
  if (rules.waterEmptyInvert) return adc >= thr;
  return adc <= thr;
}

/** Debounce vài mẫu liên tiếp trước khi đổi isWaterEmpty (tránh còi kêu oan). */
void waterUpdateEmptyState(int adc) {
  const uint8_t need = 4;  // ~1.6s với SENSOR_FAST_MS=400
  if (waterSampleEmpty(adc)) {
    waterOkHits = 0;
    if (waterEmptyHits < 255) waterEmptyHits++;
    if (!isWaterEmpty && waterEmptyHits >= need) {
      isWaterEmpty = true;
      Serial.printf("Water EMPTY adc=%d thr=%d inv=%d\n",
                    adc, rules.waterEmptyThr, rules.waterEmptyInvert);
    }
  } else {
    waterEmptyHits = 0;
    if (waterOkHits < 255) waterOkHits++;
    if (isWaterEmpty && waterOkHits >= need) {
      isWaterEmpty = false;
      buzzerWrite(false);
      Serial.printf("Water OK adc=%d\n", adc);
    }
  }
}

void buzzerEmptyUpdate() {
  if (!(isWaterEmpty && rules.buzzerOnEmpty)) {
    buzzerWrite(false);
    return;
  }
  // Bíp ngắn 150ms mỗi 2s — không kêu liên tục
  unsigned long now = millis();
  unsigned long dt = now - lastBuzzerPulse;
  if (dt >= 2000) {
    lastBuzzerPulse = now;
    buzzerWrite(true);
  } else if (dt >= 150) {
    buzzerWrite(false);
  }
}

// ─── ADC helpers ─────────────────────────────────────────────
int readAdcScaled(uint8_t pin) {
  long sum = 0;
  const int n = 8;  // ít mẫu hơn — tránh delay dài làm đơ loop/TFT
  for (int i = 0; i < n; i++) {
    int raw12 = analogRead(pin);
    if (raw12 < 0) raw12 = 0;
    if (raw12 > ADC_RAW_MAX) raw12 = ADC_RAW_MAX;
    sum += (raw12 * ADC_SCALE_MAX) / ADC_RAW_MAX;
  }
  return (int)(sum / n);
}

/** Rs từ điện áp AO (giả định VCC module 5V, AO chia về ~0..3.3V trên ESP). */
float mq135RsFromAdc(int scaled0_1023) {
  float adc = (float)scaled0_1023;
  if (adc < 1.0f) adc = 1.0f;
  if (adc > 1022.0f) adc = 1022.0f;
  // Vo ≈ ADC/1023 * 3.3 — nhiều module MQ AO đã về mức ESP; dùng tỷ lệ RL
  float vo = (adc / (float)ADC_SCALE_MAX) * 3.3f;
  if (vo < 0.01f) vo = 0.01f;
  if (vo >= MQ135_VCC - 0.05f) vo = MQ135_VCC - 0.05f;
  // Rs = RL * (Vcc - Vo) / Vo
  return MQ135_RL_OHM * (MQ135_VCC - vo) / vo;
}

float mq135PpmFromRs(float rs) {
  float r0 = rules.mq135R0;
  if (r0 < 100.0f) r0 = 100.0f;
  float ratio = rs / r0;
  if (ratio < 0.01f) ratio = 0.01f;
  float ppm = MQ135_A * powf(ratio, MQ135_B);
  if (ppm < 0.0f) ppm = 0.0f;
  if (ppm > 10000.0f) ppm = 10000.0f;
  return ppm;
}

void calibrateMq135R0() {
  int raw = readAdcScaled(PIN_CO2);
  float rs = mq135RsFromAdc(raw);
  // R0 = Rs * (ppm/A)^(-1/B) tại ppm khí quyển
  float r0 = rs * powf(MQ135_ATM_PPM / MQ135_A, -1.0f / MQ135_B);
  if (r0 < 100.0f) r0 = 100.0f;
  rules.mq135R0 = r0;
  saveRules();
  lastCo2AdcHw = raw;
  lastCo2Raw = co2IndexFromHw(raw);
  lastCo2Ppm = mq135PpmFromRs(rs);
  Serial.printf("CO2 CAL R0=%.1f Rs=%.1f hw=%d idx=%d ppm~%.0f\n",
                rules.mq135R0, rs, raw, lastCo2Raw, lastCo2Ppm);
}

/** Đảo ADC nếu module AO giảm khi khí tăng (co2AdcInvert=1). */
int co2IndexFromHw(int adcHw) {
  if (adcHw < 0) adcHw = 0;
  if (adcHw > ADC_SCALE_MAX) adcHw = ADC_SCALE_MAX;
  if (rules.co2AdcInvert) return ADC_SCALE_MAX - adcHw;
  return adcHw;
}

/** AUTO: CO2 >= co2FanOn → bật; CO2 <= co2FanOff → tắt (hysteresis). */
bool wantFansFromCo2(bool currentlyOn) {
  int onThr = (int)rules.co2FanOn;
  int offThr = (int)rules.co2FanOff;
  if (offThr >= onThr) offThr = onThr - 50;
  if (offThr < 0) offThr = 0;
  if (currentlyOn) return lastCo2Raw > offThr;
  return lastCo2Raw >= onThr;
}

// ─── Rules EEPROM ────────────────────────────────────────────
void defaultRules() {
  rules.magic = RULES_MAGIC;
  rules.mode = MODE_AUTO;     // AUTO: tối→đèn, đất khô→bơm, CO2→quạt
  // Cảm biến đất điện dung: khô ~900–1023 → tưới; ẩm ~300–500 → dừng
  rules.soilDryOn = 850;      // ADC >= → bật bơm
  rules.soilWetOff = 550;     // ADC <= → tắt bơm
  // Có nước ~360–500 → thr=280: ADC<=280 mới hết; ADC 360/500 = còn nước
  rules.waterEmptyThr = 280;
  rules.waterEmptyInvert = 0;
  rules.co2FanOn = 500;    // CO2 >= 500 → bật quạt
  rules.co2FanOff = 450;   // CO2 <= 450 → tắt (hysteresis)
  rules.co2AdcInvert = 1;  // AO phần cứng giảm khi có khí → phải đảo
  rules.darkLightOn = 1;
  rules.buzzerOnEmpty = 1;
  rules.pumpLockOnEmpty = 1;
  rules.mq135R0 = 41000.0f;   // giá trị khởi tạo — nên calibrate
}

void saveRules() {
  rules.magic = RULES_MAGIC;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(RULES_EEPROM_ADDR, rules);
  EEPROM.commit();
  EEPROM.end();
}

void loadRules() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(RULES_EEPROM_ADDR, rules);
  EEPROM.end();
  if (rules.magic != RULES_MAGIC) {
    defaultRules();
    saveRules();
  }
  // Đất: dryOn phải cao hơn wetOff (khô = ADC cao)
  if (rules.soilDryOn < rules.soilWetOff) {
    uint16_t t = rules.soilDryOn;
    rules.soilDryOn = rules.soilWetOff;
    rules.soilWetOff = t;
  }
  if (rules.co2FanOff >= rules.co2FanOn) {
    if (rules.co2FanOn > 50) rules.co2FanOff = rules.co2FanOn - 50;
    else rules.co2FanOff = 0;
  }
  if (rules.co2AdcInvert > 1) rules.co2AdcInvert = 1;
  if (rules.waterEmptyThr > ADC_SCALE_MAX) rules.waterEmptyThr = ADC_SCALE_MAX;
  if (rules.waterEmptyInvert > 1) rules.waterEmptyInvert = 0;
  if (rules.mq135R0 < 100.0f) rules.mq135R0 = 41000.0f;
  if (rules.buzzerOnEmpty > 1) rules.buzzerOnEmpty = 1;
  if (rules.pumpLockOnEmpty > 1) rules.pumpLockOnEmpty = 1;
  if (rules.darkLightOn > 1) rules.darkLightOn = 1;
}

// ─── TFT ST7789 UI ───────────────────────────────────────────
void tftHwReset() {
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, HIGH);
  delay(5);
  digitalWrite(PIN_TFT_RST, LOW);
  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH);
  delay(120);
}

/** Init / phục hồi khi màn trắng (CS nối tắt rất dễ lệch SPI). */
void tftBegin() {
  tftHwReset();
  tft.init(TFT_W, TFT_H, TFT_SPI_MODE);
  tft.setRotation(1);   // landscape 320x240
  tft.setSPISpeed(TFT_SPI_HZ);
  tft.setTextWrap(false);
  tft.invertDisplay(false);
  tft.fillScreen(COL_BG);
  tftOk = true;
  homeLayoutOk = false;
  lastTftReinit = millis();
}

void tftPrintAt(int16_t x, int16_t y, uint16_t color, uint8_t size, const char* txt) {
  tft.setTextColor(color, COL_BG);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(txt);
}

/** In text độ dài cố định (pad space) + nền đen → ghi đè không cần xóa cả màn. */
void tftField(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint8_t widthChars, const char* txt) {
  char line[40];
  if (widthChars >= sizeof(line)) widthChars = sizeof(line) - 1;
  snprintf(line, sizeof(line), "%-*.*s", (int)widthChars, (int)widthChars, txt);
  tft.setTextColor(fg, COL_BG);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(line);
}

void tftBadge(int16_t x, int16_t y, int16_t w, int16_t h, const char* txt, bool on) {
  uint16_t bg = on ? COL_OK : 0x3186;
  uint16_t fg = on ? ST77XX_BLACK : ST77XX_WHITE;
  tft.fillRect(x, y, w, h, bg);  // tránh fillRoundRect (SPI dài dễ lệch)
  tft.setTextColor(fg, bg);
  tft.setTextSize(1);
  int16_t tw = (int16_t)strlen(txt) * 6;
  tft.setCursor(x + (w - tw) / 2, y + (h - 8) / 2);
  tft.print(txt);
}

void uiDrawHomeStatic() {
  tft.fillScreen(COL_BG);
  yield();
  tft.fillRect(0, 0, 320, 26, COL_HEADER);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(6, 5);
  tft.print(F("SmartFarm"));

  tft.setTextSize(1);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.setCursor(8, 124);
  tft.print(F("THIET BI"));

  memset(&uiPrev, 0xFF, sizeof(uiPrev));  // ép cập nhật hết lần đầu
  uiPrev.pump[0] = 0;
  uiPrev.light[0] = 0;
  uiPrev.ip[0] = 0;
  uiPrev.waterEmpty = !isWaterEmpty;  // ép vẽ lại dòng nước / cảnh báo
  uiPrevAlert = !isWaterEmpty;        // khác trạng thái hiện tại → chắc chắn vẽ
  homeLayoutOk = true;
}

void uiDrawHome();  // forward

/** Ép vẽ lại toàn bộ trang chính (xóa splash / phục hồi sau WiFi). */
void uiEnterHome() {
  if (!tftOk) return;
  homeLayoutOk = false;
  lastUi = 0;
  uiDrawHome();
}

void uiInit() {
  SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, -1);
  tftBegin();
  uiDrawHomeStatic();
  tftPrintAt(8, 100, COL_MUTED, 1, "Dang khoi dong...");
  Serial.println(F("TFT ST7789 OK 240x320 rot=1 SPI 8MHz + reinit"));
}

void uiDrawPortal() {
  if (!tftOk) return;
  homeLayoutOk = false;
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, 320, 28, COL_HEADER);
  tftPrintAt(8, 8, ST77XX_WHITE, 2, "WiFi PORTAL");
  tftPrintAt(16, 60, ST77XX_WHITE, 2, "SSID: SmartFarm");
  tftPrintAt(16, 100, COL_ACCENT, 2, "192.168.4.1");
  tftPrintAt(16, 150, COL_MUTED, 1, "Ket noi AP roi mo trinh duyet");
  tftPrintAt(16, 170, COL_MUTED, 1, "Giu BOOT 5s de vao portal");
}

void uiDrawHome() {
  if (!tftOk) return;
  if (!homeLayoutOk) uiDrawHomeStatic();

  char buf[40];
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = mqttClient.isConnected();

  // Header mode / link — chỉ khi đổi
  if (uiPrev.mode != rules.mode || uiPrev.wifi != wifiOk || uiPrev.mqtt != mqttOk) {
    tft.fillRect(190, 0, 130, 26, COL_HEADER);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(200, 9);
    tft.print(rules.mode == MODE_AUTO ? F("AUTO  ") : F("MANUAL"));
    tft.setCursor(260, 9);
    tft.printf("%c%c", wifiOk ? 'W' : '-', mqttOk ? 'M' : '-');
    uiPrev.mode = rules.mode;
    uiPrev.wifi = wifiOk;
    uiPrev.mqtt = mqttOk;
  }

  // Temp
  if (uiPrev.temp != lastTemp) {
    if (!isnan(lastTemp)) snprintf(buf, sizeof(buf), "Temp %.1fC", lastTemp);
    else snprintf(buf, sizeof(buf), "Temp --.-C");
    tftField(8, 36, 2, COL_ACCENT, 12, buf);
    uiPrev.temp = lastTemp;
  }

  // Humi
  if (uiPrev.humi != lastHumi) {
    if (!isnan(lastHumi)) snprintf(buf, sizeof(buf), "Hum %.0f%%", lastHumi);
    else snprintf(buf, sizeof(buf), "Hum --%%");
    tftField(160, 36, 2, COL_ACCENT, 10, buf);
    uiPrev.humi = lastHumi;
  }

  // Soil
  if (uiPrev.soil != lastSoilRaw) {
    snprintf(buf, sizeof(buf), "Dat %d", lastSoilRaw);
    tftField(8, 64, 2, ST77XX_WHITE, 10, buf);
    uiPrev.soil = lastSoilRaw;
  }

  // Water
  if (uiPrev.water != lastWaterRaw || uiPrev.waterEmpty != isWaterEmpty) {
    if (isWaterEmpty) {
      tftField(160, 64, 2, COL_ALERT, 10, "Nuoc HET");  // cảnh báo = đỏ
    } else {
      snprintf(buf, sizeof(buf), "Nuoc %d", lastWaterRaw);
      tftField(160, 64, 2, COL_WATER, 10, buf);  // có nước = xanh dương
    }
    uiPrev.water = lastWaterRaw;
    uiPrev.waterEmpty = isWaterEmpty;
  }

  // CO2 — hiện ADC (điều khiển quạt) + ppm tham khảo
  if (uiPrev.co2Raw != lastCo2Raw || uiPrev.co2Ppm != lastCo2Ppm) {
    snprintf(buf, sizeof(buf), "CO2 %d", lastCo2Raw);
    tftField(8, 92, 2, ST77XX_WHITE, 10, buf);
    uiPrev.co2Raw = lastCo2Raw;
    uiPrev.co2Ppm = lastCo2Ppm;
  }

  // LDR
  if (uiPrev.dark != isDark) {
    tftField(160, 92, 2, isDark ? COL_ALERT : COL_OK, 6, isDark ? "TOI" : "SANG");
    uiPrev.dark = isDark;
  }

  // Device badges — chỉ khi trạng thái đổi
  bool pumpOn = (pumpState == "ON");
  bool lightOn = (lightState == "ON");
  bool pumpChanged = (strcmp(uiPrev.pump, pumpState.c_str()) != 0);
  bool lightChanged = (strcmp(uiPrev.light, lightState.c_str()) != 0);
  bool f1On = (fan1State == "ON");
  bool f2On = (fan2State == "ON");
  bool fanChanged = (uiPrev.fan1 != f1On || uiPrev.fan2 != f2On);

  if (pumpChanged || lightChanged || fanChanged) {
    if (pumpChanged) {
      tftBadge(8, 140, 72, 28, pumpOn ? "BOM ON" : "BOM", pumpOn);
      strncpy(uiPrev.pump, pumpState.c_str(), sizeof(uiPrev.pump) - 1);
      uiPrev.pump[sizeof(uiPrev.pump) - 1] = 0;
    }
    if (lightChanged) {
      tftBadge(88, 140, 72, 28, lightOn ? "DEN ON" : "DEN", lightOn);
      strncpy(uiPrev.light, lightState.c_str(), sizeof(uiPrev.light) - 1);
      uiPrev.light[sizeof(uiPrev.light) - 1] = 0;
    }
    if (fanChanged) {
      // 2 quạt cùng kênh — một badge
      tft.fillRect(168, 140, 144, 28, COL_BG);
      tftBadge(168, 140, 144, 28, f1On ? "QUAT ON" : "QUAT", f1On);
      uiPrev.fan1 = f1On;
      uiPrev.fan2 = f2On;
    }
  }

  // Alert / hint — luôn theo isWaterEmpty (không phụ thuộc buzzerOnEmpty)
  bool alert = isWaterEmpty;
  if (uiPrevAlert != alert) {
    if (alert) {
      tft.fillRect(0, 200, 320, 20, COL_ALERT);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.setCursor(8, 206);
      tft.print(F("CANH BAO: HET NUOC TRONG BON"));
    } else {
      tft.fillRect(0, 200, 320, 20, COL_BG);
      tftField(8, 204, 1, COL_MUTED, 38, "BOOT giu 5s = cau hinh WiFi");
    }
    uiPrevAlert = alert;
  }

  // IP
  char ipNow[20];
  if (wifiOk) {
    snprintf(ipNow, sizeof(ipNow), "IP %s", WiFi.localIP().toString().c_str());
  } else {
    snprintf(ipNow, sizeof(ipNow), "WiFi mat ket noi");
  }
  if (strcmp(uiPrev.ip, ipNow) != 0) {
    tftField(8, 224, 1, COL_MUTED, 28, ipNow);
    strncpy(uiPrev.ip, ipNow, sizeof(uiPrev.ip) - 1);
    uiPrev.ip[sizeof(uiPrev.ip) - 1] = 0;
  }
}

void uiDraw() {
  if (!tftOk) return;
  if (millis() - lastUi < UI_MS) return;
  lastUi = millis();

  if (portalActive) {
    if (portalUiDirty) {
      uiDrawPortal();
      portalUiDirty = false;
    }
    return;
  }

  // Màn trắng: chỉ hết bằng reset cứng + init lại (CS nối tắt)
  if (millis() - lastTftReinit >= UI_TFT_REINIT_MS) {
    Serial.println(F("TFT recover: hw reset + reinit"));
    tftBegin();
    uiEnterHome();
    return;
  }

  uiDrawHome();
}

// ─── EEPROM WiFi ─────────────────────────────────────────────
void saveWifiList() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiCount);
  int addr = sizeof(wifiCount);
  for (int i = 0; i < MAX_WIFI; i++) {
    EEPROM.put(addr, wifiList[i]);
    addr += sizeof(WifiEntry);
  }
  EEPROM.commit();
  EEPROM.end();
}

void loadWifiList() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiCount);
  if (wifiCount < 0 || wifiCount > MAX_WIFI) wifiCount = 0;
  int addr = sizeof(wifiCount);
  for (int i = 0; i < MAX_WIFI; i++) {
    EEPROM.get(addr, wifiList[i]);
    addr += sizeof(WifiEntry);
  }
  EEPROM.end();
}

void addOrUpdateWifi(String ssid, String pass) {
  for (int i = 0; i < wifiCount; i++) {
    if (String(wifiList[i].ssid) == ssid) {
      pass.toCharArray(wifiList[i].pass, 32);
      saveWifiList();
      return;
    }
  }
  if (wifiCount >= MAX_WIFI) {
    for (int i = 0; i < MAX_WIFI - 1; i++) wifiList[i] = wifiList[i + 1];
    wifiCount = MAX_WIFI - 1;
  }
  ssid.toCharArray(wifiList[wifiCount].ssid, 32);
  pass.toCharArray(wifiList[wifiCount].pass, 32);
  wifiCount++;
  saveWifiList();
}

bool connectBestWifi() {
  Serial.println(F("\nQuet WiFi..."));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  if (wifiCount == 0) return false;

  int n = WiFi.scanNetworks();
  int bestIdx = -1, bestRSSI = -999;
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String scannedSSID = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      for (int w = 0; w < wifiCount; w++) {
        if (String(wifiList[w].ssid) == scannedSSID && rssi > bestRSSI) {
          bestRSSI = rssi;
          bestIdx = w;
        }
      }
    }
    WiFi.scanDelete();
  }
  if (bestIdx < 0) bestIdx = wifiCount - 1;

  WiFi.begin(wifiList[bestIdx].ssid, wifiList[bestIdx].pass);
  // Chờ ngắn hơn — tránh đơ TFT/loop quá lâu
  for (int i = 0; i < 16 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    yield();
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK " + WiFi.localIP().toString());
    return true;
  }
  return false;
}

/** Reconnect nhanh không scan (dùng trong loop). */
bool connectWifiQuick() {
  if (wifiCount <= 0) return false;
  int idx = wifiCount - 1;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiList[idx].ssid, wifiList[idx].pass);
  for (int i = 0; i < 12 && WiFi.status() != WL_CONNECTED; i++) {
    delay(200);
    yield();
  }
  return WiFi.status() == WL_CONNECTED;
}

// ─── AUTO logic ──────────────────────────────────────────────
void applyAutoLogic() {
  // Đèn theo LDR — chỉ khi AUTO (MANUAL: điều khiển bằng MQTT)
  if (rules.mode == MODE_AUTO && rules.darkLightOn) {
    bool wantLight = isDark;
    if ((lightState == "ON") != wantLight) {
      setLight(wantLight);
      pubLight();
      Serial.printf("AUTO den => %s (LDR %s)\n",
                    wantLight ? "ON" : "OFF", isDark ? "TOI" : "SANG");
    }
  } else if (rules.mode != MODE_AUTO && isDark && rules.darkLightOn) {
    static unsigned long lastManWarn = 0;
    if (millis() - lastManWarn > 10000) {
      lastManWarn = millis();
      Serial.println(F("LDR=TOI nhung dang MANUAL — chuyen AUTO de bat den"));
    }
  }

  if (rules.mode != MODE_AUTO) return;

  // Bơm tưới chuẩn: đất khô (ADC cao) → bật; đủ ẩm (ADC thấp) → tắt
  bool wantPump;
  if (pumpState == "ON") {
    wantPump = (lastSoilRaw > (int)rules.soilWetOff);   // tắt khi ADC <= wetOff
  } else {
    wantPump = (lastSoilRaw >= (int)rules.soilDryOn);   // bật khi ADC >= dryOn
  }
  if (rules.pumpLockOnEmpty && isWaterEmpty) wantPump = false;
  if ((pumpState == "ON") != wantPump) {
    setPump(wantPump);
    pubPump();
    Serial.printf("AUTO bom => %s (soil=%d dry>=%u wet<=%u empty=%d)\n",
                  wantPump ? "ON" : "OFF", lastSoilRaw,
                  rules.soilDryOn, rules.soilWetOff, isWaterEmpty ? 1 : 0);
  }

  // Quạt ON/OFF theo CO2 (mặc định >= 500 bật)
  bool fansOn = (fan1State == "ON" || fan2State == "ON");
  bool wantFans = wantFansFromCo2(fansOn);
  if (fansOn != wantFans) {
    setBothFans(wantFans);
    pubFan1();
    pubFan2();
    Serial.printf("AUTO fan => %s (CO2=%d thr on=%u off=%u)\n",
                  wantFans ? "ON" : "OFF", lastCo2Raw,
                  rules.co2FanOn, rules.co2FanOff);
  }
}

// ─── MQTT publish ────────────────────────────────────────────
void mqttPub(const char* device, const String& payload) {
  if (!mqttClient.isConnected()) return;
  mqttClient.publish(String("tele/") + device + "/status", payload, false, 0);
}

void pubOnline() { mqttPub(CHIP_ID, "online"); }
void pubTemp(float t) { mqttPub(DEV_TEMP, "{\"value\":" + String(t, 1) + "}"); }
void pubHumi(float h) { mqttPub(DEV_HUMI, "{\"value\":" + String(h, 1) + "}"); }
void pubSoil() { mqttPub(DEV_SOIL, "{\"value\":" + String(lastSoilRaw) + "}"); }
void pubWater() {
  mqttPub(DEV_WATER, String("{\"value\":") + lastWaterRaw +
                      ",\"empty\":" + String(isWaterEmpty ? 1 : 0) + "}");
}
void pubCo2() {
  String j = "{";
  j += "\"raw\":" + String(lastCo2Raw) + ",";
  j += "\"hw\":" + String(lastCo2AdcHw) + ",";
  j += "\"invert\":" + String(rules.co2AdcInvert ? 1 : 0) + ",";
  j += "\"ppm\":" + String(isnan(lastCo2Ppm) ? 0 : lastCo2Ppm, 0) + ",";
  j += "\"r0\":" + String(rules.mq135R0, 1);
  j += "}";
  mqttPub(DEV_CO2, j);
}
void pubLdr() {
  mqttPub(DEV_LDR, isDark ? "{\"value\":\"DARK\"}" : "{\"value\":\"LIGHT\"}");
}
void pubPump() {
  pumpState = pumpRead();
  mqttPub(DEV_PUMP, "{\"value\":\"" + pumpState + "\"}");
}
void pubLight() {
  lightState = lightRead();
  mqttPub(DEV_LIGHT, "{\"value\":\"" + lightState + "\"}");
}
void pubFan1() { mqttPub(DEV_FAN1, "{\"value\":\"" + fan1State + "\"}"); }
void pubFan2() { mqttPub(DEV_FAN2, "{\"value\":\"" + fan2State + "\"}"); }

void pubMode() {
  mqttPub(DEV_MODE, rules.mode == MODE_AUTO ? "{\"value\":\"AUTO\"}" : "{\"value\":\"MANUAL\"}");
}

void pubRules() {
  String j = "{";
  j += "\"mode\":\"" + String(rules.mode == MODE_AUTO ? "AUTO" : "MANUAL") + "\",";
  j += "\"soilDryOn\":" + String(rules.soilDryOn) + ",";
  j += "\"soilWetOff\":" + String(rules.soilWetOff) + ",";
  j += "\"waterEmptyThr\":" + String(rules.waterEmptyThr) + ",";
  j += "\"waterEmptyInvert\":" + String(rules.waterEmptyInvert ? 1 : 0) + ",";
  j += "\"co2FanOn\":" + String(rules.co2FanOn) + ",";
  j += "\"co2FanOff\":" + String(rules.co2FanOff) + ",";
  j += "\"co2AdcInvert\":" + String(rules.co2AdcInvert ? 1 : 0) + ",";
  j += "\"darkLightOn\":" + String(rules.darkLightOn ? 1 : 0) + ",";
  j += "\"buzzerOnEmpty\":" + String(rules.buzzerOnEmpty ? 1 : 0) + ",";
  j += "\"pumpLockOnEmpty\":" + String(rules.pumpLockOnEmpty ? 1 : 0) + ",";
  j += "\"mq135R0\":" + String(rules.mq135R0, 1);
  j += "}";
  mqttPub(DEV_RULES, j);
}

void pubVitals() {
  String j = "{";
  j += "\"temp\":" + String(isnan(lastTemp) ? 0 : lastTemp, 1) + ",";
  j += "\"humi\":" + String(isnan(lastHumi) ? 0 : lastHumi, 1) + ",";
  j += "\"soil\":" + String(lastSoilRaw) + ",";
  j += "\"water\":" + String(lastWaterRaw) + ",";
  j += "\"waterEmpty\":" + String(isWaterEmpty ? 1 : 0) + ",";
  j += "\"co2Raw\":" + String(lastCo2Raw) + ",";
  j += "\"co2Hw\":" + String(lastCo2AdcHw) + ",";
  j += "\"co2Ppm\":" + String(isnan(lastCo2Ppm) ? 0 : lastCo2Ppm, 0) + ",";
  j += "\"dark\":" + String(isDark ? 1 : 0) + ",";
  j += "\"pump\":\"" + pumpState + "\",";
  j += "\"light\":\"" + lightState + "\",";
  j += "\"fan1\":\"" + fan1State + "\",";
  j += "\"fan2\":\"" + fan2State + "\",";
  j += "\"mode\":\"" + String(rules.mode == MODE_AUTO ? "AUTO" : "MANUAL") + "\"";
  j += "}";
  mqttPub(DEV_VITALS, j);
}

// ─── JSON helpers ────────────────────────────────────────────
String jsonValueToken(const String& s, const char* key) {
  String k = String("\"") + key + "\"";
  int i = s.indexOf(k);
  if (i < 0) return "";
  int c = s.indexOf(':', i + k.length());
  if (c < 0) return "";
  int p = c + 1;
  while (p < (int)s.length() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
  int end = p;
  while (end < (int)s.length()) {
    char ch = s[end];
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ') break;
    end++;
  }
  return s.substring(p, end);
}

float jsonFloat(const String& s, const char* key, float defv) {
  String tok = jsonValueToken(s, key);
  if (tok.length() == 0) return defv;
  return tok.toFloat();
}

int jsonInt(const String& s, const char* key, int defv) {
  String tok = jsonValueToken(s, key);
  if (tok.length() == 0) return defv;
  return tok.toInt();
}

bool jsonHasKey(const String& s, const char* key) {
  String k = String("\"") + key + "\"";
  return s.indexOf(k) >= 0;
}

void handleModePayload(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.indexOf("AUTO") >= 0) rules.mode = MODE_AUTO;
  else if (cmd.indexOf("MANUAL") >= 0 || cmd.indexOf("MAN") >= 0) rules.mode = MODE_MANUAL;
  else if (cmd == "ON" || cmd == "1") rules.mode = MODE_AUTO;
  else if (cmd == "OFF" || cmd == "0") rules.mode = MODE_MANUAL;
  saveRules();
  pubMode();
  pubRules();
  pubVitals();
  if (rules.mode == MODE_AUTO) applyAutoLogic();
  beep(40);
  Serial.printf("Mode => %s\n", rules.mode == MODE_AUTO ? "AUTO" : "MANUAL");
}

void handleRulesPayload(String cmd) {
  cmd.trim();
  Serial.printf("Rules RX (%u): %s\n", (unsigned)cmd.length(), cmd.c_str());
  if (cmd.length() < 2) return;

  if (!jsonHasKey(cmd, "soilDryOn") && !jsonHasKey(cmd, "co2FanOn") &&
      (cmd.indexOf("AUTO") >= 0 || cmd.indexOf("MANUAL") >= 0) &&
      !cmd.startsWith("{")) {
    handleModePayload(cmd);
    return;
  }

  if (!(cmd.startsWith("{") || jsonHasKey(cmd, "soilDryOn") ||
        jsonHasKey(cmd, "co2FanOn") || jsonHasKey(cmd, "waterEmptyThr"))) {
    Serial.println(F("Rules: payload khong nhan dien JSON"));
    return;
  }

  if (jsonHasKey(cmd, "mode")) {
    String m = jsonValueToken(cmd, "mode");
    m.replace("\"", "");
    m.toUpperCase();
    if (m.indexOf("AUTO") >= 0) rules.mode = MODE_AUTO;
    else if (m.indexOf("MANUAL") >= 0) rules.mode = MODE_MANUAL;
  }
  if (jsonHasKey(cmd, "soilDryOn")) {
    int v = jsonInt(cmd, "soilDryOn", rules.soilDryOn);
    if (v < 0) v = 0;
    if (v > ADC_SCALE_MAX) v = ADC_SCALE_MAX;
    rules.soilDryOn = (uint16_t)v;
  }
  if (jsonHasKey(cmd, "soilWetOff")) {
    int v = jsonInt(cmd, "soilWetOff", rules.soilWetOff);
    if (v < 0) v = 0;
    if (v > ADC_SCALE_MAX) v = ADC_SCALE_MAX;
    rules.soilWetOff = (uint16_t)v;
  }
  if (jsonHasKey(cmd, "waterEmptyThr")) {
    int v = jsonInt(cmd, "waterEmptyThr", rules.waterEmptyThr);
    if (v < 0) v = 0;
    if (v > ADC_SCALE_MAX) v = ADC_SCALE_MAX;
    rules.waterEmptyThr = (uint16_t)v;
  }
  if (jsonHasKey(cmd, "waterEmptyInvert")) {
    rules.waterEmptyInvert = jsonInt(cmd, "waterEmptyInvert", rules.waterEmptyInvert) ? 1 : 0;
  }
  if (jsonHasKey(cmd, "co2FanOn")) {
    int v = jsonInt(cmd, "co2FanOn", rules.co2FanOn);
    if (v < 0) v = 0;
    if (v > ADC_SCALE_MAX) v = ADC_SCALE_MAX;
    rules.co2FanOn = (uint16_t)v;
  }
  if (jsonHasKey(cmd, "co2FanOff")) {
    int v = jsonInt(cmd, "co2FanOff", rules.co2FanOff);
    if (v < 0) v = 0;
    if (v > ADC_SCALE_MAX) v = ADC_SCALE_MAX;
    rules.co2FanOff = (uint16_t)v;
  }
  if (jsonHasKey(cmd, "co2AdcInvert")) {
    rules.co2AdcInvert = jsonInt(cmd, "co2AdcInvert", rules.co2AdcInvert) ? 1 : 0;
  }
  if (jsonHasKey(cmd, "darkLightOn")) rules.darkLightOn = jsonInt(cmd, "darkLightOn", rules.darkLightOn) ? 1 : 0;
  if (jsonHasKey(cmd, "buzzerOnEmpty")) rules.buzzerOnEmpty = jsonInt(cmd, "buzzerOnEmpty", rules.buzzerOnEmpty) ? 1 : 0;
  if (jsonHasKey(cmd, "pumpLockOnEmpty")) rules.pumpLockOnEmpty = jsonInt(cmd, "pumpLockOnEmpty", rules.pumpLockOnEmpty) ? 1 : 0;
  if (jsonHasKey(cmd, "mq135R0")) {
    float r0 = jsonFloat(cmd, "mq135R0", rules.mq135R0);
    if (r0 >= 100.0f) rules.mq135R0 = r0;
  }

  // Đất: dryOn phải cao hơn wetOff
  if (rules.soilDryOn < rules.soilWetOff) {
    uint16_t t = rules.soilDryOn;
    rules.soilDryOn = rules.soilWetOff;
    rules.soilWetOff = t;
  }
  if (rules.co2FanOff >= rules.co2FanOn) {
    if (rules.co2FanOn > 50) rules.co2FanOff = rules.co2FanOn - 50;
    else rules.co2FanOff = 0;
  }

  saveRules();
  pubRules();
  pubMode();
  pubVitals();
  if (rules.mode == MODE_AUTO) applyAutoLogic();
  beep(30);
  Serial.printf("Rules OK soil dry>=%u wet<=%u co2Fan on=%u off=%u\n",
                rules.soilDryOn, rules.soilWetOff,
                rules.co2FanOn, rules.co2FanOff);
}

bool parseOnOff(const String& cmd, bool& outOn) {
  String up = cmd;
  up.trim();
  up.toUpperCase();
  if (up == "ON" || up == "1" || up == "TRUE") { outOn = true; return true; }
  if (up == "OFF" || up == "0" || up == "FALSE") { outOn = false; return true; }
  if (up.startsWith("{") && jsonHasKey(up, "value")) {
    String v = jsonValueToken(up, "value");
    v.replace("\"", "");
    v.toUpperCase();
    if (v == "ON" || v == "1") { outOn = true; return true; }
    if (v == "OFF" || v == "0") { outOn = false; return true; }
  }
  return false;
}

void mqttCallback(const String& topicStr, const String& payload) {
  String topic = topicStr;
  String cmd = payload;
  cmd.trim();
  Serial.printf("MQTT %s <= %s\n", topic.c_str(), cmd.c_str());

  if (topic.indexOf(DEV_MODE) >= 0) {
    handleModePayload(cmd);
  } else if (topic.indexOf(DEV_RULES) >= 0) {
    if (topic.indexOf("/GET") >= 0 || cmd.equalsIgnoreCase("GET") || cmd.equalsIgnoreCase("STATUS")) {
      pubRules();
      pubMode();
    } else {
      handleRulesPayload(cmd);
    }
  } else if (topic.indexOf(DEV_CO2_CAL) >= 0) {
    String up = cmd; up.toUpperCase();
    if (up.indexOf("CAL") >= 0 || up == "1" || up == "ON" || up == "SET" || up.length() == 0) {
      calibrateMq135R0();
      pubCo2();
      pubRules();
      pubVitals();
      beep(60);
    }
  } else if (topic.indexOf(DEV_PUMP) >= 0) {
    if (rules.mode != MODE_MANUAL) { Serial.println(F("Bo qua pump: AUTO")); return; }
    bool on;
    if (parseOnOff(cmd, on)) {
      if (on && rules.pumpLockOnEmpty && isWaterEmpty) {
        Serial.println(F("Pump locked: water empty"));
        setPump(false);
      } else {
        setPump(on);
      }
      pubPump();
      pubVitals();
    }
  } else if (topic.indexOf(DEV_LIGHT) >= 0) {
    if (rules.mode != MODE_MANUAL) { Serial.println(F("Bo qua light: AUTO")); return; }
    bool on;
    if (parseOnOff(cmd, on)) { setLight(on); pubLight(); pubVitals(); }
  } else if (topic.indexOf(DEV_FAN1) >= 0 || topic.indexOf(DEV_FAN2) >= 0) {
    if (rules.mode != MODE_MANUAL) { Serial.println(F("Bo qua fan: AUTO")); return; }
    bool on;
    if (parseOnOff(cmd, on)) {
      setBothFans(on);
      pubFan1();
      pubFan2();
      pubVitals();
    }
  }
}

void reconnectMQTT() {
  if (mqttClient.isConnected()) return;
  Serial.printf("MQTT WSS ... ");
  uint64_t mac = ESP.getEfuseMac();
  String clientId = "ESP32-Farm-" + String((uint32_t)(mac >> 16), HEX);
  mqttClient.setWill(String("tele/") + CHIP_ID + "/status", "offline", true, 1);
  if (!mqttClient.connect(clientId, "", "")) {
    Serial.println(F("fail"));
    return;
  }
  Serial.println(F("OK"));
  pubOnline();
  wsClient.loop(); delay(10); yield();

  auto sub = [](const String& topic) {
    mqttClient.subscribe(topic, [topic](const char* p, unsigned int n) {
      String cmd; for (unsigned int i = 0; i < n; i++) cmd += p[i];
      mqttCallback(topic, cmd);
    });
    wsClient.loop(); delay(10); yield();
  };

  sub(String("cmnd/") + DEV_PUMP + "/POWER");
  sub(String("cmnd/") + DEV_LIGHT + "/POWER");
  sub(String("cmnd/") + DEV_FAN1 + "/POWER");
  sub(String("cmnd/") + DEV_FAN2 + "/POWER");
  sub(String("cmnd/") + DEV_MODE + "/POWER");
  sub(String("cmnd/") + DEV_RULES + "/SET");
  sub(String("cmnd/") + DEV_RULES + "/POWER");
  sub(String("cmnd/") + DEV_RULES + "/GET");
  sub(String("cmnd/") + DEV_CO2_CAL + "/SET");
  sub(String("cmnd/") + DEV_CO2_CAL + "/POWER");

  pubMode();
  pubRules();
  pubVitals();
  pubPump(); wsClient.loop(); delay(10); yield();
  pubLight();
  pubFan1();
  pubFan2();
  pubSoil();
  pubWater();
  pubCo2();
  pubLdr();
}

// ─── Portal ──────────────────────────────────────────────────
const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartFarm</title>
<style>body{font-family:sans-serif;background:#0a0e1a;color:#e2e8f0;display:flex;justify-content:center;padding:20px}
.c{max-width:400px;width:100%}input{width:100%;padding:10px;margin-bottom:10px;border-radius:5px;border:none;box-sizing:border-box}
button{padding:10px;width:100%;background:#0ea5e9;color:#fff;border:none;border-radius:5px;font-weight:bold}</style>
</head><body><div class="c"><h2>SmartFarm WiFi</h2>
<button onclick="scan()" style="margin-bottom:10px;background:#6366f1">Quet mang</button>
<div id="w"></div>
<input id="s" placeholder="Ten WiFi"><input type="password" id="p" placeholder="Mat khau">
<button onclick="conn()">Ket noi</button><div id="st"></div></div>
<script>
function scan(){document.getElementById('w').innerHTML='Dang quet...';fetch('/scan').then(r=>r.json()).then(l=>{
document.getElementById('w').innerHTML=l.map(n=>`<div style="padding:6px;background:#1e293b;margin:4px 0;border-radius:6px" onclick="document.getElementById('s').value='${n.ssid}'">${n.ssid} (${n.rssi})</div>`).join('')
}).catch(()=>document.getElementById('w').innerHTML='Loi')}
function conn(){const s=document.getElementById('s').value,p=document.getElementById('p').value;if(!s)return alert('Nhap WiFi');
const st=document.getElementById('st');st.innerHTML='Dang ket noi...';
fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)})
.then(r=>r.json()).then(d=>{st.innerHTML=d.ok?'OK restart...':'Loi: '+(d.message||'')})}
</script></body></html>
)rawhtml";

void handleNotFound() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}
void handleRoot() { webServer.send_P(200, "text/html", PORTAL_HTML); }
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}
void handleConnect() {
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  if (!ssid.length()) {
    webServer.send(200, "application/json", "{\"ok\":false,\"message\":\"empty\"}");
    return;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    addOrUpdateWifi(ssid, pass);
    webServer.send(200, "application/json", "{\"ok\":true}");
    delay(800);
    ESP.restart();
  } else {
    WiFi.disconnect();
    webServer.send(200, "application/json", "{\"ok\":false,\"message\":\"fail\"}");
  }
}
void startPortal() {
  portalActive = true;
  portalUiDirty = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", apIP);
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/scan", HTTP_GET, handleScan);
  webServer.on("/connect", HTTP_POST, handleConnect);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  Serial.println(F("WiFi PORTAL SSID=SmartFarm  http://192.168.4.1"));
  Serial.println(F("Giu BOOT 5s de vao portal (da vao)"));
  uiDrawPortal();
  portalUiDirty = false;
  beep(80);
  delay(60);
  beep(80);
  delay(60);
  beep(80);
}

/** Nút BOOT (GPIO0) active LOW — giữ 5s → xóa WiFi đã lưu + vào portal cấu hình. */
void checkBootHoldForPortal() {
  if (digitalRead(PIN_BOOT) == LOW) {
    if (bootPressStart == 0) {
      bootPressStart = millis();
      bootHoldDone = false;
    } else if (!bootHoldDone && (millis() - bootPressStart >= BOOT_HOLD_MS)) {
      bootHoldDone = true;
      Serial.println(F("BOOT giu 5s -> WiFi portal"));
      wifiCount = 0;
      saveWifiList();
      if (!portalActive) startPortal();
    }
  } else {
    bootPressStart = 0;
    bootHoldDone = false;
  }
}

void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== SmartFarm ESP32 Greenhouse ==="));
  Serial.println(F("MQ135 AO->GPIO34 | Soil->35 | Water->32 | LDR->33 | DHT->14"));
  Serial.println(F("L298 ENA16=2quat ENB17=bom | Light13 | Buzzer25"));
  Serial.println(F("Giu nut BOOT 5 giay de cau hinh WiFi"));
  Serial.printf("TFT ST7789 SCLK=%d MOSI=%d MISO=%d DC=%d RST=%d CS=hardwired\n",
                PIN_TFT_SCLK, PIN_TFT_MOSI, PIN_TFT_MISO, PIN_TFT_DC, PIN_TFT_RST);

  analogReadResolution(12);

  l298Init();
  pinMode(PIN_LIGHT, OUTPUT); setLight(false);
  pinMode(PIN_LDR, INPUT_PULLUP);

  dht.begin();
  loadRules();
  uiInit();

  // Đọc nước lần đầu — không báo hết/còi cho tới khi debounce
  lastWaterRaw = readAdcScaled(PIN_WATER);
  lastSoilRaw = readAdcScaled(PIN_SOIL);
  waterEmptyHits = 0;
  waterOkHits = 0;
  isWaterEmpty = false;
  setPump(false);
  setBothFans(false);
  buzzerWrite(false);

  beep(120);
  delay(80);
  beep(120);
  buzzerWrite(false);

  wsClient.beginSSL(MQTT_HOST, MQTT_PORT, MQTT_PATH);
  wsClient.setExtraHeaders("Sec-WebSocket-Protocol: mqtt");
  mqttClient.begin(wsClient);

  loadWifiList();

  if (!(wifiCount > 0 && connectBestWifi())) {
    startPortal();
  } else {
    // Xóa "Dang khoi dong..." — vào trang giám sát ngay
    uiEnterHome();
    Serial.println(F("UI home san sang"));
  }
}

void loop() {
  checkBootHoldForPortal();

  if (portalActive) {
    buzzerWrite(false);
    dnsServer.processNextRequest();
    webServer.handleClient();
    uiDraw();
    return;
  }

  unsigned long now = millis();

  // ── Cảm biến + AUTO + TFT luôn chạy (kể cả mất WiFi) ──
  if (now - lastFastRead >= SENSOR_FAST_MS) {
    lastFastRead = now;

    lastSoilRaw = readAdcScaled(PIN_SOIL);
    lastWaterRaw = readAdcScaled(PIN_WATER);
    lastCo2AdcHw = readAdcScaled(PIN_CO2);
    lastCo2Raw = co2IndexFromHw(lastCo2AdcHw);  // đảo nếu AO ngược
    float rs = mq135RsFromAdc(lastCo2AdcHw);
    lastCo2Ppm = mq135PpmFromRs(rs);

    bool wasEmpty = isWaterEmpty;
    waterUpdateEmptyState(lastWaterRaw);
    if (wasEmpty != isWaterEmpty && WiFi.status() == WL_CONNECTED) {
      pubWater();
      pubVitals();
    }

    isSoilDry = (lastSoilRaw >= (int)rules.soilDryOn);

    bool dark = (digitalRead(PIN_LDR) == HIGH);
    if (dark != isDark) {
      isDark = dark;
      if (WiFi.status() == WL_CONNECTED) pubLdr();
    }

    applyAutoLogic();
  }

  // Còi cập nhật mỗi vòng loop (pulse chính xác; tắt ngay khi có nước)
  buzzerEmptyUpdate();

  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(t) && !isnan(h)) {
      lastTemp = t;
      lastHumi = h;
    }
    applyAutoLogic();
    if (WiFi.status() == WL_CONNECTED && mqttClient.isConnected()) {
      if (!isnan(lastTemp) && !isnan(lastHumi)) {
        pubTemp(lastTemp);
        pubHumi(lastHumi);
      }
      pubSoil();
      pubWater();
      pubCo2();
      pubVitals();
    }
  }

  uiDraw();  // luôn cập nhật màn trước phần mạng

  // ── WiFi / MQTT ──
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastReconnect >= RECONNECT_MS) {
      lastReconnect = now;
      Serial.println(F("WiFi mat — thu ket noi lai..."));
      if (connectWifiQuick() || connectBestWifi()) wifiRetries = 0;
      else if (++wifiRetries >= 5) { startPortal(); wifiRetries = 0; }
    }
    return;
  }
  wifiRetries = 0;

  wsClient.loop();
  mqttClient.update();
  if (!mqttClient.isConnected() && now - lastReconnect >= 5000) {
    lastReconnect = now;
    reconnectMQTT();
  }

  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    pubOnline();
    pubMode();
  }
}
