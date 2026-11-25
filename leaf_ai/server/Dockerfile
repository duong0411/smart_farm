FROM python:3.12-slim-bookworm

WORKDIR /app

RUN pip install --no-cache-dir \
    "fastapi>=0.110" \
    "uvicorn[standard]>=0.27" \
    "python-multipart>=0.0.9" \
    "pillow>=10.0" \
    "numpy>=1.26" \
    "tensorflow-cpu==2.18.0"

COPY server/ /app/server/
COPY pipeline/models/leaf_server.tflite /app/pipeline/models/leaf_server.tflite
COPY pipeline/models/meta.json /app/pipeline/models/meta.json

RUN mkdir -p /app/server/logs /app/server/data/detections

WORKDIR /app/server
ENV LEAF_PORT=3090
ENV LEAF_PREFER_TFLITE=1
ENV TF_CPP_MIN_LOG_LEVEL=2
ENV TF_ENABLE_ONEDNN_OPTS=0
EXPOSE 3090

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "3090"]
