// Smart Irrigation Node for ESP32
// This firmware reads soil moisture, controls a valve, simulates LoRaWAN payload, and sends data to AWS IoT via HTTP/MQTT.
// WiFi credentials are provisioned via AP and web server. Device sleeps for 1 minute or until button press.

#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <WebServer.h>
#include <ArduinoOTA.h> // OTA library for ESP32

// --- CONFIGURATION ---
#define SOIL_PIN 34 // Analog pin for soil moisture sensor (potentiometer)
#define VALVE_PIN 2 // Digital pin for valve actuator (LED)
#define BUTTON_PIN 0 // GPIO for wake-up button
#define EEPROM_SIZE (WIFI_CRED_EEPROM_ADDR + 2*WIFI_CRED_MAXLEN) // EEPROM size for threshold + WiFi credentials
#define EEPROM_ADDR 0 // EEPROM address for threshold
#define DEFAULT_THRESHOLD 30 // Default moisture threshold (%)
#define AWS_MQTT_HOST "your-aws-iot-host.amazonaws.com" // Host for MQTT
#define AWS_ENDPOINT "https://your-mock-endpoint.amazonaws.com/irrigation" // Full URL for HTTP
#define AP_SSID "IrrigationNode_AP" // AP SSID for provisioning
#define AP_PASS "12345678" // AP password
#define WIFI_CRED_EEPROM_ADDR 2 // EEPROM address for WiFi credentials
#define WIFI_CRED_MAXLEN 32 // Max length for SSID/password
#define AWS_MQTT_PORT 8883 // AWS IoT MQTT port
#define AWS_MQTT_CLIENT_ID "ESP32_Irrigation_Node" // MQTT client ID
#define AWS_MQTT_TOPIC "irrigation/data" // MQTT topic

// --- AWS CERTIFICATES (REQUIRED FOR MQTT) ---
// These must be filled with your actual AWS IoT Core credentials
const char* aws_root_ca = "-----BEGIN CERTIFICATE-----\nYOUR_CA_CERT_HERE\n-----END CERTIFICATE-----";
const char* aws_cert = "-----BEGIN CERTIFICATE-----\nYOUR_DEVICE_CERT_HERE\n-----END CERTIFICATE-----";
const char* aws_private_key = "-----BEGIN PRIVATE KEY-----\nYOUR_PRIVATE_KEY_HERE\n-----END PRIVATE KEY-----";

// --- SENSOR MODULE ---
// Reads analog value from potentiometer and converts to moisture percentage
class SoilMoistureSensor {
  int analogPin; // Pin connected to soil moisture sensor
public:
  // Constructor: sets the analog pin
  SoilMoistureSensor(int pin) : analogPin(pin) {}
  // Initializes the sensor pin
  void begin() { pinMode(analogPin, INPUT); }
  // Reads the analog value and converts to percentage (0-100%)
  int readPercent() {
    int raw = analogRead(analogPin);
    return map(raw, 0, 4095, 0, 100);
  }
  // De-initializes the sensor pin (for power saving)
  void deinit() { pinMode(analogPin, INPUT); }
};

// --- ACTUATOR MODULE ---
// Controls the valve (LED) to irrigate when needed
class ValveActuator {
  int ledPin; // Pin connected to valve actuator (LED)
public:
  // Constructor: sets the digital pin
  ValveActuator(int pin) : ledPin(pin) {}
  // Initializes the actuator pin
  void begin() { pinMode(ledPin, OUTPUT); }
  // Turns the valve ON (LED ON)
  void on()  { digitalWrite(ledPin, HIGH); }
  // Turns the valve OFF (LED OFF)
  void off() { digitalWrite(ledPin, LOW); }
  // De-initializes the actuator pin (for power saving)
  void deinit() { pinMode(ledPin, INPUT); }
};

// --- EEPROM MODULE ---
// Stores and retrieves the moisture threshold value
class ThresholdConfig {
  int threshold; // Moisture threshold value
public:
  // Constructor: sets default threshold
  ThresholdConfig() : threshold(DEFAULT_THRESHOLD) {}
  // Initializes EEPROM
  void begin() { EEPROM.begin(EEPROM_SIZE); }
  // Gets threshold from EEPROM, with sanity check
  int get() {
    int t = EEPROM.read(EEPROM_ADDR);
    if (t < 10 || t > 90) t = DEFAULT_THRESHOLD;
    threshold = t;
    return threshold;
  }
  // Sets threshold and saves to EEPROM
  void set(int t) {
    threshold = t;
    EEPROM.write(EEPROM_ADDR, t);
    EEPROM.commit();
  }
};

// --- CRC MODULE ---
// Calculates CRC-8 for payload integrity
class CRC8 {
public:
  // Static method to calculate CRC-8
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
// Simulates LoRaWAN payload and prints to Serial
class LoRaWAN {
public:
  // Formats and prints payload as hex, including CRC
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
// Handles WiFi connection and provisioning via AP and web server
class WiFiManager {
public:
  String ssid, pass; // WiFi credentials
  bool provisioned; // True if credentials are set
  WebServer server; // Web server for provisioning
  WiFiManager() : provisioned(false), server(80) {}
  // Loads credentials from EEPROM
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
  // Saves credentials to EEPROM
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
  // Starts AP and web server for WiFi provisioning
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
  // Connects to WiFi using stored credentials
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
  // Main entry: loads credentials, provisions if needed, then connects
  void begin() {
    loadCredentials();
    if (!provisioned) startProvisioning();
    connectWiFi();
  }
  // De-initializes WiFi (for power saving)
  void deinit() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
};

// --- AWS CLOUD MODULE ---
// Handles sending data to AWS IoT Core via HTTP or MQTT
class CloudClient {
public:
  enum Mode { HTTP, MQTT };
  static Mode commMode; // Communication mode
  static WiFiClientSecure secureClient; // Secure client for MQTT
  static PubSubClient mqttClient; // MQTT client
  // Initializes MQTT client and certificates
  static void beginMQTT() {
    secureClient.setCACert(aws_root_ca);
    secureClient.setCertificate(aws_cert);
    secureClient.setPrivateKey(aws_private_key);
    mqttClient.setServer(AWS_MQTT_HOST, AWS_MQTT_PORT); // Use host for MQTT
    mqttClient.setClient(secureClient);
    Serial.print("Connecting to AWS IoT MQTT...");
    int retry = 0;
    while (!mqttClient.connected() && retry < 10) {
      if (mqttClient.connect(AWS_MQTT_CLIENT_ID)) {
        Serial.println("connected.");
      } else {
        Serial.print(".");
        delay(1000);
        retry++;
      }
    }
    if (!mqttClient.connected()) Serial.println("MQTT connection failed!");
  }
  // Sends data to AWS endpoint as JSON via HTTP POST
  static void sendToAWS_HTTP(int moisture, int threshold) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi not connected. HTTP POST skipped.");
      return;
    }
    HTTPClient http;
    String url = AWS_ENDPOINT;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"moisture\":" + String(moisture) + ",\"threshold\":" + String(threshold) + "}";
    int httpResponseCode = http.POST(payload);
    Serial.print("AWS HTTP POST Response: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode <= 0) Serial.println("HTTP POST failed!");
    http.end();
  }
  // Sends data to AWS IoT Core via MQTT
  static void sendToAWS_MQTT(int moisture, int threshold) {
    if (!mqttClient.connected()) beginMQTT();
    String payload = "{\"moisture\":" + String(moisture) + ",\"threshold\":" + String(threshold) + "}";
    bool success = mqttClient.publish(AWS_MQTT_TOPIC, payload.c_str());
    Serial.print("AWS MQTT Publish: ");
    Serial.println(success ? "Success" : "Failed");
  }
  // Unified send function: chooses HTTP or MQTT
  static void send(int moisture, int threshold) {
    if (commMode == HTTP) sendToAWS_HTTP(moisture, threshold);
    else sendToAWS_MQTT(moisture, threshold);
  }
};
// Static member initialization
CloudClient::Mode CloudClient::commMode = CloudClient::HTTP;
WiFiClientSecure CloudClient::secureClient;
PubSubClient CloudClient::mqttClient(CloudClient::secureClient);

// --- OTA MODULE ---
// Handles Over-the-Air firmware updates
class OTAUpdater {
public:
  // Initializes OTA and sets up event handlers
  void begin(const char* hostname = "SmartIrrigationNode") {
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.onStart([]() {
      Serial.println("OTA Update Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("OTA Update End");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA Progress: %u%%\n", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("OTA Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("OTA Ready. Update via network.");
  }
  // Handles OTA events (call in loop if not sleeping)
  void handle() {
    ArduinoOTA.handle();
  }
};

// --- BUTTON MODULE ---
// Handles button events and long/short press detection
class Button {
  uint8_t pin;
public:
  Button(uint8_t p) : pin(p) {}
  void begin() { pinMode(pin, INPUT_PULLUP); }
  // Returns true if button is currently pressed (active LOW)
  bool isPressed() { return digitalRead(pin) == LOW; }
  // Waits for a long press (durationMs), returns true if detected
  bool waitForLongPress(uint32_t durationMs) {
    uint32_t start = millis();
    while (isPressed()) {
      if (millis() - start >= durationMs) return true;
      delay(10);
    }
    return false;
  }
  // Waits for a short press (durationMs), returns true if detected
  bool waitForShortPress(uint32_t durationMs) {
    uint32_t start = millis();
    while (isPressed()) {
      if (millis() - start >= durationMs) return true;
      delay(10);
    }
    return false;
  }
};

// --- SYSTEM MANAGER ---
// Main class that manages all modules and system logic
class SmartIrrigationNode {
  SoilMoistureSensor sensor; // Soil moisture sensor
  ValveActuator valve;       // Valve actuator
  ThresholdConfig thresholdConfig; // Threshold config
  LoRaWAN lorawan;           // LoRaWAN simulator
  WiFiManager wifi;          // WiFi manager
  OTAUpdater ota;            // OTA updater
  Button button;             // Button handler
public:
  // Constructor: initializes sensor, valve, and button pins
  SmartIrrigationNode() : sensor(SOIL_PIN), valve(VALVE_PIN), button(BUTTON_PIN) {}
  // Initializes all modules
  void begin() {
    Serial.begin(115200);
    delay(100);
    thresholdConfig.begin();
    wifi.begin();
    sensor.begin();
    valve.begin();
    ota.begin();
    button.begin();
  }
  // Runs one irrigation cycle: sense, act, report
  void runCycle() {
    int threshold = thresholdConfig.get();
    int moisture = sensor.readPercent();
    Serial.print("Soil moisture: ");
    Serial.print(moisture);
    Serial.println("%");
    Serial.print("Threshold: ");
    Serial.println(threshold);
    // Actuator logic: turn valve ON if moisture below threshold
    if (moisture < threshold) {
      valve.on();
      Serial.println("Valve ON");
    } else {
      valve.off();
      Serial.println("Valve OFF");
    }
    // Simulate LoRaWAN packet
    lorawan.sendPayload(moisture, threshold);
    // Send data to AWS cloud (HTTP or MQTT)
    CloudClient::send(moisture, threshold);
  }
  // Updates threshold and saves to EEPROM
  void setThreshold(int t) {
    thresholdConfig.set(t);
    Serial.print("Threshold updated to: ");
    Serial.println(t);
  }
  // De-initializes all hardware for power saving
  void deinitHardware() {
    valve.deinit();
    sensor.deinit();
    wifi.deinit();
  }
  // Handles OTA events (call in loop if not sleeping)
  void handleOTA() {
    ota.handle();
  }
  // Handles button events for wakeup and OTA mode
  void handleButtonEvents() {
    // If woke by button, check for long press (OTA mode)
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Woke up by button press!");
      if (button.isPressed() && button.waitForLongPress(5000)) {
        Serial.println("Entering OTA mode for 5 minutes...");
        uint32_t otaStart = millis();
        while (millis() - otaStart < 5 * 60 * 1000UL) {
          handleOTA();
          delay(10);
          // If button pressed again, break early
          if (button.isPressed() && button.waitForShortPress(2000)) {
            Serial.println("Exiting OTA mode early due to button press.");
            break;
          }
        }
        Serial.println("OTA mode finished. Going to deep sleep.");
        deinitHardware();
        button.begin();
        esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
        Serial.println("Entering deep sleep...");
        esp_deep_sleep_start();
      } else {
        // Normal wakeup by button, run cycle and sleep
        deinitHardware();
        button.begin();
        esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
        Serial.println("Entering deep sleep...");
        esp_deep_sleep_start();
      }
    } else {
      // Woke by timer, run cycle and sleep
      deinitHardware();
      button.begin();
      esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
      Serial.println("Entering deep sleep...");
      esp_deep_sleep_start();
    }
  }
};

// --- MAIN ---
// Instantiates the system manager and runs the main logic
SmartIrrigationNode node;

// Helper function to detect long button press (5 seconds)
bool isLongButtonPress(uint8_t pin, uint32_t durationMs = 5000) {
  uint32_t start = millis();
  while (digitalRead(pin) == LOW) {
    if (millis() - start >= durationMs) return true;
    delay(10);
  }
  return false;
}

void setup() {
  node.begin();
  // Optional: set communication mode via Serial (type "mqtt" or "http")
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
  node.handleButtonEvents();
}

void loop() {
  // do nothing
}