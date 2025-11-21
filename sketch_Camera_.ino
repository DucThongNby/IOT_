#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// ====== CẤU HÌNH WIFI ======
const char* WIFI_SSID = "Xuanuio.";
const char* WIFI_PASS = "244466666";

// ====== CẤU HÌNH SERVER PYTHON ======
const char* SERVER_IP   = "192.168.43.168"; // IP máy chạy server Python
const int   SERVER_PORT = 5000;
const char* SERVER_PATH = "/upload";

// ====== PIN CAMERA (AI THINKER ESP32-CAM) ======
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ====== BUZZER ======
#define BUZZER_PIN 4   // BUZZER+ -> GPIO4, BUZZER- -> GND

// ====== HTTP SERVER CHO ESP32 (điều khiển còi) ======
WebServer server(80);

// Gửi 1 frame/giây
const unsigned long FRAME_INTERVAL_MS = 1000;
unsigned long lastFrameMillis = 0;

// ====== HÀM KHỞI TẠO CAMERA ======
void startCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  // mapping CHUẨN cho AI Thinker ESP32-CAM
  config.pin_d0       = Y2_GPIO_NUM;  // 5
  config.pin_d1       = Y3_GPIO_NUM;  // 18
  config.pin_d2       = Y4_GPIO_NUM;  // 19
  config.pin_d3       = Y5_GPIO_NUM;  // 21
  config.pin_d4       = Y6_GPIO_NUM;  // 36
  config.pin_d5       = Y7_GPIO_NUM;  // 39
  config.pin_d6       = Y8_GPIO_NUM;  // 34
  config.pin_d7       = Y9_GPIO_NUM;  // 35

  config.pin_xclk     = XCLK_GPIO_NUM;    // 0
  config.pin_pclk     = PCLK_GPIO_NUM;    // 22
  config.pin_vsync    = VSYNC_GPIO_NUM;   // 25
  config.pin_href     = HREF_GPIO_NUM;    // 23
  config.pin_sccb_sda = SIOD_GPIO_NUM;    // 26
  config.pin_sccb_scl = SIOC_GPIO_NUM;    // 27
  config.pin_pwdn     = PWDN_GPIO_NUM;    // 32
  config.pin_reset    = RESET_GPIO_NUM;   // -1

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    // Độ phân giải cho YOLO: 320x240 khá ổn
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240
    config.jpeg_quality = 15;              // nét vừa phải
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_QQVGA; // 160x120
    config.jpeg_quality = 20;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    while (true) {
      delay(1000);
    }
  }
  Serial.println("Camera init OK");
}

// ====== WIFI ======
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Dang ket noi WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nDa ket noi WiFi");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());
}

// ====== GỬI FRAME JPEG LÊN SERVER PYTHON ======
bool sendFrameToServer(camera_fb_t *fb) {
  WiFiClient client;

  if (!client.connect(SERVER_IP, SERVER_PORT)) {
    Serial.println("Ket noi server that bai");
    return false;
  }

  // HTTP POST header
  String header = "";
  header += "POST " + String(SERVER_PATH) + " HTTP/1.1\r\n";
  header += "Host: " + String(SERVER_IP) + "\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Length: " + String(fb->len) + "\r\n";
  header += "Connection: close\r\n\r\n";

  client.print(header);

  // Body: dữ liệu JPEG
  size_t fbLen = fb->len;
  uint8_t *fbBuf = fb->buf;
  size_t bytesSent = 0;
  const size_t CHUNK = 1024;

  while (bytesSent < fbLen) {
    size_t toSend = (fbLen - bytesSent > CHUNK) ? CHUNK : (fbLen - bytesSent);
    size_t written = client.write(fbBuf + bytesSent, toSend);
    if (written == 0) {
      Serial.println("Loi gui du lieu");
      client.stop();
      return false;
    }
    bytesSent += written;
  }

  // Đọc response (debug)
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 2000) {
      Serial.println("Server timeout");
      client.stop();
      return false;
    }
  }

  while (client.available()) {
    String line = client.readStringUntil('\n');
    Serial.println(line);
  }

  client.stop();
  return true;
}

// ====== HTTP HANDLER: BẬT/TẮT CÒI ======
void handleAlarm() {
  String on = server.hasArg("on") ? server.arg("on") : "0";
  if (on == "1") {
    digitalWrite(BUZZER_PIN, HIGH);  // còi kêu
    Serial.println("BUZZER ON");
  } else {
    digitalWrite(BUZZER_PIN, LOW);   // còi tắt
    Serial.println("BUZZER OFF");
  }
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ====== SETUP & LOOP ======
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // tắt còi

  connectWifi();
  startCamera();

  // HTTP server cho lệnh điều khiển còi
  server.on("/alarm", handleAlarm);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server bat dau (path /alarm?on=1 hoac on=0)");
}

void loop() {
  // xử lý HTTP request (bật/tắt còi)
  server.handleClient();

  // gửi frame theo chu kỳ
  if (millis() - lastFrameMillis < FRAME_INTERVAL_MS) {
    return;
  }
  lastFrameMillis = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  Serial.print("Frame size: ");
  Serial.println(fb->len);

  bool ok = sendFrameToServer(fb);
  Serial.println(ok ? "Gui frame OK" : "Gui frame FAIL");

  esp_camera_fb_return(fb);
}
