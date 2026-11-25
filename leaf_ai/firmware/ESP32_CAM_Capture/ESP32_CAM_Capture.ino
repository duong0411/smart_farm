/**
 * ESP32-CAM — chỉ thu ảnh + gửi server AI (không chạy model trên board)
 *
 * - WiFiManager SoftAP "ESP32-CAM": khách cấu hình WiFi nhà
 * - Custom field: URL server (vd https://xxx.trycloudflare.com)
 * - POST multipart JPEG → /api/leaf/predict → in Serial + trang web local
 *
 * Board: AI Thinker ESP32-CAM
 */
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#ifndef LED_GPIO_NUM
#define LED_GPIO_NUM 4
#endif

static const char *AP_NAME = "ESP32-CAM";
static const uint32_t CAPTURE_EVERY_MS = 15000;
static const uint32_t WIFI_LOST_REBOOT_MS = 25000;

// Mặc định — sẽ ghi đè bằng portal / Preferences
// Tunnel public → PC:3090 (cloudflared). Đổi trong portal WiFi nếu URL thay đổi.
String serverBase = "https://api.duynguyen.io.vn";
String lastJson = "{}";
String lastLabel = "—";
float lastScore = 0;
unsigned long lastCapture = 0;
unsigned long wifiLostSince = 0;
bool cameraReady = false;

Preferences prefs;
WebServer server(80);
WiFiManager wm;

bool initCamera() {
  if (cameraReady) {
    esp_camera_deinit();
    delay(50);
    cameraReady = false;
  }

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
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init err 0x%x — retry 1FB\n", (unsigned)err);
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.println("Camera FAIL");
      return false;
    }
  }

  // Lấy ngay 1 frame — xác nhận DMA sống; không warm-up dài (dễ OVF)
  camera_fb_t *probe = esp_camera_fb_get();
  if (probe) esp_camera_fb_return(probe);

  cameraReady = true;
  Serial.println("Camera OK (JPEG QVGA)");
  return true;
}

void stopCamera() {
  if (!cameraReady) return;
  esp_camera_deinit();
  cameraReady = false;
  delay(30);
}

camera_fb_t *grabFreshFrame() {
  // Bật lại camera ngay trước khi chụp — tránh FB-OVF khi chờ WiFi / idle
  if (!cameraReady && !initCamera()) return nullptr;

  camera_fb_t *fb = nullptr;
  for (int attempt = 0; attempt < 3; attempt++) {
    // bỏ 1–2 frame đầu (AE settle)
    for (int i = 0; i < 2; i++) {
      camera_fb_t *skip = esp_camera_fb_get();
      if (skip) esp_camera_fb_return(skip);
      delay(40);
    }
    fb = esp_camera_fb_get();
    if (fb && fb->len > 500) break;
    if (fb) {
      esp_camera_fb_return(fb);
      fb = nullptr;
    }
    Serial.println("reinit camera...");
    stopCamera();
    if (!initCamera()) return nullptr;
  }
  return fb;
}

String extractJsonString(const String &json, const char *key) {
  String pat = String("\"") + key + "\":\"";
  int i = json.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int j = json.indexOf('"', i);
  if (j < 0) return "";
  return json.substring(i, j);
}

float extractJsonFloat(const String &json, const char *key) {
  String pat = String("\"") + key + "\":";
  int i = json.indexOf(pat);
  if (i < 0) return 0;
  i += pat.length();
  return json.substring(i).toFloat();
}

bool postImageToServer(camera_fb_t *fb) {
  if (!fb || WiFi.status() != WL_CONNECTED) return false;

  String url = serverBase;
  if (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/leaf/predict";

  Serial.printf("POST %s (%u bytes)\n", url.c_str(), fb->len);

  HTTPClient http;
  WiFiClientSecure *secure = nullptr;
  WiFiClient *plain = nullptr;

  if (url.startsWith("https://")) {
    secure = new WiFiClientSecure();
    secure->setInsecure();  // tunnel/public cert
    http.begin(*secure, url);
  } else {
    plain = new WiFiClient();
    http.begin(*plain, url);
  }

  http.setTimeout(20000);
  String boundary = "----ESP32LeafBoundary";
  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"image\"; filename=\"leaf.jpg\"\r\n";
  head += "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t total = head.length() + fb->len + tail.length();
  uint8_t *body = (uint8_t *)malloc(total);
  if (!body) {
    http.end();
    delete secure;
    delete plain;
    return false;
  }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), fb->buf, fb->len);
  memcpy(body + head.length() + fb->len, tail.c_str(), tail.length());

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, total);
  free(body);

  bool ok = false;
  if (code > 0) {
    lastJson = http.getString();
    Serial.printf("HTTP %d: %s\n", code, lastJson.c_str());
    if (code == 200) {
      lastLabel = extractJsonString(lastJson, "label_vi");
      if (lastLabel.isEmpty()) lastLabel = extractJsonString(lastJson, "label");
      lastScore = extractJsonFloat(lastJson, "score");
      ok = true;
    }
  } else {
    Serial.printf("HTTP fail: %s\n", http.errorToString(code).c_str());
    lastJson = "{\"ok\":false,\"error\":\"http\"}";
  }

  http.end();
  delete secure;
  delete plain;
  return ok;
}

bool captureAndSend() {
  camera_fb_t *fb = grabFreshFrame();
  if (!fb) {
    Serial.println("capture fail (null fb)");
    stopCamera();
    return false;
  }
  Serial.printf("Captured %u bytes fmt=%u\n", fb->len, (unsigned)fb->format);
  bool ok = postImageToServer(fb);
  esp_camera_fb_return(fb);
  // Tắt camera giữa các lần chụp — hết FB-OVF khi idle
  stopCamera();
  Serial.println(ok ? "Send OK" : "Send FAIL");
  return ok;
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
                  "<meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>ESP32 Leaf Cam</title>"
                  "<style>body{font-family:sans-serif;background:#102418;color:#e8f5e9;padding:16px}"
                  ".card{max-width:480px;margin:auto;background:#1b3324;border-radius:14px;padding:16px}"
                  "button{width:100%;padding:12px;border:0;border-radius:10px;background:#66bb6a;color:#063;font-weight:700;margin-top:8px}"
                  "img{width:100%;border-radius:10px;margin-top:10px}.lab{font-size:1.4rem;color:#a5d6a7;font-weight:700}</style>"
                  "</head><body><div class=card><h1>Camera → Server AI</h1>");
  html += "<div class=lab>" + lastLabel + "</div>";
  html += "<div>Score: " + String(lastScore, 3) + "</div>";
  html += "<div style='opacity:.75;font-size:.85rem;word-break:break-all'>Server: " + serverBase + "</div>";
  html += F("<button onclick=\"fetch('/capture').then(()=>location.reload())\">Chụp & gửi server</button>");
  html += F("<img src='/jpg'>");
  html += F("<p style='opacity:.7;font-size:.85rem'>Mất WiFi / mạng mới → SoftAP portal ESP32-CAM</p>");
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleCapture() {
  bool ok = captureAndSend();
  server.send(200, "application/json", lastJson.length() ? lastJson : String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleJpg() {
  camera_fb_t *fb = grabFreshFrame();
  if (!fb) {
    server.send(500, "text/plain", "fail");
    return;
  }
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  stopCamera();
}

bool startWifiPortal() {
  prefs.begin("leafcam", true);
  serverBase = prefs.getString("server", serverBase);
  prefs.end();
  // Migrate khỏi quick-tunnel tạm → domain cố định
  if (serverBase.indexOf("trycloudflare.com") >= 0) {
    serverBase = "https://api.duynguyen.io.vn";
  }

  WiFiManagerParameter p_server(
      "server", "URL server AI (https://...)", serverBase.c_str(), 120);

  wm.addParameter(&p_server);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(25);
  wm.setTitle("ESP32-CAM Leaf");
  wm.setDarkMode(true);
  wm.setSaveConfigCallback([]() {});

  bool ok = wm.autoConnect(AP_NAME);
  if (!ok) ok = wm.startConfigPortal(AP_NAME);

  // Lưu URL server từ portal
  String s = p_server.getValue();
  if (s.length() > 8) {
    serverBase = s;
    prefs.begin("leafcam", false);
    prefs.putString("server", serverBase);
    prefs.end();
  }
  Serial.println("Server: " + serverBase);
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-CAM Capture → Server AI ===");

  // WiFi TRƯỚC — không để camera chạy trong lúc AutoConnect (gây FB-OVF)
  Serial.println("WiFi portal: SoftAP ESP32-CAM neu chua co / fail mang nha");
  if (!startWifiPortal()) {
    delay(1500);
    ESP.restart();
  }
  WiFi.setSleep(false);
  Serial.print("IP local: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/capture", handleCapture);
  server.on("/jpg", handleJpg);
  server.begin();

  // Chụp ngay (init camera bên trong) — không init/stop thừa
  captureAndSend();
  lastCapture = millis();
}

void loop() {
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostSince = 0;
    if (millis() - lastCapture >= CAPTURE_EVERY_MS) {
      captureAndSend();
      lastCapture = millis();
    }
  } else {
    if (wifiLostSince == 0) wifiLostSince = millis();
    WiFi.reconnect();
    if (millis() - wifiLostSince > WIFI_LOST_REBOOT_MS) {
      Serial.println("Mat WiFi -> restart portal");
      delay(200);
      ESP.restart();
    }
  }
  delay(2);
}
