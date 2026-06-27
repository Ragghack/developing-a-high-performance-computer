#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>

// ── WiFi & MQTT ──
const char* ssids[]     = {"PADDYearpay1", "brooker router", "BueaLabWiFi"};
const char* passwords[] = {"1234xxxx",     "12345678",  "labpass123"};
const int   NUM_NETWORKS = 3;
const char* mqtt_server  = "192.168.1.25";
const int   mqtt_port    = 1883;
const char* sensor_id    = "esp32_01";

// ── Pins ──
#define DHT_PIN      4
#define DHT_TYPE     DHT11
#define BUZZER_PIN   27
#define BUZZER_FREQ  2000

// ── Sensors ──
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// ── Alarm state ──
String currentLevel  = "SAFE";
bool   alarmSilenced = false;
unsigned long lastBeepTime = 0;
int beepPhase = 0;

// ── LEDC tone helpers (Arduino core v3.x) ──
void toneOn(int freq) {
  ledcAttach(BUZZER_PIN, freq, 8);
  ledcWrite(BUZZER_PIN, 128);
}

void toneOff() {
  ledcWrite(BUZZER_PIN, 0);
  ledcDetach(BUZZER_PIN);
}

void beep(int durationMs, int freq = BUZZER_FREQ) {
  toneOn(freq);
  delay(durationMs);
  toneOff();
}

// ── 5-Level alarm patterns (non-blocking) ──
void handleAlarm() {
  if (alarmSilenced) { toneOff(); return; }
  unsigned long now = millis();

  if (currentLevel == "SAFE") {
    // silent

  } else if (currentLevel == "WATCH") {
    if (now - lastBeepTime >= 30000) {
      beep(100, 1500);
      lastBeepTime = now;
    }

  } else if (currentLevel == "WARNING") {
    if (now - lastBeepTime >= 10000) {
      beep(100, 2000); delay(100); beep(100, 2000);
      lastBeepTime = now;
    }

  } else if (currentLevel == "ALARM") {
    if (now - lastBeepTime >= 3000) {
      beep(80, 2500); delay(80);
      beep(80, 2500); delay(80);
      beep(80, 2500);
      lastBeepTime = now;
    }

  } else if (currentLevel == "EMERGENCY") {
    if (now - lastBeepTime >= 400) {
      if (beepPhase % 2 == 0) toneOn(3000);
      else                     toneOn(1500);
      beepPhase++;
      lastBeepTime = now;
    }
  }
}

// ── MQTT callback ──
void on_mqtt_message(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  // Replace this section in on_mqtt_message:
if (String(topic).startsWith("edge/predictions/")) {
  // More robust label extraction
  String payload = msg;
  
  // Find "label" key (last occurrence to avoid lab_label)
  int labelIdx = payload.lastIndexOf("\"label\":");
  if (labelIdx >= 0) {
    int start = payload.indexOf('"', labelIdx + 8) + 1;
    int end   = payload.indexOf('"', start);
    String newLevel = payload.substring(start, end);
    newLevel.trim();
    
    // Validate it's a known level
    if (newLevel == "SAFE" || newLevel == "WATCH" || 
        newLevel == "WARNING" || newLevel == "ALARM" || 
        newLevel == "EMERGENCY") {
      if (newLevel != currentLevel) {
        Serial.println("Level: " + currentLevel + " -> " + newLevel);
        currentLevel  = newLevel;
        alarmSilenced = false;
        lastBeepTime  = 0;
      }
    } else {
      Serial.println("Unknown level: " + newLevel);
    }
  }
}
}

void reconnect_mqtt() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect(sensor_id)) {
      Serial.println("connected");
      client.subscribe("edge/predictions/#");
      client.subscribe("edge/alarm/stop");
      client.subscribe("edge/alarm/resume");
    } else {
      Serial.print("failed rc="); Serial.println(client.state());
      delay(3000);
    }
  }
}

void setup_wifi() {
  int n = WiFi.scanNetworks();
  int bestMatch = -1;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < NUM_NETWORKS; j++) {
      if (WiFi.SSID(i) == ssids[j]) { bestMatch = j; break; }
    }
    if (bestMatch != -1) break;
  }
  if (bestMatch == -1) { delay(10000); ESP.restart(); }
  WiFi.begin(ssids[bestMatch], passwords[bestMatch]);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) ESP.restart();
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);

  // Startup confirmation — 3 ascending beeps
  beep(100, 1000); delay(50);
  beep(100, 2000); delay(50);
  beep(200, 3000);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(on_mqtt_message);

  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 not found!"); while (1);
  }
  dht.begin();
  Serial.println("Ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) reconnect_mqtt();
  client.loop();

  handleAlarm();

  static unsigned long lastPublish = 0;
  if (millis() - lastPublish >= 2000) {
    float bmp_temp     = bmp.readTemperature();
    float bmp_pressure = bmp.readPressure() / 100.0F;
    float dht_humidity = dht.readHumidity();
    float dht_temp     = dht.readTemperature();
    float avg_temp     = (bmp_temp + dht_temp) / 2.0;

    if (!isnan(dht_humidity) && !isnan(dht_temp)) {
      char payload[250];
      snprintf(payload, sizeof(payload),
        "{\"sensor_id\":\"%s\",\"temperature\":%.2f,\"pressure\":%.2f,"
        "\"humidity\":%.2f,\"bmp_temp\":%.2f,\"dht_temp\":%.2f}",
        sensor_id, avg_temp, bmp_pressure, dht_humidity, bmp_temp, dht_temp);
      client.publish("edge/sensors/esp32_01", payload);
      Serial.println(payload);
    }
    lastPublish = millis();
  }
}
