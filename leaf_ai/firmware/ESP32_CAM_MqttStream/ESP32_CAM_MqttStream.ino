/**
 * ESP32-CAM — Continuous MQTT JPEG stream (AES-128-CBC binary)
 *
 * SoftAP: ESP32-CAM-MQTT
 * Broker: wss://mqtt.duynguyen.io.vn:443/mqtt
 *
 * Stream liên tục (camera luôn bật), 1 message / frame:
 *   tele/{id}_cam/frame   binary SFC1|w|h|iv[16]|ciphertext
 *   tele/{id}_cam/status  online|offline
 *   cmnd/{id}_cam/STREAM  ON|OFF
 *   cmnd/{id}_cam/SNAP
 *
 * Libs: WiFiManager, WebSockets, MQTTPubSubClient
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <MQTTPubSubClient.h>
#include <esp_camera.h>
#include <esp_random.h>
#include "mbedtls/aes.h"

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ~4 KB đủ JPEG QQVGA đã nén + header SFC1; để dành RAM cho camera DMA
using MqttClient4k = arduino::mqtt::PubSubClient<4096>;

static const char *AP_NAME = "ESP32-CAM-MQTT";
static const char *MQTT_HOST = "mqtt.duynguyen.io.vn";
static const uint16_t MQTT_PORT = 443;
static const char *MQTT_PATH = "/mqtt";

static const char *DEFAULT_AES_HEX = "536d6172744661726d43616d4b657921";  // SmartFarmCamKey!
static const char *DEFAULT_CHIP = "123";

// Stream liên tục ~4 fps (có thể chỉnh portal)
static const uint32_t FRAME_MS_DEFAULT = 250;
static const uint32_t FRAME_MS_MIN = 100;
static const uint32_t WIFI_LOST_REBOOT_MS = 25000;
static const uint32_t MQTT_RETRY_MS = 8000;

// Packet: S F C 1 | w u16le | h u16le | iv[16] | cipher
static const uint8_t MAGIC[4] = {'S', 'F', 'C', '1'};
static const size_t HDR_LEN = 4 + 2 + 2 + 16;

String chipId = DEFAULT_CHIP;
uint8_t aesKey[16];
uint32_t frameIntervalMs = FRAME_MS_DEFAULT;
bool streamOn = true;
bool cameraReady = false;
bool snapOnce = false;

Preferences prefs;
WiFiManager wm;
WebSocketsClient wsClient;
MqttClient4k mqttClient;

unsigned long lastFrame = 0;
unsigned long lastMqttTry = 0;
unsigned long wifiLostSince = 0;
uint32_t frameCounter = 0;

String topicTele(const char *suffix) {
  return String("tele/") + chipId + "_cam/" + suffix;
}
String topicCmnd(const char *suffix) {
  return String("cmnd/") + chipId + "_cam/" + suffix;
}

bool parseAesHex(const String &hexIn, uint8_t out[16]) {
  String hex = hexIn;
  hex.trim();
  hex.replace(" ", "");
  if (hex.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    char a = hex.charAt(i * 2);
    char b = hex.charAt(i * 2 + 1);
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nib(a), lo = nib(b);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool aes128CbcEncrypt(const uint8_t *plain, size_t plainLen,
                      const uint8_t key[16], const uint8_t iv[16],
                      uint8_t **outCipher, size_t *outLen) {
  size_t pad = 16 - (plainLen % 16);
  size_t total = plainLen + pad;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return false;
  memcpy(buf, plain, plainLen);
  memset(buf + plainLen, (uint8_t)pad, pad);

  uint8_t ivCopy[16];
  memcpy(ivCopy, iv, 16);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
    mbedtls_aes_free(&ctx);
    free(buf);
    return false;
  }
  if (mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, total, ivCopy, buf, buf) != 0) {
    mbedtls_aes_free(&ctx);
    free(buf);
    return false;
  }
  mbedtls_aes_free(&ctx);
  *outCipher = buf;
  *outLen = total;
  return true;
}

bool initCamera() {
  if (cameraReady) return true;

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QQVGA;  // 160x120 — nén JPEG nhỏ, stream mượt
  config.jpeg_quality = 28;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init 0x%x — retry 1FB\n", (unsigned)err);
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.println("Camera FAIL");
      return false;
    }
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_QQVGA);
    s->set_quality(s, 28);
    s->set_saturation(s, -1);
  }

  // Drain 1 frame
  camera_fb_t *probe = esp_camera_fb_get();
  if (probe) esp_camera_fb_return(probe);

  cameraReady = true;
  Serial.println("Camera OK — continuous JPEG stream");
  return true;
}

camera_fb_t *grabLatest() {
  if (!cameraReady && !initCamera()) return nullptr;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb || fb->len < 200) {
    if (fb) esp_camera_fb_return(fb);
    return nullptr;
  }
  return fb;
}

void mqttPublishStr(const String &topic, const String &payload) {
  if (!mqttClient.isConnected()) return;
  mqttClient.publish(topic, payload, false, 0);
  wsClient.loop();
}

void handleMqttCommand(const String &topic, const String &payload) {
  String p = payload;
  p.trim();
  p.toUpperCase();
  if (topic.endsWith("/STREAM")) {
    streamOn = (p == "ON" || p == "1" || p == "TRUE");
    Serial.println(streamOn ? "Stream ON" : "Stream OFF");
  } else if (topic.endsWith("/SNAP")) {
    snapOnce = true;
  }
}

void reconnectMqtt() {
  if (mqttClient.isConnected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print("MQTT WSS ... ");
  uint64_t mac = ESP.getEfuseMac();
  String clientId = "CAM" + chipId + String((uint32_t)(mac >> 16), HEX);
  if (clientId.length() > 22) clientId = clientId.substring(0, 22);

  mqttClient.setWill(topicTele("status"), "offline", true, 1);
  if (!mqttClient.connect(clientId, "", "")) {
    Serial.println("fail");
    return;
  }
  Serial.println("OK");
  mqttPublishStr(topicTele("status"), "online");

  auto sub = [](const String &t) {
    mqttClient.subscribe(t, [t](const char *p, unsigned int n) {
      String cmd;
      for (unsigned int i = 0; i < n; i++) cmd += p[i];
      handleMqttCommand(t, cmd);
    });
    wsClient.loop();
    delay(5);
  };
  sub(topicCmnd("STREAM"));
  sub(topicCmnd("SNAP"));
}

bool publishLiveFrame(camera_fb_t *fb) {
  if (!fb || !mqttClient.isConnected()) return false;

  uint8_t iv[16];
  esp_fill_random(iv, sizeof(iv));

  uint8_t *cipher = nullptr;
  size_t cipherLen = 0;
  if (!aes128CbcEncrypt(fb->buf, fb->len, aesKey, iv, &cipher, &cipherLen)) {
    return false;
  }

  size_t total = HDR_LEN + cipherLen;
  if (total > 3900) {  // dưới buffer MQTT 4k
    free(cipher);
    Serial.printf("frame too big jpg=%u\n", fb->len);
    return false;
  }

  uint8_t *pkt = (uint8_t *)malloc(total);
  if (!pkt) {
    free(cipher);
    return false;
  }

  memcpy(pkt, MAGIC, 4);
  pkt[4] = (uint8_t)(fb->width & 0xFF);
  pkt[5] = (uint8_t)((fb->width >> 8) & 0xFF);
  pkt[6] = (uint8_t)(fb->height & 0xFF);
  pkt[7] = (uint8_t)((fb->height >> 8) & 0xFF);
  memcpy(pkt + 8, iv, 16);
  memcpy(pkt + HDR_LEN, cipher, cipherLen);
  free(cipher);

  bool ok = mqttClient.publish(topicTele("frame"), pkt, total, false, 0);
  free(pkt);
  wsClient.loop();

  frameCounter++;
  if ((frameCounter % 20) == 1) {
    Serial.printf("live #%u jpg=%u cipher=%u\n", frameCounter, fb->len, (unsigned)cipherLen);
  }
  return ok;
}

bool streamOnce() {
  camera_fb_t *fb = grabLatest();
  if (!fb) return false;
  bool ok = publishLiveFrame(fb);
  esp_camera_fb_return(fb);
  return ok;
}

void loadPrefs() {
  prefs.begin("cammqtt", true);
  chipId = prefs.getString("chip", DEFAULT_CHIP);
  frameIntervalMs = prefs.getUInt("interval", FRAME_MS_DEFAULT);
  String aesHex = prefs.getString("aes", DEFAULT_AES_HEX);
  prefs.end();
  if (chipId.length() < 1) chipId = DEFAULT_CHIP;
  // Migrate prefs cũ (chụp từng frame 1.5–15s) → stream liên tục
  if (frameIntervalMs > 500) {
    frameIntervalMs = FRAME_MS_DEFAULT;
    prefs.begin("cammqtt", false);
    prefs.putUInt("interval", frameIntervalMs);
    prefs.end();
  }
  if (frameIntervalMs < FRAME_MS_MIN) frameIntervalMs = FRAME_MS_MIN;
  if (frameIntervalMs > 60000) frameIntervalMs = 60000;
  if (!parseAesHex(aesHex, aesKey)) parseAesHex(DEFAULT_AES_HEX, aesKey);
}

bool startWifiPortal() {
  loadPrefs();

  char chipBuf[32];
  char aesBuf[40];
  char intBuf[12];
  chipId.toCharArray(chipBuf, sizeof(chipBuf));
  String aesHex;
  prefs.begin("cammqtt", true);
  aesHex = prefs.getString("aes", DEFAULT_AES_HEX);
  prefs.end();
  aesHex.toCharArray(aesBuf, sizeof(aesBuf));
  snprintf(intBuf, sizeof(intBuf), "%u", (unsigned)frameIntervalMs);

  WiFiManagerParameter p_chip("chip", "Chip ID (vd 123)", chipBuf, 24);
  WiFiManagerParameter p_aes("aes", "AES-128 key hex 32", aesBuf, 36);
  WiFiManagerParameter p_int("interval", "Stream interval ms (100+)", intBuf, 10);

  wm.addParameter(&p_chip);
  wm.addParameter(&p_aes);
  wm.addParameter(&p_int);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(25);
  wm.setTitle("ESP32-CAM MQTT");
  wm.setDarkMode(true);

  bool ok = wm.autoConnect(AP_NAME);
  if (!ok) ok = wm.startConfigPortal(AP_NAME);

  String newChip = p_chip.getValue();
  String newAes = p_aes.getValue();
  String newInt = p_int.getValue();

  prefs.begin("cammqtt", false);
  if (newChip.length() > 0) {
    chipId = newChip;
    chipId.trim();
    prefs.putString("chip", chipId);
  }
  if (parseAesHex(newAes, aesKey)) prefs.putString("aes", newAes);
  uint32_t iv = (uint32_t)newInt.toInt();
  if (iv >= FRAME_MS_MIN && iv <= 60000) {
    frameIntervalMs = iv;
    prefs.putUInt("interval", frameIntervalMs);
  }
  prefs.end();

  Serial.println("Chip: " + chipId);
  Serial.printf("Stream interval: %u ms\n", (unsigned)frameIntervalMs);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-CAM continuous MQTT stream ===");

  if (!startWifiPortal()) {
    delay(1500);
    ESP.restart();
  }
  WiFi.setSleep(false);
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  wsClient.beginSSL(MQTT_HOST, MQTT_PORT, MQTT_PATH);
  wsClient.setExtraHeaders("Sec-WebSocket-Protocol: mqtt");
  mqttClient.begin(wsClient);

  reconnectMqtt();
  initCamera();  // giữ camera bật suốt stream
  lastFrame = 0;
}

void loop() {
  wsClient.loop();
  mqttClient.update();

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostSince = 0;
    if (!mqttClient.isConnected()) {
      if (millis() - lastMqttTry >= MQTT_RETRY_MS) {
        lastMqttTry = millis();
        reconnectMqtt();
      }
    } else {
      bool due = streamOn && (millis() - lastFrame >= frameIntervalMs);
      if (snapOnce || due) {
        snapOnce = false;
        lastFrame = millis();
        streamOnce();
      }
    }
  } else {
    if (wifiLostSince == 0) wifiLostSince = millis();
    WiFi.reconnect();
    if (millis() - wifiLostSince > WIFI_LOST_REBOOT_MS) {
      delay(200);
      ESP.restart();
    }
  }
  delay(1);
}
