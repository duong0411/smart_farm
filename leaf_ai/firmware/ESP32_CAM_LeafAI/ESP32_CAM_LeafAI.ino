/**
 * ESP32-CAM — Tiny Leaf Disease (INT8) + WiFiManager portal
 * Model: Tiny CNN 96x96 — classes: dom_la_goc / gi_sat_la / khoe_manh
 *
 * Board: AI Thinker ESP32-CAM
 * - Boot: thử WiFi đã lưu → fail thì SoftAP portal "ESP32-CAM"
 * - HTTP: http://<ip>/  xem stream + kết quả nhận diện
 */
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>

#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"
#include "img_converters.h"

static const char *AP_NAME = "ESP32-CAM";
static const uint32_t WIFI_CONNECT_TIMEOUT_S = 20;
static const uint32_t WIFI_LOST_REBOOT_MS = 20000;

// Tensor arena trong PSRAM nếu có
static const int kArenaSize = 80 * 1024;
static uint8_t *tensorArena = nullptr;

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;

WebServer server(80);
WiFiManager wm;

String lastLabel = "—";
float lastScore = 0;
unsigned long lastInferMs = 0;
unsigned long wifiLostSince = 0;
bool camOk = false;

bool initCamera() {
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
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA;  // 160x120
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.jpeg_quality = 12;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init FAIL");
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_QQVGA);
    s->set_vflip(s, 0);
  }
  Serial.println("Camera OK (RGB565 QQVGA)");
  return true;
}

bool initModel() {
  if (psramFound()) {
    tensorArena = (uint8_t *)ps_malloc(kArenaSize);
    Serial.println("Arena in PSRAM");
  } else {
    tensorArena = (uint8_t *)malloc(kArenaSize);
    Serial.println("Arena in DRAM");
  }
  if (!tensorArena) {
    Serial.println("Arena alloc FAIL");
    return false;
  }

  model = tflite::GetModel(leaf_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema mismatch");
    return false;
  }

  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(model, resolver, tensorArena, kArenaSize);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors FAIL");
    return false;
  }
  input = interpreter->input(0);
  output = interpreter->output(0);
  Serial.printf("Model OK  in:%d out:%d  type_in=%d\n",
                input->bytes, output->bytes, input->type);
  return true;
}

// Lấy pixel RGB565 → R,G,B 0..255
static inline void rgb565_to_rgb(uint16_t p, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = ((p >> 11) & 0x1F) << 3;
  g = ((p >> 5) & 0x3F) << 2;
  b = (p & 0x1F) << 3;
}

bool runInference() {
  if (!interpreter || !camOk) return false;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb || fb->format != PIXFORMAT_RGB565) {
    if (fb) esp_camera_fb_return(fb);
    return false;
  }

  const int srcW = fb->width;   // 160
  const int srcH = fb->height;  // 120
  const int dst = LEAF_IMG_SIZE;
  const uint16_t *src = (const uint16_t *)fb->buf;

  // Quant params
  float scale = input->params.scale;
  int zp = input->params.zero_point;
  if (scale == 0) scale = 1.0f / 255.0f;

  // Nearest-neighbor resize 160x120 → 96x96 + int8 quant (input was trained 0..1)
  int8_t *in = input->data.int8;
  for (int y = 0; y < dst; y++) {
    int sy = y * srcH / dst;
    for (int x = 0; x < dst; x++) {
      int sx = x * srcW / dst;
      uint16_t pix = src[sy * srcW + sx];
      uint8_t r, g, b;
      rgb565_to_rgb(pix, r, g, b);
      float rf = r / 255.0f;
      float gf = g / 255.0f;
      float bf = b / 255.0f;
      int idx = (y * dst + x) * 3;
      in[idx + 0] = (int8_t)constrain((int)roundf(rf / scale) + zp, -128, 127);
      in[idx + 1] = (int8_t)constrain((int)roundf(gf / scale) + zp, -128, 127);
      in[idx + 2] = (int8_t)constrain((int)roundf(bf / scale) + zp, -128, 127);
    }
  }
  esp_camera_fb_return(fb);

  unsigned long t0 = millis();
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Invoke FAIL");
    return false;
  }
  lastInferMs = millis() - t0;

  // Dequantize output softmax-ish int8
  float outScale = output->params.scale;
  int outZp = output->params.zero_point;
  int best = 0;
  float bestScore = -1e9f;
  for (int i = 0; i < LEAF_NUM_CLASSES; i++) {
    float v = (output->data.int8[i] - outZp) * outScale;
    if (v > bestScore) {
      bestScore = v;
      best = i;
    }
  }
  lastLabel = LEAF_LABELS[best];
  lastScore = bestScore;
  Serial.printf("=> %s (%.3f) %lums\n", LEAF_LABELS[best], bestScore, lastInferMs);
  return true;
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
                  "<meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>ESP32 Leaf AI</title>"
                  "<style>body{font-family:sans-serif;background:#0f1a12;color:#e8f5e9;margin:0;padding:16px}"
                  ".card{background:#1b2e22;border-radius:14px;padding:16px;max-width:480px;margin:auto}"
                  "h1{font-size:1.25rem;margin:0 0 8px} .lab{font-size:1.6rem;color:#a5d6a7;font-weight:700}"
                  "img{width:100%;border-radius:10px;margin-top:12px;background:#000}"
                  "button{width:100%;padding:12px;border:0;border-radius:10px;background:#66bb6a;color:#063;font-weight:700;margin-top:10px}"
                  ".meta{opacity:.8;font-size:.9rem;margin-top:8px}</style></head><body><div class=card>");
  html += F("<h1>Nhận diện bệnh lá (TinyML)</h1>");
  html += "<div class=lab>" + lastLabel + "</div>";
  html += "<div class=meta>Điểm: " + String(lastScore, 3) + " · Infer: " + String(lastInferMs) + " ms</div>";
  html += F("<button onclick=\"fetch('/infer').then(()=>location.reload())\">Chụp & nhận diện</button>");
  html += F("<img id=shot src='/jpg?_='>");
  html += F("<div class=meta>Classes: dom_la_goc · gi_sat_la · khoe_manh<br>"
            "Mất WiFi / đổi mạng → SoftAP portal ESP32-CAM</div></div>"
            "<script>document.getElementById('shot').src='/jpg?_='+Date.now()</script>"
            "</body></html>");
  server.send(200, "text/html", html);
}

void handleInfer() {
  bool ok = runInference();
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") +
                  ",\"label\":\"" + lastLabel + "\",\"score\":" + String(lastScore, 4) +
                  ",\"ms\":" + String(lastInferMs) + "}");
}

void handleJpg() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "capture fail");
    return;
  }
  if (fb->format == PIXFORMAT_JPEG) {
    server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return;
  }
  uint8_t *jpg_buf = nullptr;
  size_t jpg_len = 0;
  bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  if (!ok || !jpg_buf) {
    server.send(500, "text/plain", "jpg encode fail");
    return;
  }
  server.send_P(200, "image/jpeg", (const char *)jpg_buf, jpg_len);
  free(jpg_buf);
}

void handleStatus() {
  String j = "{";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"ssid\":\"" + WiFi.SSID() + "\",";
  j += "\"label\":\"" + lastLabel + "\",";
  j += "\"score\":" + String(lastScore, 4) + ",";
  j += "\"ms\":" + String(lastInferMs);
  j += "}";
  server.send(200, "application/json", j);
}

bool startWifiOrPortal() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  wm.setDebugOutput(true);
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  wm.setConfigPortalBlocking(true);
  wm.setTitle("ESP32-CAM Leaf AI");
  wm.setDarkMode(true);
  bool ok = wm.autoConnect(AP_NAME);
  if (!ok) ok = wm.startConfigPortal(AP_NAME);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-CAM Leaf TinyML ===");

  camOk = initCamera();
  if (!initModel()) {
    Serial.println("Model init failed");
  }

  Serial.println("WiFi: thu mang da luu, fail -> portal ESP32-CAM");
  if (!startWifiOrPortal()) {
    delay(1500);
    ESP.restart();
  }
  WiFi.setSleep(false);
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/infer", handleInfer);
  server.on("/jpg", handleJpg);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("HTTP OK — mo trinh duyet /infer");

  // Infer lần đầu
  if (camOk) runInference();
}

void loop() {
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostSince = 0;
  } else {
    if (wifiLostSince == 0) wifiLostSince = millis();
    WiFi.reconnect();
    if (millis() - wifiLostSince > WIFI_LOST_REBOOT_MS) {
      Serial.println("Mat WiFi lau -> restart portal");
      delay(200);
      ESP.restart();
    }
  }
  delay(2);
}
