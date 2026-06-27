#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// Uncomment ONE based on your sensor:
// For BME280 (temperature, pressure, humidity)
// Adafruit_BME280 bme;

// For BMP280 (temperature, pressure only)
#include <Adafruit_BMP280.h>
Adafruit_BMP280 bmp;

// WiFi and MQTT (add your credentials)
#include <WiFi.h>
#include <PubSubClient.h>

const char* ssid = "Overdrive injector";
const char* password = "impledown##.##";
const char* mqtt_server = "192.168.43.181";  // Your Jetson IP

WiFiClient espClient;
PubSubClient client(espClient);

// I2C pins for ESP32
#define SDA_PIN 21
#define SCL_PIN 22

// Measurement interval (milliseconds)
const long interval = 5000;
unsigned long previousMillis = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize sensor
  if (!bmp.begin(0x76)) {  // Try 0x76, if fails try 0x77
    Serial.println("BMP280 not found! Check wiring.");
    while (1);
  }
  Serial.println("BMP280 connected!");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  
  // Connect to MQTT
  client.setServer(mqtt_server, 1883);
  connectMQTT();
}

void connectMQTT() {
  while (!client.connected()) {
    if (client.connect("ESP32_Sensor")) {
      Serial.println("MQTT connected!");
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();
  
  unsigned long currentMillis = millis();
  
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    
    // Read temperature (°C)
    float temperature = bmp.readTemperature();
    
    // Read pressure (hPa)
    float pressure = bmp.readPressure() / 100.0F;
    
    // Print to Serial
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.print(" °C | Pressure: ");
    Serial.print(pressure);
    Serial.println(" hPa");
    
    // Create JSON payload
    char payload[100];
    snprintf(payload, sizeof(payload),
      "{\"temp\":%.1f,\"pressure\":%.1f}",
      temperature, pressure
    );
    
    // Publish to MQTT
    client.publish("room/sensor", payload);
  }
}
