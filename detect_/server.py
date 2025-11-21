import os
import json
import sqlite3
from datetime import datetime

from flask import Flask, request, jsonify, redirect, Response
from ultralytics import YOLO
import numpy as np
import cv2
import requests  # vẫn giữ nếu sau này cần
import websocket  # pip install websocket-client

# ====== CONFIG ======
DB_PATH = "detect_log.db"
SAVE_DIR = os.path.join("static", "frames")
os.makedirs(SAVE_DIR, exist_ok=True)

PRESENCE_THRESHOLD_SEC = 5.0     # Người đứng liên tục > 5s thì báo động
ALARM_COOLDOWN_SEC = 10.0        # 10s mới cho báo động lại

# (Python KHÔNG còn điều khiển còi trực tiếp nữa)
# ESP32_IP = "192.168.43.50"

# Địa chỉ WebSocket của server.js (wss Node đang nghe ở port 8080)
IOT_WS_URL = "ws://192.168.1.40:8080"   # ĐỔI IP này cho đúng máy chạy server.js

# ====== APP & MODEL ======
app = Flask(__name__)

print("Loading YOLO model...")
model = YOLO("yolov8n.pt")
try:
    model.to("cuda")
    print("Using CUDA (GPU)")
except Exception as e:
    print("CUDA not available, using CPU:", e)

# ====== DB (SQLite) ======
def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("""
      CREATE TABLE IF NOT EXISTS detections (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts TEXT,
        person_count INTEGER,
        image_path TEXT,
        boxes_json TEXT
      )
    """)
    conn.commit()
    conn.close()

def insert_detection(ts, person_count, image_path, boxes):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("""
      INSERT INTO detections (ts, person_count, image_path, boxes_json)
      VALUES (?, ?, ?, ?)
    """, (ts, person_count, image_path, json.dumps(boxes)))
    conn.commit()
    conn.close()

def get_last_detections(limit=50):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("""
      SELECT id, ts, person_count, image_path, boxes_json
      FROM detections
      ORDER BY id DESC
      LIMIT ?
    """, (limit,))
    rows = c.fetchall()
    conn.close()
    return rows

# ====== GỬI ALERT SANG SERVER JS QUA WEBSOCKET ======
def send_intrusion_alert_ws(now, person_count, duration_sec):
    """
    Gửi thông báo xâm nhập sang server.js qua WebSocket.
    server.js (wss) sẽ nhận JSON dạng:
      { "type": "intrusion", "timestamp": "...", "persons": n, "duration_sec": x.x }
    """
    payload = {
        "type": "intrusion",
        "timestamp": now.isoformat(),
        "persons": person_count,
        "duration_sec": duration_sec
    }
    try:
        ws = websocket.create_connection(IOT_WS_URL, timeout=2)
        ws.send(json.dumps(payload))
        ws.close()
        print(">>> Sent intrusion alert via WS:", payload)
    except Exception as e:
        print("Error sending intrusion alert via WS:", e)

# ====== THEO DÕI TRẠNG THÁI NGƯỜI ======
person_active = False
person_start_ts = None
last_alarm_ts = None

def trigger_alarm(now, person_count, duration_sec):
    """
    Khi phát hiện người xâm nhập >= 5s:
    - KHÔNG bật còi ở Python nữa
    - Chỉ đẩy thông báo qua WebSocket cho server JS
    """
    global last_alarm_ts
    print("=== ALARM (INTRUSION) ===")
    print(f"Time: {now.isoformat()} | persons: {person_count} | duration: {duration_sec:.1f}s")
    print("=========================")

    # Đẩy alert sang server JS (Node)
    send_intrusion_alert_ws(now, person_count, duration_sec)

    last_alarm_ts = now

# ====== STREAM FRAME MỚI NHẤT ======
latest_frame = None  # lưu frame BGR mới nhất (đã vẽ bbox)

def generate_stream():
    """Generator trả về luồng MJPEG từ latest_frame."""
    global latest_frame
    while True:
        if latest_frame is not None:
            # Encode BGR -> JPEG
            ret, jpeg = cv2.imencode(".jpg", latest_frame)
            if not ret:
                continue
            frame = jpeg.tobytes()
            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
            )
        else:
            # Nếu chưa có frame nào thì nghỉ nhẹ 1 chút
            import time
            time.sleep(0.1)

# ====== ROUTES ======

@app.route("/")
def index():
    return redirect("/history")

@app.route("/upload", methods=["POST"])
def upload():
    global person_active, person_start_ts, last_alarm_ts, latest_frame

    img_bytes = request.data
    if not img_bytes:
        return jsonify({"error": "no data"}), 400

    nparr = np.frombuffer(img_bytes, np.uint8)
    img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    if img is None:
        return jsonify({"error": "decode fail"}), 400

    now = datetime.utcnow()

    # YOLO detect person
    results = model(img, classes=[0])
    person_count = 0
    boxes_info = []

    for r in results:
        for box in r.boxes:
            cls_id = int(box.cls[0])
            conf = float(box.conf[0])
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            person_count += 1
            boxes_info.append({
                "class_id": cls_id,
                "conf": conf,
                "bbox": [x1, y1, x2, y2]
            })

            # Vẽ bounding box
            cv2.rectangle(
                img,
                (int(x1), int(y1)),
                (int(x2), int(y2)),
                (0, 255, 0),
                2
            )
            label = f"person {conf:.2f}"
            cv2.putText(
                img,
                label,
                (int(x1), int(y1) - 5),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (0, 255, 0),
                1
            )

    alarm_triggered = False
    presence_duration = 0.0

    # ====== LOGIC THEO DÕI LIÊN TỤC ======
    if person_count > 0:
        if not person_active:
            person_active = True
            person_start_ts = now
        else:
            presence_duration = (now - person_start_ts).total_seconds()
            if presence_duration >= PRESENCE_THRESHOLD_SEC:
                if (last_alarm_ts is None or
                    (now - last_alarm_ts).total_seconds() >= ALARM_COOLDOWN_SEC):
                    trigger_alarm(now, person_count, presence_duration)
                    alarm_triggered = True
    else:
        # Không còn người
        person_active = False
        person_start_ts = None
        presence_duration = 0.0

        # KHÔNG tắt còi ở đây nữa, JS tự quản lý

    # ====== CẬP NHẬT FRAME CHO STREAM ======
    latest_frame = img.copy()

    # ====== LƯU ẢNH & LOG CHỈ KHI CÓ NGƯỜI ======
    timestamp_str = now.strftime("%Y%m%d_%H%M%S_%f")
    image_url = None

    if person_count > 0:
        filename = f"{timestamp_str}_p{person_count}.jpg"
        save_path = os.path.join(SAVE_DIR, filename)
        cv2.imwrite(save_path, img)
        image_url = f"/static/frames/{filename}"

        insert_detection(timestamp_str, person_count, image_url, boxes_info)

    return jsonify({
        "timestamp": timestamp_str,
        "persons": person_count,
        "image_url": image_url,
        "boxes": boxes_info,
        "presence_active": person_active,
        "presence_duration_sec": presence_duration,
        "alarm_triggered": alarm_triggered
    })

@app.route("/history", methods=["GET"])
def history():
    rows = get_last_detections(limit=50)
    html = """
    <html>
    <head>
      <meta charset="utf-8" />
      <title>Lich su detect</title>
      <style>
        body { font-family: Arial, sans-serif; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ccc; padding: 6px; text-align: left; }
        img { max-width: 320px; height: auto; }
        a.btn {
          display: inline-block;
          margin-bottom: 10px;
          padding: 6px 10px;
          background: #007bff;
          color: #fff;
          text-decoration: none;
          border-radius: 4px;
        }
      </style>
    </head>
    <body>
      <h2>Lich su detect (moi nhat)</h2>
      <a class="btn" href="/stream" target="_blank">Xem live stream (/stream)</a>
      <table>
        <tr>
          <th>ID</th>
          <th>Time (UTC)</th>
          <th>Persons</th>
          <th>Image</th>
        </tr>
    """

    for row in rows:
        id_, ts, person_count, image_path, boxes_json = row
        html += f"""
        <tr>
          <td>{id_}</td>
          <td>{ts}</td>
          <td>{person_count}</td>
          <td>
            <a href="{image_path}" target="_blank">
              <img src="{image_path}" />
            </a>
          </td>
        </tr>
        """

    html += """
      </table>
    </body>
    </html>
    """
    return html

@app.route("/stream", methods=["GET"])
def stream():
    """Route xem live stream từ frame mới nhất."""
    return Response(
        generate_stream(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )

# ====== MAIN ======
if __name__ == "__main__":
    init_db()
    app.run(host="0.0.0.0", port=5000, debug=False)
