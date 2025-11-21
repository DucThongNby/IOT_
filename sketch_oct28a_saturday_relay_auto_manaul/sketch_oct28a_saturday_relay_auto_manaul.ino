#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <WebServer.h>

#define MQTT_MAX_PACKET_SIZE 512

// ===========================
// WiFi credentials
// ===========================
const char *ssid = "esp32_";
const char *password = "Hello12345";

// ===========================
// ThingsBoard MQTT setup
// ===========================
const char* mqtt_server = "mqtt.thingsboard.cloud";
const int mqtt_port = 1883;
const char* access_token = "Bne7L61gji0bCwWtzn0e";  // Thay bằng token của bạn

WiFiClient espClient;
PubSubClient client(espClient);

// ===========================
// DHT11 setup
// ===========================
#define DHTPIN 14
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ===========================
// Soil moisture sensor (Analog)
// ===========================
#define SOIL_PIN 32  // AO -> GPIO32

// ===========================
// Relay setup
// ===========================
#define RELAY_PIN 25

// ===========================
// Web Server
// ===========================
WebServer server(80);

// ===========================
// Biến trạng thái
// ===========================
bool autoMode = true;  // true = tự động, false = thủ công

// ===========================
// RPC handler
// ===========================
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("📩 Nhận RPC từ topic: %s\n", topic);
  
  // Lấy ID của request để phản hồi
  String t = topic;
  int p = t.lastIndexOf('/');
  String id = (p >= 0) ? t.substring(p + 1) : "0";
  String respTopic = "v1/devices/me/rpc/response/" + id;

  // Giải mã JSON RPC
  StaticJsonDocument<256> in;
  DeserializationError err = deserializeJson(in, payload, length);
  if (err) {
    StaticJsonDocument<64> out;
    out["ok"] = false;
    out["err"] = "bad_json";
    char buf[128];
    size_t n = serializeJson(out, buf);
    client.publish(respTopic.c_str(), buf, n);
    return;
  }

  const char* method = in["method"] | "";
  JsonVariant params = in["params"];
  bool handled = false;

  // ===== Điều khiển relay =====
  if (strcmp(method, "relay") == 0 || strcmp(method, "setRelayPower") == 0) {
    bool val = false;
    if (params.is<bool>()) val = params.as<bool>();
    else if (params.is<int>()) val = (params.as<int>() != 0);
    else if (params["value"].is<bool>()) val = params["value"].as<bool>();
    else if (params["value"].is<int>()) val = (params["value"].as<int>() != 0);

    digitalWrite(RELAY_PIN, val ? HIGH : LOW);
    Serial.printf("🔘 Relay %s từ cloud\n", val ? "BẬT" : "TẮT");

    // Phản hồi RPC
    StaticJsonDocument<96> out;
    out["ok"] = true;
    out["relay"] = val;
    char buf[128];
    size_t n = serializeJson(out, buf);
    client.publish(respTopic.c_str(), buf, n);
    handled = true;
  }

  // ===== Chuyển chế độ Auto/Manual =====
  if (strcmp(method, "mode") == 0) {
    String mode = params.as<String>();
    if (mode.equalsIgnoreCase("auto")) {
      autoMode = true;
      Serial.println("⚙️ Chuyển sang chế độ TỰ ĐỘNG (từ cloud)");
    } else if (mode.equalsIgnoreCase("manual")) {
      autoMode = false;
      Serial.println("🧭 Chuyển sang chế độ THỦ CÔNG (từ cloud)");
    }

    // Gửi phản hồi RPC
    StaticJsonDocument<96> out;
    out["ok"] = true;
    out["mode"] = autoMode ? "auto" : "manual";
    char buf[128];
    size_t n = serializeJson(out, buf);
    client.publish(respTopic.c_str(), buf, n);
    handled = true;
  }

  // ===== Yêu cầu trạng thái relay =====
  if (strcmp(method, "getRelayStatus") == 0) {
    bool current = digitalRead(RELAY_PIN);
    StaticJsonDocument<96> out;
    out["relay"] = current;
    char buf[128];
    size_t n = serializeJson(out, buf);
    client.publish(respTopic.c_str(), buf, n);
    Serial.println("↩️ Gửi trạng thái relay hiện tại về cloud");
    handled = true;
  }

  // ===== Không khớp method =====
  if (!handled) {
    StaticJsonDocument<96> out;
    out["ok"] = false;
    out["err"] = "unknown_method";
    char buf[128];
    size_t n = serializeJson(out, buf);
    client.publish(respTopic.c_str(), buf, n);
  }
}

// ===========================
// Gửi dữ liệu telemetry lên ThingsBoard
// ===========================
void sendToCloud(float t, float h, int soilPercent) {
  if (!client.connected()) {
    while (!client.connected()) {
      Serial.print("🔄 Kết nối lại MQTT...");
      if (client.connect("ESP32SmartGarden", access_token, NULL)) {
        Serial.println("✅ MQTT connected!");
        client.subscribe("v1/devices/me/rpc/request/+");
        Serial.println("📡 Subscribed to RPC topic!");
      } else {
        Serial.print("⚠️ MQTT failed, rc=");
        Serial.println(client.state());
        delay(2000);
      }
    }
  }

  String payload = "{";
  payload += "\"temperature\":" + String(t) + ",";
  payload += "\"humidity\":" + String(h) + ",";
  payload += "\"soil\":" + String(soilPercent) + ",";
  payload += "\"relay\":" + String(digitalRead(RELAY_PIN)) + ",";
  payload += "\"mode\":\"" + String(autoMode ? "auto" : "manual") + "\"";
  payload += "}";

  client.publish("v1/devices/me/telemetry", payload.c_str());
  Serial.println("📡 Gửi dữ liệu lên Cloud: " + payload);
}

// ===========================
// Web Page hiển thị cục bộ
// ===========================
void handleRoot() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int soil = analogRead(SOIL_PIN);
  int soilPercent = map(soil, 0, 4095, 100, 0);
  String relayState = digitalRead(RELAY_PIN) ? "BẬT" : "TẮT";

  String html = "<html><head><meta charset='UTF-8'></head><body style='font-family:Arial;text-align:center;background:#f5f5f5'>";
  html += "<h2>🌿 ESP32 Smart Garden Dashboard</h2>";
  html += "<p><b>Nhiệt độ:</b> " + String(t) + " °C</p>";
  html += "<p><b>Độ ẩm không khí:</b> " + String(h) + " %</p>";
  html += "<p><b>Độ ẩm đất:</b> " + String(soilPercent) + " %</p>";
  html += "<p><b>Trạng thái quạt (relay):</b> <span style='color:" + String(digitalRead(RELAY_PIN) ? "green" : "red") + "'>" + relayState + "</span></p>";
  html += "<p><b>Chế độ:</b> " + String(autoMode ? "TỰ ĐỘNG" : "THỦ CÔNG") + "</p>";
  html += "<form action='/toggle'><button style='padding:10px 20px;font-size:16px'>🔘 Bật/Tắt quạt</button></form>";
  html += "<form action='/mode'><button style='padding:10px 20px;font-size:16px;margin-top:10px'>⚙️ Chuyển chế độ</button></form>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleToggle() {
  if (!autoMode) digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMode() {
  autoMode = !autoMode;
  server.sendHeader("Location", "/");
  server.send(303);
}

void startServer() {
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/mode", handleMode);
  server.begin();
  Serial.println("HTTP server started");
}

// ===========================
// Setup
// ===========================
void setup() {
  Serial.begin(115200);
  Serial.println("\n🌿 ESP32 Smart Garden - Cloud + RPC + Serial Control");

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi connected!");
  Serial.print("🔗 IP: ");
  Serial.println(WiFi.localIP());

  // MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.subscribe("v1/devices/me/rpc/request/+");

  // Sensors
  dht.begin();
  pinMode(SOIL_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  startServer();

  Serial.println("\nNhập lệnh Serial:");
  Serial.println("  on  -> bật relay");
  Serial.println("  off -> tắt relay");
  Serial.println("  auto -> chế độ tự động");
  Serial.println("  manual -> chế độ thủ công");
}

// ===========================
// Loop
// ===========================
void loop() {
  client.loop();
  server.handleClient();

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int soil = analogRead(SOIL_PIN);
  int soilPercent = map(soil, 0, 4095, 100, 0);

  if (autoMode) {
    if (soilPercent < 40) {
      digitalWrite(RELAY_PIN, HIGH);
      Serial.println("🌬️ Quạt BẬT (đất khô)");
    } else {
      digitalWrite(RELAY_PIN, LOW);
      Serial.println("💤 Quạt TẮT (đất đủ ẩm)");
    }
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("on")) {
      autoMode = false;
      digitalWrite(RELAY_PIN, HIGH);
      Serial.println("🔘 Bật relay (thủ công)");
    } 
    else if (cmd.equalsIgnoreCase("off")) {
      autoMode = false;
      digitalWrite(RELAY_PIN, LOW);
      Serial.println("💤 Tắt relay (thủ công)");
    } 
    else if (cmd.equalsIgnoreCase("auto")) {
      autoMode = true;
      Serial.println("⚙️ Chuyển sang chế độ TỰ ĐỘNG");
    } 
    else if (cmd.equalsIgnoreCase("manual")) {
      autoMode = false;
      Serial.println("🧭 Chuyển sang chế độ THỦ CÔNG");
    } 
    else {
      Serial.println("⚠️ Lệnh không hợp lệ! Gõ: on / off / auto / manual");
    }
  }

  sendToCloud(t, h, soilPercent);
  Serial.printf("🌡️ %.1f°C | 💧 %.1f%% | 🌿 Soil: %d (%d%%)\n", t, h, soil, soilPercent);
  delay(5000);
}
