#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h> // Install via Arduino Library Manager
#include <esp_sleep.h>

// --- CONFIGURATION ---
// Pin definitions and constants
#define SOIL_PIN 34      // Analog pin for potentiometer (simulates soil moisture)
#define VALVE_PIN 2      // Digital pin for LED (simulates valve/solenoid)
#define BUTTON_PIN 0 // GPIO for wake-up button (use GPIO0 or any available pin)
#define EEPROM_SIZE 4    // EEPROM size in bytes (enough for threshold)
#define EEPROM_ADDR 0    // EEPROM address for threshold storage
#define DEFAULT_THRESHOLD 30 // Default moisture threshold (%)

#define AWS_ENDPOINT "https://your-mock-endpoint.amazonaws.com/irrigation" // Mock AWS endpoint
#define AP_SSID "IrrigationNode_AP"
#define AP_PASS "12345678"
#define WIFI_CRED_EEPROM_ADDR 2 // EEPROM address for WiFi credentials
#define WIFI_CRED_MAXLEN 32

// --- SENSOR MODULE ---
// Reads analog value from potentiometer and converts to moisture percentage
class SoilMoistureSensor {
  int analogPin;
public:
  SoilMoistureSensor(int pin) : analogPin(pin) {}
  void begin() { /* No setup needed for analogRead */ }
  // Returns soil moisture as percentage (0-100%)
  int readPercent() {
    int raw = analogRead(analogPin); // ESP32 ADC: 0-4095
    return map(raw, 0, 4095, 0, 100); // Convert to 0-100%
  }
  void deinit() { pinMode(analogPin, INPUT); } // Optionally detach ADC
};

// --- ACTUATOR MODULE ---
// Controls the LED (simulates valve ON/OFF)
class ValveActuator {
  int ledPin;
public:
  ValveActuator(int pin) : ledPin(pin) {}
  void begin() { pinMode(ledPin, OUTPUT); }
  void on()  { digitalWrite(ledPin, HIGH); } // Turn LED ON (valve open)
  void off() { digitalWrite(ledPin, LOW); }  // Turn LED OFF (valve closed)
  void deinit() { pinMode(ledPin, INPUT); }  // De-initialize pin
};

// --- EEPROM MODULE ---
// Stores and retrieves the moisture threshold value
class ThresholdConfig {
public:
  static void begin() { EEPROM.begin(EEPROM_SIZE); }
  // Get threshold from EEPROM, with sanity check
  static int getThreshold() {
    int t = EEPROM.read(EEPROM_ADDR);
    if (t < 10 || t > 90) return DEFAULT_THRESHOLD; // Use default if out of bounds
    return t;
  }
  // Save threshold to EEPROM
  static void setThreshold(int t) {
    EEPROM.write(EEPROM_ADDR, t);
    EEPROM.commit();
  }
};

// --- CRC MODULE ---
// Calculates CRC-8 for payload integrity
uint8_t crc8(uint8_t *data, size_t len) {
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

// --- LORAWAN MODULE ---
// Simulates LoRaWAN payload and prints to Serial
class LoRaWAN {
public:
  // Format: [moisture, threshold, CRC]
  static void sendPayload(int moisturePercent, int threshold) {
    uint8_t payload[2];
    payload[0] = moisturePercent;
    payload[1] = threshold;
    uint8_t crc = crc8(payload, 2);
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
#include <WebServer.h>
class WiFiManager {
public:
  static String ssid;
  static String pass;
  static bool provisioned;
  static WebServer server;

  // Load credentials from EEPROM
  static void loadCredentials() {
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

  // Save credentials to EEPROM
  static void saveCredentials(const String& s, const String& p) {
    for (int i = 0; i < WIFI_CRED_MAXLEN; i++) {
      EEPROM.write(WIFI_CRED_EEPROM_ADDR + i, i < s.length() ? s[i] : 0);
      EEPROM.write(WIFI_CRED_EEPROM_ADDR + WIFI_CRED_MAXLEN + i, i < p.length() ? p[i] : 0);
    }
    EEPROM.commit();
    ssid = s;
    pass = p;
    provisioned = true;
  }

  // Start AP and web server for provisioning
  static void startProvisioning() {
    Serial.println("Starting WiFi provisioning AP...");
    WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    server.on("/", HTTP_GET, []() {
      String html = "<html><body><h2>WiFi Provisioning</h2>"
        "<form action='/save' method='post'>"
        "SSID: <input name='ssid'><br>"
        "Password: <input name='pass' type='password'><br>"
        "<input type='submit' value='Save'>"
        "</form></body></html>";
      server.send(200, "text/html", html);
    });
    server.on("/save", HTTP_POST, []() {
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
    // Wait for credentials to be set
    while (!provisioned) {
      server.handleClient();
      delay(10);
    }
  }

  // Connect to WiFi using stored credentials
  static void connectWiFi() {
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

  // Main entry: load credentials, provision if needed, then connect
  static void begin() {
    loadCredentials();
    if (!provisioned) {
      startProvisioning();
    }
    connectWiFi();
  }
  static void deinit() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
};

String WiFiManager::ssid = "";
String WiFiManager::pass = "";
bool WiFiManager::provisioned = false;
WebServer WiFiManager::server(80);


// --- AWS CLOUD MODULE ---
// Supports both HTTP and MQTT for AWS IoT Core
#define AWS_MQTT_PORT 8883
#define AWS_MQTT_CLIENT_ID "ESP32_Irrigation_Node"
#define AWS_MQTT_TOPIC "irrigation/data"

// Provide your AWS IoT Core certificate, private key, and CA certificate as strings
const char* aws_root_ca = "-----BEGIN CERTIFICATE-----\n...your CA cert...\n-----END CERTIFICATE-----";
const char* aws_cert = "-----BEGIN CERTIFICATE-----\n...your device cert...\n-----END CERTIFICATE-----";
const char* aws_private_key = "-----BEGIN PRIVATE KEY-----\n...your private key...\n-----END PRIVATE KEY-----";

class CloudClient {
public:
  enum Mode { HTTP, MQTT };
  static Mode commMode;
  static WiFiClientSecure secureClient;
  static PubSubClient mqttClient;

  // Initialize MQTT client and certificates
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

  // Send data to AWS endpoint as JSON via HTTP POST
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

  // Send data to AWS IoT Core via MQTT
  static void sendToAWS_MQTT(int moisture, int threshold) {
    if (!mqttClient.connected()) beginMQTT();
    String payload = "{\"moisture\":" + String(moisture) + ",\"threshold\":" + String(threshold) + "}";
    bool success = mqttClient.publish(AWS_MQTT_TOPIC, payload.c_str());
    Serial.print("AWS MQTT Publish: ");
    Serial.println(success ? "Success" : "Failed");
  }

  // Unified send function
  static void send(int moisture, int threshold) {
    if (commMode == HTTP) {
      sendToAWS_HTTP(moisture, threshold);
    } else {
      sendToAWS_MQTT(moisture, threshold);
    }
  }
};

// Static member initialization
CloudClient::Mode CloudClient::commMode = CloudClient::HTTP;
WiFiClientSecure CloudClient::secureClient;
PubSubClient CloudClient::mqttClient(CloudClient::secureClient);

// --- POWER MANAGER MODULE ---
// Manages deep sleep for power saving
class PowerManager {
public:
  // Enter deep sleep for specified seconds, support button wakeup
  static void deepSleep(uint64_t seconds) {
    Serial.println("De-initializing hardware...");
    deinitHardware();
    Serial.println("Configuring wakeup sources...");
    // Enable timer wakeup
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL); // Convert seconds to microseconds
    // Enable ext0 wakeup (button press, active LOW)
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); // Wake on LOW
    Serial.println("Entering deep sleep...");
    esp_deep_sleep_start();
  }
};

// --- CONTROLLER MODULE ---
// Main logic: reads sensor, controls actuator, sends payloads
class IrrigationController {
  SoilMoistureSensor* sensor;
  ValveActuator* valve;
  int threshold;
public:
  IrrigationController(SoilMoistureSensor* s, ValveActuator* v)
    : sensor(s), valve(v) {}
  // Initialize sensor, actuator, and threshold
  void begin() {
    sensor->begin();
    valve->begin();
    threshold = ThresholdConfig::getThreshold();
  }
  // Run one irrigation cycle: sense, act, report
  void runCycle() {
    int moisture = sensor->readPercent();
    Serial.print("Soil moisture: ");
    Serial.print(moisture);
    Serial.println("%");
    Serial.print("Threshold: ");
    Serial.println(threshold);
    // Actuator logic: turn valve ON if moisture below threshold
    if (moisture < threshold) {
      valve->on();
      Serial.println("Valve ON");
    } else {
      valve->off();
      Serial.println("Valve OFF");
    }
  // Simulate LoRaWAN packet
  LoRaWAN::sendPayload(moisture, threshold);
  // Send data to AWS cloud (HTTP or MQTT)
  CloudClient::send(moisture, threshold);
  }
  // Update threshold and save to EEPROM
  void setThreshold(int t) {
    threshold = t;
    ThresholdConfig::setThreshold(t);
    Serial.print("Threshold updated to: ");
    Serial.println(threshold);
  }
};

// --- GLOBALS ---
// Instantiate sensor, actuator, and controller objects
SoilMoistureSensor soilSensor(SOIL_PIN);
ValveActuator valve(VALVE_PIN);
IrrigationController controller(&soilSensor, &valve);

// --- DEINIT FUNCTION ---
// Hardware de-initialization before deep sleep
void deinitHardware() {
  valve.deinit();
  soilSensor.deinit();
  WiFiManager::deinit();
  // Add more deinit calls if needed
}

// --- SETUP ---
// Runs once after reset/wake from deep sleep
void setup() {
  Serial.begin(115200);
  delay(100); // Allow Serial to initialize
  ThresholdConfig::begin(); // Initialize EEPROM
  WiFiManager::begin();     // WiFi provisioning and connection
  controller.begin();       // Initialize controller
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
      controller.setThreshold(newThreshold);
    }
  }
  controller.runCycle();    // Run one irrigation cycle
  delay(100);               // Allow output to flush
  // If woke from button, skip sleep (optional)
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up by button press!");
    // Optionally, add logic for manual cycle or config
  } else {
    PowerManager::deepSleep(60); // Sleep for 1 minute or until button press
  }
}

// --- LOOP ---
// Not used; device sleeps after setup
void loop() {
  // No code needed here
}