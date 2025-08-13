# Smart Irrigation Node for ESP32

## Overview
This project implements a low-power smart irrigation node using an ESP32. The node reads soil moisture, controls a valve (simulated by an LED), simulates LoRaWAN payloads, and sends data to AWS IoT Core via HTTP or MQTT. WiFi credentials are provisioned via an onboard AP and web server. The device enters deep sleep for 1 minute or until a button is pressed to wake up.

## Features
- **Soil Moisture Sensing:** Reads analog values from a potentiometer (simulating a soil moisture sensor) and converts them to a percentage.
- **Actuator Control:** Controls a valve (LED) based on soil moisture threshold. Turns ON if moisture < threshold, OFF otherwise.
- **LoRaWAN Simulation:** Formats and prints payloads (including CRC) to the Serial Monitor, simulating LoRaWAN transmission.
- **AWS IoT Integration:** Sends data to AWS IoT Core using HTTP POST or MQTT publish. Secure connection with device certificates.
- **WiFi Provisioning:** If credentials are missing, the device starts as an AP and serves a web page for SSID/password input. Credentials are saved to EEPROM.
- **Power Optimization:** Uses deep sleep for 1 minute between readings. Can wake up via RTC timer or button press.
- **EEPROM Storage:** Stores moisture threshold and WiFi credentials securely.

## Hardware Requirements
- ESP32 development board
- LED (simulates valve/solenoid)
- Potentiometer (simulates soil moisture sensor)
- Push button (for wake-up)
- Resistors, wires

## System Block Diagram

```mermaid
flowchart TD
    SoilSensor[Potentiometer (Soil Sensor)] -->|Analog| ESP32
    Valve[LED (Valve)] -->|Digital| ESP32
    Button[Push Button] -->|Digital| ESP32
    ESP32 -->|WiFi| Cloud[AWS_IoT_Core]
    ESP32 -->|Serial| User[Serial_Monitor]
```

> **Note:** The original diagram uses Mermaid syntax, which is not supported by GitHub or most Markdown viewers. For a graphical view, use the [Mermaid Live Editor](https://mermaid.live/) or a Markdown editor with Mermaid support. Below is an ASCII diagram for universal compatibility.

```
+-------------------+      +-------------------+
| Potentiometer     |      | LED (Valve)       |
| (Soil Sensor)     |      |                   |
+-------------------+      +-------------------+
         | Analog                | Digital
         v                       v
      +-------------------------------+
      |           ESP32               |
      |  - Reads Soil Moisture        |
      |  - Controls Valve             |
      |  - Handles Button             |
      |  - WiFi Provisioning          |
      |  - Sends Data to AWS IoT Core |
      +-------------------------------+
         ^                       ^
         | Digital               |
+-------------------+            |
| Push Button       |            |
+-------------------+            |
                                 |
      +-------------------------------+
      |         AWS IoT Core           |
      +-------------------------------+
```

## Advanced Usage Scenarios
### 1. Remote Monitoring & Control
- Integrate with AWS IoT rules to trigger notifications or control irrigation remotely.
- Use AWS Lambda to process incoming data and update dashboards.
- Add a mobile/web dashboard to visualize soil moisture and control the valve remotely via MQTT.

### 2. Multiple Sensor Nodes
- Deploy multiple ESP32 nodes in different zones.
- Each node publishes to a unique MQTT topic (e.g., `irrigation/zone1`, `irrigation/zone2`).
- Central server or cloud function aggregates data and controls irrigation schedules.

### 3. OTA Updates
- Integrate OTA (Over-the-Air) firmware updates using ArduinoOTA or ESP32 HTTP Update.
- Remotely update logic, thresholds, or add new features without physical access.

### 4. Data Logging
- Log sensor data to AWS DynamoDB or S3 for historical analysis.
- Use AWS IoT Analytics for trend detection and predictive irrigation.

### 5. Security Enhancements
- Use device certificates and AWS IoT policies for secure communication.
- Implement secure boot and flash encryption on ESP32 for tamper resistance.

## Additional Documentation
### Class Responsibilities
- **SoilMoistureSensor:** Reads and processes analog soil moisture data.
- **ValveActuator:** Controls irrigation valve (LED) and supports power saving.
- **ThresholdConfig:** Stores/retrieves threshold from EEPROM.
- **CRC8:** Ensures payload integrity for LoRaWAN simulation.
- **LoRaWAN:** Simulates LoRaWAN packet format and prints to Serial.
- **WiFiManager:** Handles WiFi provisioning, connection, and deinit.
- **CloudClient:** Sends data to AWS IoT Core via HTTP/MQTT, manages secure connections.
- **SmartIrrigationNode:** Orchestrates all modules, manages cycles, sleep/wake logic.

### Main Logic Flow
```mermaid
sequenceDiagram
    participant User
    participant ESP32
    participant AWS
    User->>ESP32: Power on / Reset
    ESP32->>ESP32: Initialize modules
    ESP32->>User: Start AP for WiFi provisioning (if needed)
    User->>ESP32: Enter WiFi credentials via web page
    ESP32->>ESP32: Connect to WiFi
    ESP32->>ESP32: Read soil moisture
    ESP32->>ESP32: Compare with threshold
    ESP32->>ESP32: Control valve (LED)
    ESP32->>User: Print status to Serial
    ESP32->>AWS: Send data (HTTP/MQTT)
    ESP32->>ESP32: Enter deep sleep
    User->>ESP32: Press button to wake
    ESP32->>ESP32: Repeat cycle
```

### Example AWS IoT Rule
```sql
SELECT * FROM 'irrigation/data' WHERE moisture < 30
```
- Triggers an alert or Lambda function if soil moisture drops below threshold.

### Example Serial Output
```
Soil moisture: 28%
Threshold: 30
Valve ON
LoRaWAN Payload: 0x1C 0x1E CRC: 0x02
AWS HTTP POST Response: 200
Entering deep sleep...
```

### Example WiFi Provisioning Web Page
```
http://192.168.4.1/
------------------
WiFi Provisioning
SSID: [__________]
Password: [__________]
[Save]
```

## Button Handling and OTA Feature

### Button Handling
- The push button connected to `BUTTON_PIN` is used for waking up the device from deep sleep and for entering OTA update mode.
- **Wake-Up:**
  - If the device is in deep sleep, pressing the button will wake it up and start a new irrigation cycle.
- **OTA Mode:**
  - If the button is held for more than 5 seconds after wake-up, the device enters OTA (Over-the-Air) update mode.
  - In OTA mode, the device is available for firmware updates via the ArduinoOTA protocol for 5 minutes.
  - If the button is pressed again for a short duration (2 seconds) during OTA mode, the device will exit OTA mode early and return to deep sleep.
- **Debouncing and Detection:**
  - The firmware uses time-based detection for long and short presses, ensuring reliable event handling.

#### Example Button Usage
```
- Press button briefly: Device wakes up, runs irrigation cycle, and returns to sleep.
- Hold button for >5 seconds: Device enters OTA mode for 5 minutes.
- Press button again for ~2 seconds during OTA mode: Device exits OTA mode and sleeps.
```

### OTA (Over-the-Air) Updates
- OTA updates are supported using the ArduinoOTA library.
- When in OTA mode, you can upload new firmware from the Arduino IDE or compatible tools over WiFi.
- The device advertises itself on the network with the hostname `SmartIrrigationNode`.
- After 5 minutes (or early exit via button), the device returns to deep sleep to save power.

#### How to Use OTA
1. Hold the button for 5 seconds after device wakes up to enter OTA mode.
2. In Arduino IDE, select the network port corresponding to `SmartIrrigationNode`.
3. Upload your new firmware.
4. The device will automatically reboot and return to deep sleep after OTA mode ends.

#### Troubleshooting OTA
- Ensure your computer and ESP32 are on the same WiFi network.
- If OTA fails, check firewall settings and ensure the device is in OTA mode.
- If the device does not appear for OTA, try waking it and holding the button again.

## Troubleshooting
- **Include Errors:** Make sure you have ESP32 board support and all required libraries installed in Arduino IDE.
- **AWS Connection:** Ensure certificates and endpoint are correct. Check your AWS IoT policy.
- **WiFi Provisioning:** If AP does not appear, reset the device or erase EEPROM.
- **Button Wake-Up:** Ensure button is wired to `BUTTON_PIN` and ground, and is active LOW.
- **Serial Monitor:** Use 115200 baud rate for best results.

## Step-by-Step Setup Guide

### 1. Hardware Assembly
- Connect potentiometer to ESP32 analog pin (default: GPIO 34).
- Connect LED (valve simulation) to digital pin (default: GPIO 2) with a current-limiting resistor.
- Connect push button to GPIO 0 and ground (active LOW).
- Ensure ESP32 is powered via USB or battery.

### 2. Software Preparation
- Install Arduino IDE (latest version recommended).
- Install ESP32 board support via Boards Manager.
- Install required libraries:
  - WiFi
  - HTTPClient
  - WiFiClientSecure
  - PubSubClient
  - EEPROM
  - WebServer
  - ArduinoOTA
- Clone or download this repository.
- Open `smart_irrigation_node.ino` in Arduino IDE.

### 3. AWS IoT Core Setup
- Create a new AWS IoT Thing and download device certificates.
- Attach an IoT policy to allow publish/subscribe and HTTP POST.
- Fill in your AWS endpoint and certificates in the `.ino` file:
  ```cpp
  #define AWS_MQTT_HOST "your-aws-iot-host.amazonaws.com"
  #define AWS_ENDPOINT "https://your-mock-endpoint.amazonaws.com/irrigation"
  const char* aws_root_ca = "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----";
  const char* aws_cert = "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----";
  const char* aws_private_key = "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----";
  ```

### 4. Initial Flash & WiFi Provisioning
- Upload the firmware to ESP32 via USB.
- Open Serial Monitor at 115200 baud.
- On first boot, ESP32 starts as an AP (`IrrigationNode_AP`).
- Connect to the AP from your computer/phone and visit `http://192.168.4.1/`.
- Enter your WiFi SSID and password, then click Save.
- Device will reboot and connect to your WiFi.

### 5. Normal Operation
- Device reads soil moisture, controls valve, sends data to AWS, and sleeps for 1 minute.
- Wake device by pressing the button.
- Hold button for 5 seconds after wake-up to enter OTA mode.
- Upload new firmware via ArduinoOTA if needed.

## Example Code Snippets

### Reading Soil Moisture
```cpp
SoilMoistureSensor sensor(SOIL_PIN);
sensor.begin();
int moisture = sensor.readPercent();
Serial.println(moisture);
```

### Controlling Valve
```cpp
ValveActuator valve(VALVE_PIN);
valve.begin();
if (moisture < threshold) {
  valve.on();
} else {
  valve.off();
}
```

### Sending Data to AWS IoT Core
```cpp
CloudClient::send(moisture, threshold); // Automatically chooses HTTP or MQTT
```

### Button Handling for OTA Mode
```cpp
Button button(BUTTON_PIN);
button.begin();
if (button.isPressed() && button.waitForLongPress(5000)) {
  // Enter OTA mode
}
```

## Deeper Technical Background

### Power Management
- ESP32 uses deep sleep to minimize power consumption between cycles.
- Wake sources: RTC timer (every 1 minute) or external button (GPIO 0, active LOW).
- Before sleep, all peripherals are de-initialized to reduce leakage current.

### EEPROM Usage
- Stores soil moisture threshold and WiFi credentials for persistent configuration.
- Credentials are written as fixed-length char arrays for reliability.

### WiFi Provisioning
- On first boot or after EEPROM erase, device starts as AP and serves a web page for credentials.
- Credentials are saved to EEPROM and used for subsequent connections.

### Cloud Communication
- **HTTP:** Sends JSON payload to AWS endpoint using HTTP POST.
- **MQTT:** Publishes JSON payload to AWS IoT Core topic using device certificates for TLS.
- Communication mode can be switched via Serial input on boot.

### OTA Updates
- Uses ArduinoOTA library for secure, wireless firmware updates.
- OTA mode is triggered by a long button press and lasts 5 minutes (or until short press).
- Device advertises itself with hostname `SmartIrrigationNode`.

### Button Event Logic
- Button class encapsulates all event detection (short/long press).
- Long press (>5s) triggers OTA mode; short press during OTA exits early.
- All logic is debounced and time-based for reliability.

### LoRaWAN Simulation
- LoRaWAN payload is formatted and printed to Serial, including CRC for integrity.
- This is for demonstration and can be replaced with actual LoRaWAN transmission if needed.

## Additional Resources
- [ESP32 Deep Sleep Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html)
- [AWS IoT Core Getting Started](https://docs.aws.amazon.com/iot/latest/developerguide/iot-gs.html)
- [ArduinoOTA Library](https://github.com/arduino-libraries/ArduinoOTA)

## License
MIT License

## Author
Viral Mandaliya

---
For questions or improvements, open an issue or contact the author.
