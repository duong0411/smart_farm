# ESP32-CAM — Continuous MQTT JPEG stream

Camera **luôn bật**, nén JPEG liên tục → AES → MQTT binary. Mobile decode từng packet và hiển thị (gapless).

## SoftAP / Broker

- SoftAP: `ESP32-CAM-MQTT`
- Broker: `wss://mqtt.duynguyen.io.vn:443/mqtt`
- Chip mặc định: `123` (trùng app)

## Topics

| Topic | Payload |
|-------|---------|
| `tele/{chip}_cam/status` | `online` / `offline` |
| `tele/{chip}_cam/frame` | **binary** `SFC1` + w/h + iv + AES ciphertext |
| `cmnd/{chip}_cam/STREAM` | `ON` / `OFF` |
| `cmnd/{chip}_cam/SNAP` | chụp ngay 1 frame |

## Packet `SFC1`

```
magic[4] = 'S','F','C','1'
width    u16 LE
height   u16 LE
iv[16]
ciphertext  AES-128-CBC PKCS7(JPEG)
```

## Nén / tốc độ

- QQVGA 160×120, `jpeg_quality = 28` (~1–3 KB JPEG)
- Interval mặc định **250 ms** (~4 fps), portal chỉnh được (≥100 ms)
- 1 MQTT message / frame (không chia chunk)

## Flash

Board **AI Thinker ESP32-CAM** · WiFiManager, WebSockets, MQTTPubSubClient
