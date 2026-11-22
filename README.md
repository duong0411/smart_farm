# Smart Farm

Firmware ESP32 + app Flutter cho nhà kính thông minh: cảm biến, bơm / đèn / quạt qua MQTT.

## Cấu trúc

| Thư mục | Mô tả |
|---------|--------|
| `SmartFarm_ESP32.ino` | Firmware ESP32 (AUTO/MANUAL, MQTT WSS) |
| `mobile/` | App Flutter (Android + iOS) |
| `.github/workflows/release.yml` | Build APK + IPA và tạo GitHub Release |

## MQTT (khớp firmware)

- Broker: `wss://mqtt.duynguyen.io.vn:443/mqtt`
- Chip ID mặc định firmware: `123`
- Telemetry: `tele/{chipId}_vitals/status`, `tele/{chipId}_pump/status`, …
- Điều khiển: `cmnd/{chipId}_pump/POWER` → `ON` \| `OFF` (chỉ khi **MANUAL**)
- Mode: `cmnd/{chipId}_mode/POWER` → `AUTO` \| `MANUAL`

## App Flutter

### Tính năng

- Đăng ký / đăng nhập / quên mật khẩu (lưu local trên thiết bị)
- Nhập **Chip ID** → kết nối MQTT
- Hiển thị: nhiệt độ, độ ẩm, đất, mực nước, CO₂, ánh sáng
- Điều khiển: bơm, đèn, quạt, chuyển AUTO/MANUAL, calibrate CO₂

### Chạy local

```bash
cd mobile
flutter pub get
flutter run
```

Đăng ký tài khoản mới → nhập Chip ID (ví dụ `123`) → xem dashboard.

## GitHub Actions — APK & IPA Release

Workflow: **Build & Release**

**Cách 1 — tag version**

```bash
git tag v1.0.0
git push origin v1.0.0
```

**Cách 2 — chạy tay**

GitHub → Actions → *Build & Release* → *Run workflow*

Artifacts trên Release:

- `SmartFarm-arm64-v8a.apk` / `armeabi-v7a` / `x86_64`
- `SmartFarm.aab`
- `SmartFarm-unsigned.ipa` (chưa codesign — sideload / cần thêm cert Apple để ký App Store)

### Ký IPA (tùy chọn)

Thêm secrets repo (`BUILD_CERTIFICATE_BASE64`, `P12_PASSWORD`, `BUILD_PROVISION_PROFILE_BASE64`, …) rồi sửa job `build-ios` bỏ `--no-codesign` và import cert trước khi `flutter build ipa`.

## Firmware nhanh

1. Sửa `CHIP_ID` trong `SmartFarm_ESP32.ino` nếu cần
2. Nạp ESP32, cấu hình WiFi (giữ BOOT 5s → portal `SmartFarm`)
3. App nhập đúng Chip ID → nhận `tele/...` và gửi `cmnd/...`
