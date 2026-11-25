"""
Smart Farm — Leaf Disease AI Server
ESP32-CAM POST ảnh → predict + lưu detection + cảnh báo theo ngưỡng.
"""
from __future__ import annotations

import asyncio
import io
import json
import logging
import os
import time
import uuid
from collections import deque
from datetime import datetime
from logging.handlers import RotatingFileHandler
from pathlib import Path
from threading import Lock

import numpy as np
from fastapi import FastAPI, File, Request, UploadFile, HTTPException, Body
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, HTMLResponse, StreamingResponse, FileResponse
from PIL import Image, ImageDraw, ImageFont
import tensorflow as tf

ROOT = Path(__file__).resolve().parents[1]
MODEL_DIR = ROOT / "pipeline" / "models"
TFLITE_SERVER = MODEL_DIR / "leaf_server.tflite"
TFLITE_TINY = MODEL_DIR / "leaf_int8.tflite"
KERAS_PATH = MODEL_DIR / "leaf_float.keras"
META_PATH = MODEL_DIR / "meta.json"
LOG_DIR = Path(__file__).resolve().parent / "logs"
DATA_DIR = Path(__file__).resolve().parent / "data"
DET_DIR = DATA_DIR / "detections"
SETTINGS_PATH = DATA_DIR / "settings.json"
INDEX_PATH = DATA_DIR / "index.json"

for d in (LOG_DIR, DATA_DIR, DET_DIR):
    d.mkdir(parents=True, exist_ok=True)

LOG_FILE = LOG_DIR / "leaf_ai.log"
IMG = 224
PORT = int(os.environ.get("LEAF_PORT", "3090"))
PREFER_TFLITE = os.environ.get("LEAF_PREFER_TFLITE", "1") == "1"
MAX_DETECTIONS = 60
HEALTHY_LABEL = "khoe_manh"

RECENT: deque[str] = deque(maxlen=300)
_lock = Lock()

DEFAULT_SETTINGS = {
    "alert_threshold": 0.55,
    "alert_enabled": True,
    "healthy_label": HEALTHY_LABEL,
}


class RingHandler(logging.Handler):
    def emit(self, record):
        try:
            RECENT.append(self.format(record))
        except Exception:
            pass


def setup_logger():
    log = logging.getLogger("leaf")
    log.setLevel(logging.INFO)
    log.handlers.clear()
    fmt = logging.Formatter("%(asctime)s | %(levelname)s | %(message)s", "%Y-%m-%d %H:%M:%S")
    fh = RotatingFileHandler(LOG_FILE, maxBytes=2_000_000, backupCount=3, encoding="utf-8")
    fh.setFormatter(fmt)
    sh = logging.StreamHandler()
    sh.setFormatter(fmt)
    rh = RingHandler()
    rh.setFormatter(fmt)
    log.addHandler(fh)
    log.addHandler(sh)
    log.addHandler(rh)
    return log


log = setup_logger()

app = FastAPI(title="Smart Farm Leaf AI", version="2.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

LABELS = ["dom_la_goc", "gi_sat_la", "khoe_manh"]
LABELS_VI = {
    "dom_la_goc": "Đốm lá góc (bệnh)",
    "gi_sat_la": "Gỉ sắt lá (bệnh/nấm)",
    "khoe_manh": "Lá khỏe mạnh",
}
model = None
interpreter = None
model_name = "—"
_input_detail = None
_output_detail = None


def load_settings() -> dict:
    if SETTINGS_PATH.exists():
        try:
            data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            return {**DEFAULT_SETTINGS, **data}
        except Exception:
            pass
    return dict(DEFAULT_SETTINGS)


def save_settings(data: dict) -> dict:
    merged = {**DEFAULT_SETTINGS, **data}
    thr = float(merged.get("alert_threshold", 0.55))
    merged["alert_threshold"] = max(0.2, min(0.99, thr))
    merged["alert_enabled"] = bool(merged.get("alert_enabled", True))
    SETTINGS_PATH.write_text(json.dumps(merged, indent=2), encoding="utf-8")
    return merged


def load_index() -> list[dict]:
    if not INDEX_PATH.exists():
        return []
    try:
        return json.loads(INDEX_PATH.read_text(encoding="utf-8"))
    except Exception:
        return []


def save_index(items: list[dict]):
    INDEX_PATH.write_text(json.dumps(items[:MAX_DETECTIONS], indent=2), encoding="utf-8")


def annotate_image(raw: bytes, label: str, label_vi: str, score: float, alert: bool) -> bytes:
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    draw = ImageDraw.Draw(img)
    w, h = img.size
    bar_h = max(36, h // 10)
    color = (196, 69, 54) if alert else (45, 106, 79)
    draw.rectangle([0, 0, w, bar_h], fill=color)
    text = f"{'⚠ CẢNH BÁO' if alert else 'OK'} · {label_vi} · {score * 100:.0f}%"
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", max(14, bar_h // 2))
    except Exception:
        font = ImageFont.load_default()
    draw.text((10, bar_h // 4), text, fill=(255, 255, 255), font=font)
    if alert:
        # khung đỏ quanh ảnh
        draw.rectangle([2, 2, w - 3, h - 3], outline=(196, 69, 54), width=4)
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=85)
    return out.getvalue()


def persist_detection(raw: bytes, result: dict) -> dict:
    settings = load_settings()
    det_id = datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:6]
    alert = bool(result.get("alert"))
    viz = annotate_image(
        raw,
        result["label"],
        result["label_vi"],
        result["score"],
        alert,
    )
    jpg_path = DET_DIR / f"{det_id}.jpg"
    viz_path = DET_DIR / f"{det_id}_viz.jpg"
    jpg_path.write_bytes(raw)
    viz_path.write_bytes(viz)

    item = {
        "id": det_id,
        "ts": datetime.now().isoformat(timespec="seconds"),
        "label": result["label"],
        "label_vi": result["label_vi"],
        "score": result["score"],
        "probs": result["probs"],
        "alert": alert,
        "threshold": settings["alert_threshold"],
        "image_url": f"/api/leaf/detections/{det_id}/image",
        "viz_url": f"/api/leaf/detections/{det_id}/viz",
        "bytes": len(raw),
        "infer_ms": result.get("infer_ms"),
    }
    with _lock:
        items = load_index()
        items.insert(0, item)
        # dọn file cũ
        for old in items[MAX_DETECTIONS:]:
            for suffix in (".jpg", "_viz.jpg"):
                p = DET_DIR / f"{old['id']}{suffix}"
                if p.exists():
                    try:
                        p.unlink()
                    except OSError:
                        pass
        save_index(items[:MAX_DETECTIONS])
    return item


def load_model():
    global model, interpreter, model_name, LABELS, _input_detail, _output_detail, IMG
    if META_PATH.exists():
        meta = json.loads(META_PATH.read_text(encoding="utf-8"))
        LABELS = meta.get("classes", LABELS)
        IMG = int(meta.get("img", IMG))

    tflite_path = TFLITE_SERVER if TFLITE_SERVER.exists() else TFLITE_TINY
    if PREFER_TFLITE and tflite_path.exists():
        interpreter = tf.lite.Interpreter(model_path=str(tflite_path))
        interpreter.allocate_tensors()
        _input_detail = interpreter.get_input_details()[0]
        _output_detail = interpreter.get_output_details()[0]
        shape = _input_detail.get("shape")
        if shape is not None and len(shape) == 4:
            IMG = int(shape[1])
        model_name = tflite_path.name
        log.info("Model loaded: %s img=%d labels=%s", model_name, IMG, LABELS)
        return

    if not KERAS_PATH.exists():
        raise FileNotFoundError(f"Missing model: {tflite_path} or {KERAS_PATH}")
    model = tf.keras.models.load_model(KERAS_PATH)
    model_name = KERAS_PATH.name
    log.info("Model loaded: %s img=%d labels=%s", model_name, IMG, LABELS)


def infer(x: np.ndarray) -> np.ndarray:
    if interpreter is not None:
        inp = _input_detail
        out = _output_detail
        if inp["dtype"] == np.int8:
            scale, zp = inp["quantization"]
            xq = np.round(x / scale + zp).astype(np.int8)
            interpreter.set_tensor(inp["index"], xq)
        elif inp["dtype"] == np.uint8:
            scale, zp = inp["quantization"]
            xq = np.round(x / scale + zp).clip(0, 255).astype(np.uint8)
            interpreter.set_tensor(inp["index"], xq)
        else:
            interpreter.set_tensor(inp["index"], x.astype(inp["dtype"]))
        interpreter.invoke()
        y = interpreter.get_tensor(out["index"])[0]
        if out["dtype"] in (np.int8, np.uint8):
            scale, zp = out["quantization"]
            y = (y.astype(np.float32) - zp) * scale
        return y.astype(np.float32)
    return model.predict(x, verbose=0)[0].astype(np.float32)


def preprocess(raw: bytes) -> np.ndarray:
    img = Image.open(io.BytesIO(raw)).convert("RGB").resize((IMG, IMG))
    arr = np.asarray(img, dtype=np.float32) / 255.0
    return np.expand_dims(arr, 0)


@app.on_event("startup")
def _startup():
    os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
    save_settings(load_settings())
    load_model()
    log.info("Smart Farm Leaf AI ready :%s", PORT)


@app.middleware("http")
async def access_log(request: Request, call_next):
    t0 = time.perf_counter()
    response = await call_next(request)
    ms = (time.perf_counter() - t0) * 1000
    path = request.url.path
    if path not in ("/logs/stream", "/favicon.ico") and not path.endswith("/image") and not path.endswith("/viz"):
        log.info("HTTP %s %s %s %.0fms", request.method, path, response.status_code, ms)
    return response


@app.get("/", response_class=HTMLResponse)
def home():
    return """
    <html><head><meta charset=utf-8><title>Smart Farm Leaf AI</title></head>
    <body style="font-family:sans-serif;max-width:720px;margin:24px auto">
      <h1>Smart Farm — Phát hiện sâu bệnh</h1>
      <p>POST /api/leaf/predict · GET /api/leaf/detections · GET/PUT /api/leaf/settings</p>
      <p><a href="/health">/health</a> · <a href="/logs">/logs</a></p>
    </body></html>
    """


@app.get("/logs", response_class=HTMLResponse)
def logs_page():
    return home()


@app.get("/logs/stream")
async def logs_stream():
    async def gen():
        for line in list(RECENT)[-80:]:
            yield f"data: {line}\n\n"
        idx = len(RECENT)
        while True:
            await asyncio.sleep(0.4)
            snap = list(RECENT)
            while idx < len(snap):
                yield f"data: {snap[idx]}\n\n"
                idx += 1
            if len(RECENT) == RECENT.maxlen and idx > len(RECENT):
                idx = max(0, len(RECENT) - 20)

    return StreamingResponse(gen(), media_type="text/event-stream")


@app.get("/health")
def health():
    settings = load_settings()
    items = load_index()
    alerts = [x for x in items if x.get("alert")]
    return {
        "ok": True,
        "project": "smart_farm",
        "model": model_name,
        "backend": "tflite" if interpreter is not None else "keras",
        "labels": LABELS,
        "port": PORT,
        "alert_threshold": settings["alert_threshold"],
        "detections": len(items),
        "open_alerts": len(alerts),
        "time": datetime.now().isoformat(timespec="seconds"),
    }


@app.get("/api/leaf/settings")
def get_settings():
    return {"ok": True, **load_settings()}


@app.put("/api/leaf/settings")
@app.post("/api/leaf/settings")
async def update_settings(payload: dict = Body(...)):
    cur = load_settings()
    if "alert_threshold" in payload:
        cur["alert_threshold"] = float(payload["alert_threshold"])
    if "alert_enabled" in payload:
        cur["alert_enabled"] = bool(payload["alert_enabled"])
    saved = save_settings(cur)
    log.info("SETTINGS threshold=%.2f enabled=%s", saved["alert_threshold"], saved["alert_enabled"])
    return {"ok": True, **saved}


@app.get("/api/leaf/detections")
def list_detections(limit: int = 30, alerts_only: bool = False):
    items = load_index()
    if alerts_only:
        items = [x for x in items if x.get("alert")]
    return {"ok": True, "count": len(items), "items": items[: max(1, min(limit, 60))]}


@app.get("/api/leaf/alerts")
def list_alerts(limit: int = 20):
    settings = load_settings()
    items = [x for x in load_index() if x.get("alert")]
    return {
        "ok": True,
        "threshold": settings["alert_threshold"],
        "enabled": settings["alert_enabled"],
        "count": len(items),
        "items": items[: max(1, min(limit, 60))],
        "latest": items[0] if items else None,
    }


@app.get("/api/leaf/detections/{det_id}/image")
def detection_image(det_id: str):
    path = DET_DIR / f"{det_id}.jpg"
    if not path.exists():
        raise HTTPException(404, "Not found")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/leaf/detections/{det_id}/viz")
def detection_viz(det_id: str):
    path = DET_DIR / f"{det_id}_viz.jpg"
    if not path.exists():
        raise HTTPException(404, "Not found")
    return FileResponse(path, media_type="image/jpeg")


@app.post("/api/leaf/predict")
async def predict(request: Request, image: UploadFile = File(...)):
    if model is None and interpreter is None:
        raise HTTPException(503, "Model not loaded")
    raw = await image.read()
    client = request.client.host if request.client else "?"
    if not raw:
        raise HTTPException(400, "Empty image")
    try:
        settings = load_settings()
        t0 = time.perf_counter()
        probs = infer(preprocess(raw))
        idx = int(np.argmax(probs))
        label = LABELS[idx]
        score = float(probs[idx])
        ms = (time.perf_counter() - t0) * 1000
        healthy = settings.get("healthy_label", HEALTHY_LABEL)
        is_disease = label != healthy
        alert = bool(
            settings.get("alert_enabled", True)
            and is_disease
            and score >= float(settings.get("alert_threshold", 0.55))
        )
        result = {
            "ok": True,
            "label": label,
            "label_vi": LABELS_VI.get(label, label),
            "score": round(score, 4),
            "probs": {LABELS[i]: round(float(probs[i]), 4) for i in range(len(LABELS))},
            "bytes": len(raw),
            "infer_ms": round(ms, 1),
            "alert": alert,
            "threshold": settings["alert_threshold"],
        }
        item = persist_detection(raw, result)
        result["id"] = item["id"]
        result["image_url"] = item["image_url"]
        result["viz_url"] = item["viz_url"]
        log.info(
            "PREDICT ok client=%s bytes=%d label=%s score=%.4f alert=%s",
            client,
            len(raw),
            label,
            score,
            alert,
        )
        return JSONResponse(result)
    except Exception as e:
        log.exception("PREDICT fail client=%s err=%s", client, e)
        raise HTTPException(500, f"Infer error: {e}") from e


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("app:app", host="0.0.0.0", port=PORT, reload=False)
