# Smart Farm — Leaf AI

Server AI + firmware ESP32-CAM cho phát hiện sâu bệnh lá.

- Model: MobileNetV2 @224 (~91% test)
- Public: `https://api.duynguyen.io.vn` → Docker `:3090`
- App mobile tab **Sâu bệnh** đọc `/api/leaf/detections` + `/api/leaf/settings`

```bash
cd D:\duongpv\smart_farm\leaf_ai
docker compose up -d --build
```
