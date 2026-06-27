#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>

// ── WiFi & MQTT ──
const char* ssids[]     = {"PADDYearpay1", "brooker router", "BueaLabWiFi"};
const char* passwords[] = {"1234xxxx",     "12345678",      "labpass123"};
const int   NUM_NETWORKS = 3;
const char* mqtt_server  = "192.168.1.25";
const int   mqtt_port    = 1883;
const char* sensor_id    = "esp32_01";

// ── Pins ──
#define DHT_PIN      4
#define DHT_TYPE     DHT11
#define BUZZER_PIN   27
#define BUZZER_FREQ  2000

// ── PWM Channel ──
#define PWM_CHANNEL  0
#define PWM_RES      8  // 8-bit resolution (0-255)

// ── Sensors ──
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// ── Alarm state ──
String currentLevel  = "SAFE";
bool   alarmSilenced = false;
bool   alarmAcknowledged = false;
unsigned long lastBeepTime = 0;
unsigned long lastPredictionTime = 0;
unsigned long lastPublishTime = 0;
int beepPhase = 0;

// ── Constants ──
const unsigned long AUTO_SAFE_TIMEOUT = 60000;  // 60 seconds
const unsigned long PUBLISH_INTERVAL = 2000;    // 2 seconds

// ── LEDC tone helpers (ESP32 v3.x compatible) ──
void toneOn(int freq) {
  // Attach pin to PWM channel with frequency and resolution
  ledcAttach(BUZZER_PIN, freq, PWM_RES);
  // Set duty cycle to 50% (128/255)
  ledcWrite(BUZZER_PIN, 128);
}

void toneOff() {
  // Turn off PWM on the pin
  ledcWrite(BUZZER_PIN, 0);
  // Detach is optional but good practice
  // ledcDetach(BUZZER_PIN);
}

void beep(int durationMs, int freq = BUZZER_FREQ) {
  toneOn(freq);
  delay(durationMs);
  toneOff();
}

// ── 5-Level alarm patterns ──
void handleAlarm() {
  if (alarmSilenced) { 
    toneOff(); 
    return; 
  }
  
  unsigned long now = millis();

  if (currentLevel == "SAFE") {
    toneOff();

  } else if (currentLevel == "WATCH") {
    if (now - lastBeepTime >= 30000) {
      beep(100, 1500);
      lastBeepTime = now;
    }

  } else if (currentLevel == "WARNING") {
    if (now - lastBeepTime >= 10000) {
      beep(100, 2000); 
      delay(100); 
      beep(100, 2000);
      lastBeepTime = now;
    }

  } else if (currentLevel == "ALARM") {
    if (now - lastBeepTime >= 3000) {
      beep(80, 2500); 
      delay(80);
      beep(80, 2500); 
      delay(80);
      beep(80, 2500);
      lastBeepTime = now;
    }

  } else if (currentLevel == "EMERGENCY") {
    if (now - lastBeepTime >= 400) {
      if (beepPhase % 2 == 0) {
        toneOn(3000);
      } else {
        toneOn(1500);
      }
      beepPhase++;
      lastBeepTime = now;
    }
  }
}

// ── MQTT callback ──
void on_mqtt_message(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  
  Serial.print("📥 [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(msg);

  if (String(topic) == "edge/alarm/stop") {
    alarmSilenced = true;
    toneOff();
    Serial.println("🔇 Alarm silenced");
    return;
  }

  if (String(topic) == "edge/alarm/resume") {
    alarmSilenced = false;
    Serial.println("🔊 Alarm resumed");
    return;
  }

  if (String(topic) == "edge/alarm/acknowledge") {
    alarmAcknowledged = true;
    Serial.println("✅ Alarm acknowledged");
    return;
  }

  if (String(topic).startsWith("edge/predictions/")) {
    lastPredictionTime = millis();

    int labelIdx = msg.indexOf("\"label\"");
    if (labelIdx < 0) {
      Serial.println("⚠️ No label found");
      return;
    }

    int colonIdx = msg.indexOf(':', labelIdx);
    if (colonIdx < 0) return;
    
    int startQuote = msg.indexOf('"', colonIdx + 1);
    if (startQuote < 0) return;
    
    int endQuote = msg.indexOf('"', startQuote + 1);
    if (endQuote < 0) return;

    String newLevel = msg.substring(startQuote + 1, endQuote);
    newLevel.trim();

    Serial.print("📊 Level: '");
    Serial.print(newLevel);
    Serial.println("'");

    if (newLevel == "SAFE" || newLevel == "WATCH" || 
        newLevel == "WARNING" || newLevel == "ALARM" || 
        newLevel == "EMERGENCY") {
      
      if (newLevel != currentLevel) {
        Serial.print("🔄 Change: ");
        Serial.print(currentLevel);
        Serial.print(" -> ");
        Serial.println(newLevel);
        
        currentLevel = newLevel;
        alarmSilenced = false;
        alarmAcknowledged = false;
        lastBeepTime = 0;
        beepPhase = 0;
        
        if (newLevel == "SAFE") {
          toneOn(2000);
          delay(100);
          toneOff();
          Serial.println("✅ SAFE");
        } else if (newLevel == "WATCH") {
          toneOn(1000);
          delay(200);
          toneOff();
          Serial.println("👀 WATCH");
        } else {
          toneOn(2500);
          delay(200);
          toneOff();
          delay(100);
          toneOn(2500);
          delay(200);
          toneOff();
          Serial.println("⚠️ " + newLevel);
        }
      }
    } else {
      Serial.print("❌ Unknown: '");
      Serial.print(newLevel);
      Serial.println("'");
    }
  }

  if (String(topic) == "edge/buzzer/test") {
    Serial.println("🔊 Testing buzzer");
    toneOn(2000);
    delay(300);
    toneOn(1000);
    delay(300);
    toneOn(500);
    delay(300);
    toneOff();
    Serial.println("✅ Test complete");
  }
}

// ── MQTT reconnect ──
void reconnect_mqtt() {
  while (!client.connected()) {
    Serial.print("MQTT connecting...");
    if (client.connect(sensor_id)) {
      Serial.println("connected");
      client.subscribe("edge/predictions/#");
      client.subscribe("edge/alarm/stop");
      client.subscribe("edge/alarm/resume");
      client.subscribe("edge/alarm/acknowledge");
      client.subscribe("edge/buzzer/test");
    } else {
      Serial.print("failed rc="); 
      Serial.println(client.state());
      delay(3000);
    }
  }
}

// ── WiFi setup ──
void setup_wifi() {
  Serial.println("\n📡 Scanning WiFi...");
  int n = WiFi.scanNetworks();
  int bestMatch = -1;
  
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < NUM_NETWORKS; j++) {
      if (WiFi.SSID(i) == ssids[j]) { 
        bestMatch = j; 
        Serial.print("✅ Found: ");
        Serial.println(ssids[j]);
        break; 
      }
    }
    if (bestMatch != -1) break;
  }
  
  if (bestMatch == -1) { 
    Serial.println("❌ No known networks. Restarting...");
    delay(10000); 
    ESP.restart(); 
  }
  
  Serial.print("🔗 Connecting to ");
  Serial.println(ssids[bestMatch]);
  WiFi.begin(ssids[bestMatch], passwords[bestMatch]);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); 
    Serial.print("."); 
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi failed. Restarting...");
    ESP.restart();
  }
  
  Serial.println("\n✅ WiFi connected!");
  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP().toString());
}

// ── Setup ──
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("ESP32 INDUSTRIAL SENSOR NODE");
  Serial.println("========================================");

  // ── Boot beep ──
  beep(50, 2000);
  delay(50);
  beep(50, 2500);

  // ── Setup WiFi ──
  setup_wifi();

  // ── Setup MQTT ──
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(on_mqtt_message);

  // ── Setup BMP280 ──
  if (!bmp.begin(0x76)) {
    Serial.println("❌ BMP280 not found!");
    while (1) {
      beep(500, 1000);
      delay(500);
    }
  }
  Serial.println("✅ BMP280 initialized");

  // ── Setup DHT11 ──
  dht.begin();
  Serial.println("✅ DHT11 initialized");

  // ── Setup Buzzer PWM (ESP32 v3.x) ──
  // No explicit setup needed - ledcAttach handles it
  Serial.println("✅ Buzzer initialized");

  Serial.println("\n✅ System ready!");
  Serial.println("📡 MQTT Server: " + String(mqtt_server));
  Serial.println("📡 Sensor ID: " + String(sensor_id));
  Serial.println("========================================\n");
}

// ── Main Loop ──
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi lost. Reconnecting...");
    setup_wifi();
  }

  if (!client.connected()) {
    reconnect_mqtt();
  }
  client.loop();

  handleAlarm();

  // ── Auto-SAFE ──
  if (currentLevel != "SAFE" && !alarmSilenced) {
    if (millis() - lastPredictionTime > AUTO_SAFE_TIMEOUT) {
      Serial.println("⏰ Auto-SAFE");
      currentLevel = "SAFE";
      alarmSilenced = false;
      toneOff();
      toneOn(2000);
      delay(100);
      toneOff();
    }
  }

  // ── Publish sensor data ──
  if (millis() - lastPublishTime >= PUBLISH_INTERVAL) {
    float bmp_temp     = bmp.readTemperature();
    float bmp_pressure = bmp.readPressure() / 100.0F;
    float dht_humidity = dht.readHumidity();
    float dht_temp     = dht.readTemperature();
    float avg_temp     = (bmp_temp + dht_temp) / 2.0;

    if (!isnan(dht_humidity) && !isnan(dht_temp)) {
      char payload[300];
      snprintf(payload, sizeof(payload),
        "{\"sensor_id\":\"%s\",\"temperature\":%.2f,\"pressure\":%.2f,"
        "\"humidity\":%.2f,\"bmp_temp\":%.2f,\"dht_temp\":%.2f,"
        "\"level\":\"%s\",\"alarm_silenced\":%s,\"alarm_acknowledged\":%s}",
        sensor_id, avg_temp, bmp_pressure, dht_humidity, 
        bmp_temp, dht_temp,
        currentLevel.c_str(),
        alarmSilenced ? "true" : "false",
        alarmAcknowledged ? "true" : "false");
      
      client.publish("edge/sensors/esp32_01", payload);
      
      static int counter = 0;
      counter++;
      if (counter % 10 == 0) {
        Serial.print("📤 ");
        Serial.print(avg_temp, 1);
        Serial.print("°C | ");
        Serial.print(bmp_pressure, 1);
        Serial.print(" hPa | ");
        Serial.print(dht_humidity, 1);
        Serial.print("% | Level: ");
        Serial.println(currentLevel);
      }
    }
    lastPublishTime = millis();
  }
}
