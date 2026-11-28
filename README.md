# Smart Farm

Firmware ESP32 + app Flutter cho nhà kính thông minh: cảm biến, bơm / đèn / quạt qua MQTT, **phát hiện sâu bệnh lá** (ESP32-CAM → AI server).

## Cấu trúc

| Thư mục | Mô tả |
|---------|--------|
| `SmartFarm_ESP32.ino` | Firmware ESP32 nhà kính (AUTO/MANUAL, MQTT WSS) |
| `mobile/` | App Flutter (Android + iOS) |
| `leaf_ai/` | Server AI sâu bệnh + firmware ESP32-CAM Capture |
| `.github/workflows/release.yml` | Build APK + IPA và tạo GitHub Release |

## MQTT (khớp firmware)

- Broker: `wss://mqtt.duynguyen.io.vn:443/mqtt`
- Chip ID mặc định firmware: `123`
- Telemetry: `tele/{chipId}_vitals/status`, `tele/{chipId}_pump/status`, …
- Điều khiển: `cmnd/{chipId}_pump/POWER` → `ON` \| `OFF` (chỉ khi **MANUAL**)
- Mode: `cmnd/{chipId}_mode/POWER` → `AUTO` \| `MANUAL`

## Phát hiện sâu bệnh (Leaf AI)

```
ESP32-CAM  →  POST /api/leaf/predict  →  https://api.duynguyen.io.vn
                                              ↓
                                    Docker leaf-ai :3090
                                    (MobileNetV2 ~91% acc)
```

### Chạy server AI (Docker)

```bash
cd leaf_ai
docker compose up -d --build
```

- Local: `http://127.0.0.1:3090/health`
- Public: `https://api.duynguyen.io.vn` (Cloudflare Tunnel → `:3090`)
- Ngưỡng cảnh báo: `GET/POST /api/leaf/settings` (`alert_threshold`)
- Lịch sử + ảnh viz: `GET /api/leaf/detections`, `/api/leaf/alerts`

### App mobile

Tab **Camera**: stream JPEG từ ESP32-CAM qua MQTT (AES decrypt → hiển thị).

Tab **Sâu bệnh**:

- Cảnh báo khi phát hiện bệnh ≥ ngưỡng
- Ảnh visualize (banner + khung đỏ nếu alert)
- Lịch sử detection
- Set ngưỡng cảnh báo (icon tune)

### Firmware camera

| Sketch | Mô tả |
|--------|--------|
| `leaf_ai/firmware/ESP32_CAM_Capture` | SoftAP `ESP32-CAM`, POST ảnh → `https://api.duynguyen.io.vn` (Leaf AI) |
| `leaf_ai/firmware/ESP32_CAM_MqttStream` | SoftAP `ESP32-CAM-MQTT`, AES-128 + MQTT WSS → mobile (broker Duy Nguyen) |

## App Flutter

### Tính năng

- Đăng ký / đăng nhập / quên mật khẩu (lưu local)
- Nhập **Chip ID** → MQTT
- Cảm biến + điều khiển bơm / đèn / quạt / AUTO·MANUAL
- **Sâu bệnh**: cảnh báo, ảnh, ngưỡng

### Chạy local

```bash
cd mobile
flutter pub get
flutter run
```

## GitHub Actions — APK & IPA Release

Tag `v1.0.0` hoặc Actions → *Build & Release* → *Run workflow*.
