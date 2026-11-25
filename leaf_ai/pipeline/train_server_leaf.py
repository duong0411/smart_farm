"""
Train server-side leaf classifier (4GB VRAM OK).
EfficientNetB0 transfer learning @ 224 — độ chính xác cao hơn tiny CNN.
Xuất: models/leaf_float.keras + leaf_server.tflite + meta.json
"""
from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import tensorflow as tf

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "pipeline" / "models"
OUT.mkdir(parents=True, exist_ok=True)

IMG = 224
BATCH = 24  # CPU / 4GB VRAM đều ổn
EPOCHS_HEAD = 10
EPOCHS_FT = 15
SEED = 42
LR_HEAD = 1e-3
LR_FT = 1e-5


def setup_gpu():
    gpus = tf.config.list_physical_devices("GPU")
    for g in gpus:
        try:
            tf.config.experimental.set_memory_growth(g, True)
        except Exception:
            pass
    print("GPUs:", gpus or "CPU only")


def load_beans(img_size: int):
    import io
    import urllib.request
    from PIL import Image
    import pyarrow.parquet as pq

    data_dir = OUT / "ibeans_pq"
    data_dir.mkdir(parents=True, exist_ok=True)
    urls = {
        "train": "https://huggingface.co/datasets/beans/resolve/main/data/train-00000-of-00001.parquet",
        "validation": "https://huggingface.co/datasets/beans/resolve/main/data/validation-00000-of-00001.parquet",
        "test": "https://huggingface.co/datasets/beans/resolve/main/data/test-00000-of-00001.parquet",
    }
    class_names = ["dom_la_goc", "gi_sat_la", "khoe_manh"]

    def ensure(name: str) -> Path:
        path = data_dir / f"{name}.parquet"
        if path.exists() and path.stat().st_size > 1000:
            return path
        print("Downloading", name)
        req = urllib.request.Request(urls[name], headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=180) as r, open(path, "wb") as f:
            f.write(r.read())
        return path

    def load_split(name: str):
        table = pq.read_table(ensure(name))
        images = table.column("image").to_pylist()
        labels = table.column("labels").to_pylist()
        xs, ys = [], []
        for img_obj, lab in zip(images, labels):
            raw = img_obj["bytes"] if isinstance(img_obj, dict) else img_obj
            img = Image.open(io.BytesIO(raw)).convert("RGB").resize((img_size, img_size))
            xs.append(np.asarray(img, dtype=np.float32) / 255.0)
            ys.append(int(lab))
        return np.stack(xs), np.array(ys, dtype=np.int32)

    print("Classes:", class_names)
    return (
        load_split("train"),
        load_split("validation"),
        load_split("test"),
        class_names,
    )


def make_augmenter():
    return tf.keras.Sequential(
        [
            tf.keras.layers.RandomFlip("horizontal"),
            tf.keras.layers.RandomRotation(0.15),
            tf.keras.layers.RandomZoom(0.15),
            tf.keras.layers.RandomContrast(0.15),
            tf.keras.layers.RandomTranslation(0.08, 0.08),
        ],
        name="aug",
    )


def build_model(num_classes: int) -> tuple[tf.keras.Model, tf.keras.Model]:
    """MobileNetV2 ImageNet — nhẹ 4GB, accuracy cao trên beans."""
    base = tf.keras.applications.MobileNetV2(
        include_top=False,
        weights="imagenet",
        input_shape=(IMG, IMG, 3),
        pooling="avg",
    )
    base.trainable = False

    inputs = tf.keras.Input(shape=(IMG, IMG, 3))
    # input [0,1] → preprocess MobileNet ([-1,1])
    x = tf.keras.layers.Rescaling(255.0)(inputs)
    x = tf.keras.applications.mobilenet_v2.preprocess_input(x)
    x = base(x, training=False)
    x = tf.keras.layers.Dropout(0.35)(x)
    x = tf.keras.layers.Dense(128, activation="relu")(x)
    x = tf.keras.layers.Dropout(0.25)(x)
    outputs = tf.keras.layers.Dense(num_classes, activation="softmax")(x)
    model = tf.keras.Model(inputs, outputs, name="leaf_mobilenetv2")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(LR_HEAD),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model, base


def export_tflite(model, path: Path):
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]  # dynamic range — nhẹ, đủ chính xác
    data = converter.convert()
    path.write_bytes(data)
    print("TFLite KB:", round(len(data) / 1024, 1))
    return data


def main():
    os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
    setup_gpu()
    tf.keras.utils.set_random_seed(SEED)

    (x_train, y_train), (x_val, y_val), (x_test, y_test), class_names = load_beans(IMG)
    print("train", x_train.shape, "val", x_val.shape, "test", x_test.shape)

    aug = make_augmenter()

    def gen(xs, ys, training: bool):
        ds = tf.data.Dataset.from_tensor_slices((xs, ys))
        if training:
            ds = ds.shuffle(len(xs), seed=SEED, reshuffle_each_iteration=True)

            def _aug(img, lab):
                return aug(img, training=True), lab

            ds = ds.map(_aug, num_parallel_calls=tf.data.AUTOTUNE)
        return ds.batch(BATCH).prefetch(tf.data.AUTOTUNE)

    train_ds = gen(x_train, y_train, True)
    val_ds = gen(x_val, y_val, False)
    test_ds = gen(x_test, y_test, False)

    model, base = build_model(len(class_names))
    model.summary()

    cb_head = [
        tf.keras.callbacks.EarlyStopping(
            patience=3, restore_best_weights=True, monitor="val_accuracy", mode="max"
        ),
        tf.keras.callbacks.ReduceLROnPlateau(patience=2, factor=0.5, monitor="val_loss"),
    ]
    print("=== Phase 1: train head ===")
    model.fit(train_ds, validation_data=val_ds, epochs=EPOCHS_HEAD, callbacks=cb_head, verbose=1)

    print("=== Phase 2: fine-tune top layers ===")
    # Unfreeze top ~30% blocks
    base.trainable = True
    freeze_until = int(len(base.layers) * 0.7)
    for i, layer in enumerate(base.layers):
        layer.trainable = i >= freeze_until
    model.compile(
        optimizer=tf.keras.optimizers.Adam(LR_FT),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    cb_ft = [
        tf.keras.callbacks.EarlyStopping(
            patience=4, restore_best_weights=True, monitor="val_accuracy", mode="max"
        ),
        tf.keras.callbacks.ReduceLROnPlateau(patience=2, factor=0.5, monitor="val_loss"),
    ]
    model.fit(train_ds, validation_data=val_ds, epochs=EPOCHS_FT, callbacks=cb_ft, verbose=1)

    loss, acc = model.evaluate(test_ds, verbose=0)
    print(f"Test accuracy: {acc:.4f}")

    float_path = OUT / "leaf_float.keras"
    model.save(float_path)
    print("Saved", float_path)

    tflite_path = OUT / "leaf_server.tflite"
    export_tflite(model, tflite_path)

    # giữ leaf_int8.tflite cũ cho firmware on-device nếu cần; server dùng leaf_server.tflite
    meta = {
        "classes": class_names,
        "img": IMG,
        "test_accuracy": float(acc),
        "backbone": "MobileNetV2",
        "model_keras": float_path.name,
        "model_tflite": tflite_path.name,
        "tflite_kb": round(tflite_path.stat().st_size / 1024, 1),
        "dataset": "huggingface:beans",
        "vram_note": "MobileNetV2 @224 batch=24 — OK CPU/4GB VRAM",
    }
    (OUT / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print("DONE meta:", json.dumps(meta, indent=2))


if __name__ == "__main__":
    main()
