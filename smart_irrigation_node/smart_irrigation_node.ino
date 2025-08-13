#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <WebServer.h>

// --- CONFIGURATION ---
#define SOIL_PIN 34
#define VALVE_PIN 2
#define BUTTON_PIN 0
#define EEPROM_SIZE 4
#define EEPROM_ADDR 0
#define DEFAULT_THRESHOLD 30
#define AWS_ENDPOINT "https://your-mock-endpoint.amazonaws.com/irrigation"
#define AP_SSID "IrrigationNode_AP"
#define AP_PASS "12345678"
#define WIFI_CRED_EEPROM_ADDR 2
#define WIFI_CRED_MAXLEN 32
#define AWS_MQTT_PORT 8883
#define AWS_MQTT_CLIENT_ID "ESP32_Irrigation_Node"
#define AWS_MQTT_TOPIC "irrigation/data"

// --- SENSOR MODULE ---
class SoilMoistureSensor {
  int analogPin;
public:
  SoilMoistureSensor(int pin) : analogPin(pin) {}
  void begin() { pinMode(analogPin, INPUT); }
  int readPercent() {
    int raw = analogRead(analogPin);
    return map(raw, 0, 4095, 0, 100);
  }
  void deinit() { pinMode(analogPin, INPUT); }
};

// --- ACTUATOR MODULE ---
class ValveActuator {
  int ledPin;
public:
  ValveActuator(int pin) : ledPin(pin) {}
  void begin() { pinMode(ledPin, OUTPUT); }
  void on()  { digitalWrite(ledPin, HIGH); }
  void off() { digitalWrite(ledPin, LOW); }
  void deinit() { pinMode(ledPin, INPUT); }
};

// --- EEPROM MODULE ---
class ThresholdConfig {
  int threshold;
public:
  ThresholdConfig() : threshold(DEFAULT_THRESHOLD) {}
  void begin() { EEPROM.begin(EEPROM_SIZE); }
  int get() {
    int t = EEPROM.read(EEPROM_ADDR);
    if (t < 10 || t > 90) t = DEFAULT_THRESHOLD;
    threshold = t;
    return threshold;
  }
  void set(int t) {
    threshold = t;
    EEPROM.write(EEPROM_ADDR, t);
    EEPROM.commit();
  }
};

// --- CRC MODULE ---
class CRC8 {
public:
  static uint8_t calc(uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];
      for (uint8_t j = 0; j < 8; j++) {
        if (crc & 0x80)
          crc = (crc << 1) ^ 0x07;
        else
          crc <<= 1;
      }
    }
    return crc;
  }
};

// --- LORAWAN MODULE ---
class LoRaWAN {
public:
  void sendPayload(int moisturePercent, int threshold) {
    uint8_t payload[2];
    payload[0] = moisturePercent;
    payload[1] = threshold;
    uint8_t crc = CRC8::calc(payload, 2);
    Serial.print("LoRaWAN Payload: 0x");
    if (moisturePercent < 16) Serial.print("0");
    Serial.print(moisturePercent, HEX);
    Serial.print(" 0x");
    if (threshold < 16) Serial.print("0");
    Serial.print(threshold, HEX);
    Serial.print(" CRC: 0x");
    if (crc < 16) Serial.print("0");
    Serial.println(crc, HEX);
  }
};

// --- WIFI MANAGER MODULE ---
class WiFiManager {
public:
  String ssid, pass;
  bool provisioned;
  WebServer server;
  WiFiManager() : provisioned(false), server(80) {}
  void loadCredentials() {
    char ssidBuf[WIFI_CRED_MAXLEN] = {0};
    char passBuf[WIFI_CRED_MAXLEN] = {0};
    for (int i = 0; i < WIFI_CRED_MAXLEN; i++) {
      ssidBuf[i] = EEPROM.read(WIFI_CRED_EEPROM_ADDR + i);
      passBuf[i] = EEPROM.read(WIFI_CRED_EEPROM_ADDR + WIFI_CRED_MAXLEN + i);
    }
    ssid = String(ssidBuf);
    pass = String(passBuf);
    provisioned = ssid.length() > 0 && pass.length() > 0;
  }
  void saveCredentials(const String& s, const String& p) {
    for (int i = 0; i < WIFI_CRED_MAXLEN; i++) {
      EEPROM.write(WIFI_CRED_EEPROM_ADDR + i, i < s.length() ? s[i] : 0);
      EEPROM.write(WIFI_CRED_EEPROM_ADDR + WIFI_CRED_MAXLEN + i, i < p.length() ? p[i] : 0);
    }
    EEPROM.commit();
    ssid = s;
    pass = p;
    provisioned = true;
  }
  void startProvisioning() {
    Serial.println("Starting WiFi provisioning AP...");
    WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    server.on("/", HTTP_GET, [this]() {
      String html = "<html><body><h2>WiFi Provisioning</h2>"
        "<form action='/save' method='post'>"
        "SSID: <input name='ssid'><br>"
        "Password: <input name='pass' type='password'><br>"
        "<input type='submit' value='Save'>"
        "</form></body></html>";
      server.send(200, "text/html", html);
    });
    server.on("/save", HTTP_POST, [this]() {
      String newSsid = server.arg("ssid");
      String newPass = server.arg("pass");
      saveCredentials(newSsid, newPass);
      String msg = "<html><body><h2>Saved! Restarting...</h2></body></html>";
      server.send(200, "text/html", msg);
      delay(1000);
      ESP.restart();
    });
    server.begin();
    Serial.println("Provisioning web server started.");
    while (!provisioned) {
      server.handleClient();
      delay(10);
    }
  }
  void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("Connecting to WiFi");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500);
      Serial.print(".");
      tries++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi connected.");
    } else {
      Serial.println("WiFi failed.");
    }
  }
  void begin() {
    loadCredentials();
    if (!provisioned) startProvisioning();
    connectWiFi();
  }
  void deinit() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
};

// --- AWS CLOUD MODULE ---
class CloudClient {
public:
  enum Mode { HTTP, MQTT };
  static Mode commMode;
  static WiFiClientSecure secureClient;
  static PubSubClient mqttClient;
  static void beginMQTT() {
    secureClient.setCACert(aws_root_ca);
    secureClient.setCertificate(aws_cert);
    secureClient.setPrivateKey(aws_private_key);
    mqttClient.setServer(AWS_ENDPOINT, AWS_MQTT_PORT);
    mqttClient.setClient(secureClient);
    Serial.print("Connecting to AWS IoT MQTT...");
    while (!mqttClient.connected()) {
      if (mqttClient.connect(AWS_MQTT_CLIENT_ID)) {
        Serial.println("connected.");
      } else {
        Serial.print(".");
        delay(1000);
      }
    }
  }
  static void sendToAWS_HTTP(int moisture, int threshold) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = String("https://") + AWS_ENDPOINT + "/irrigation";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"moisture\":" + String(moisture) + ",\"threshold\":" + String(threshold) + "}";
    int httpResponseCode = http.POST(payload);
    Serial.print("AWS HTTP POST Response: ");
    Serial.println(httpResponseCode);
    http.end();
  }
  static void sendToAWS_MQTT(int moisture, int threshold) {
    if (!mqttClient.connected()) beginMQTT();
    String payload = "{\"moisture\":" + String(moisture) + ",\"threshold\":" + String(threshold) + "}";
    bool success = mqttClient.publish(AWS_MQTT_TOPIC, payload.c_str());
    Serial.print("AWS MQTT Publish: ");
    Serial.println(success ? "Success" : "Failed");
  }
  static void send(int moisture, int threshold) {
    if (commMode == HTTP) sendToAWS_HTTP(moisture, threshold);
    else sendToAWS_MQTT(moisture, threshold);
  }
};
CloudClient::Mode CloudClient::commMode = CloudClient::HTTP;
WiFiClientSecure CloudClient::secureClient;
PubSubClient CloudClient::mqttClient(CloudClient::secureClient);

// --- SYSTEM MANAGER ---
class SmartIrrigationNode {
  SoilMoistureSensor sensor;
  ValveActuator valve;
  ThresholdConfig thresholdConfig;
  LoRaWAN lorawan;
  WiFiManager wifi;
public:
  SmartIrrigationNode() : sensor(SOIL_PIN), valve(VALVE_PIN) {}
  void begin() {
    Serial.begin(115200);
    delay(100);
    thresholdConfig.begin();
    wifi.begin();
    sensor.begin();
    valve.begin();
  }
  void runCycle() {
    int threshold = thresholdConfig.get();
    int moisture = sensor.readPercent();
    Serial.print("Soil moisture: ");
    Serial.print(moisture);
    Serial.println("%");
    Serial.print("Threshold: ");
    Serial.println(threshold);
    if (moisture < threshold) {
      valve.on();
      Serial.println("Valve ON");
    } else {
      valve.off();
      Serial.println("Valve OFF");
    }
    lorawan.sendPayload(moisture, threshold);
    CloudClient::send(moisture, threshold);
  }
  void setThreshold(int t) {
    thresholdConfig.set(t);
    Serial.print("Threshold updated to: ");
    Serial.println(t);
  }
  void deinitHardware() {
    valve.deinit();
    sensor.deinit();
    wifi.deinit();
  }
};

// --- MAIN ---
SmartIrrigationNode node;

void setup() {
  node.begin();
  if (Serial.available()) {
    String mode = Serial.readStringUntil('\n');
    mode.trim();
    if (mode.equalsIgnoreCase("mqtt")) {
      CloudClient::commMode = CloudClient::MQTT;
    } else if (mode.equalsIgnoreCase("http")) {
      CloudClient::commMode = CloudClient::HTTP;
    }
    int newThreshold = Serial.parseInt();
    if (newThreshold >= 10 && newThreshold <= 90) {
      node.setThreshold(newThreshold);
    }
  }
  node.runCycle();
  delay(100);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up by button press!");
  } else {
    node.deinitHardware();
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
    Serial.println("Entering deep sleep...");
    esp_deep_sleep_start();
  }
}

void loop() {
  // Not used; device sleeps after setup
}