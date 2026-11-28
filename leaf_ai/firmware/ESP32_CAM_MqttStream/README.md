# ESP32-CAM — MQTT JPEG stream (AES)

SoftAP **ESP32-CAM-MQTT** → WiFi nhà → `wss://mqtt.duynguyen.io.vn/mqtt` → frame JPEG mã hóa AES-128-CBC.

## Cấu hình portal

| Field | Mặc định | Ghi chú |
|-------|----------|---------|
| Chip ID | `123` | **Trùng Chip ID trên app** |
| AES hex | `536d6172744661726d43616d4b657921` | = `SmartFarmCamKey!` |
| Interval | `1500` ms | chu kỳ stream |

## Topics

```
tele/{chip}_cam/status   online|offline
tele/{chip}_cam/meta     id, iv, chunks, w, h…
tele/{chip}_cam/part     id, i, n, d (base64)
cmnd/{chip}_cam/STREAM   ON|OFF
cmnd/{chip}_cam/SNAP     chụp 1 frame
```

## Nén ảnh

- `FRAMESIZE_QQVGA` (160×120) + `jpeg_quality = 22`
- AES + Base64 → chia chunk ~2.8 KB / MQTT message

## Flash

Board **AI Thinker ESP32-CAM** · libs: WiFiManager, WebSockets, MQTTPubSubClient
