#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ==================================================
// OLED SETTINGS
// ==================================================
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define SDA_PIN 21
#define SCL_PIN 22

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;
unsigned long lastDisplayUpdate = 0;

// ==================================================
// WI-FI SETTINGS
// ==================================================
const char* AP_NAME = "MINI-CUBESAT";
const char* AP_PASSWORD = "CubeSat123";
WebServer server(80);

// ==================================================
// BLE SETTINGS
// ==================================================
#define BLE_SERVICE_UUID "7b6d0001-7a8c-4e1b-9d3a-123456789abc"
#define BLE_TELEMETRY_UUID "7b6d0002-7a8c-4e1b-9d3a-123456789abc"

BLEServer* bleServer = nullptr;
BLECharacteristic* telemetryCharacteristic = nullptr;
volatile bool bleConnected = false;

// ==================================================
// BME/BMP280 SENSOR
// ==================================================
Adafruit_BME280 bme;
Adafruit_BMP280 bmp;

bool sensorReady = false;
bool isBME280 = false;
const char* sensorType = "NOT_CONNECTED";

float temperatureC = NAN;
float pressureHpa = NAN;
float humidityPct = NAN;

// ==================================================
// MPU6050 IMU
// ==================================================
Adafruit_MPU6050 mpu;
bool mpuReady = false;

float accelX = NAN, accelY = NAN, accelZ = NAN;
float gyroX = NAN, gyroY = NAN, gyroZ = NAN; // degrees/second


 // Estimated CubeSat orientation (degrees)
float pitch = 0.0f;
float roll = 0.0f;
float yaw = 0.0f;

// Reference orientation for the Zero orientation feature
float pitchOffset = 0.0f;
float rollOffset = 0.0f;
float yawOffset = 0.0f;

// Timing for orientation updates
unsigned long lastOrientationUpdate = 0;
unsigned long lastImuUpdate = 0;
bool orientationInitialized = false;


// ==================================================
// TELEMETRY DATA
// ==================================================
uint32_t packetSequence = 0;
unsigned long lastTelemetryUpdate = 0;
String telemetryJson = "{}";

// ==================================================
// BLE CONNECTION CALLBACKS
// ==================================================
class CubeSatBLECallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* server) override {
        bleConnected = true;
        Serial.println("BLE client connected");
    }

    void onDisconnect(BLEServer* server) override {
        bleConnected = false;
        Serial.println("BLE client disconnected");
        BLEDevice::startAdvertising();
    }
};

// ==================================================
// INITIALIZE BLUETOOTH
// ==================================================
void setupBluetooth() {
    BLEDevice::init("MINI-CUBESAT-BLE");
    BLEDevice::setMTU(200);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new CubeSatBLECallbacks());

    BLEService* service = bleServer->createService(BLE_SERVICE_UUID);

    telemetryCharacteristic = service->createCharacteristic(
        BLE_TELEMETRY_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_NOTIFY
    );

    telemetryCharacteristic->addDescriptor(new BLE2902());
    telemetryCharacteristic->setValue(
        "{\"device\":\"MINI-CUBESAT\",\"status\":\"STARTING\"}"
    );

    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("BLE initialized");
    Serial.println("BLE name: MINI-CUBESAT-BLE");
}

// ==================================================
// INITIALIZE BME/BMP280
// ==================================================
void setupSensor() {
    Serial.println("Initializing BME/BMP280...");

    const uint8_t addresses[] = {0x76, 0x77};

    for (uint8_t address : addresses) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() != 0) {
            continue;
        }

        Wire.beginTransmission(address);
        Wire.write(0xD0); // Chip ID register

        if (Wire.endTransmission(false) != 0) {
            continue;
        }

        if (Wire.requestFrom(address, (uint8_t)1) != 1) {
            continue;
        }

        uint8_t chipID = Wire.read();
        Serial.printf("Sensor address: 0x%02X, chip ID: 0x%02X\n",
                      address, chipID);

        if (chipID == 0x60) {
            isBME280 = true;
            sensorReady = bme.begin(address);
            sensorType = "BME280";
        } else if (chipID == 0x58 || chipID == 0x56 || chipID == 0x57) {
            isBME280 = false;
            sensorReady = bmp.begin(address, chipID);
            sensorType = "BMP280";
        }

        if (sensorReady) {
            Serial.printf("%s initialized successfully\n", sensorType);
            return;
        }
    }

    sensorReady = false;
    sensorType = "NOT_CONNECTED";
    Serial.println("BME/BMP280 not detected. Check wiring and I2C address.");
}

void readSensor() {
    if (!sensorReady) {
        temperatureC = NAN;
        pressureHpa = NAN;
        humidityPct = NAN;
        return;
    }

    if (isBME280) {
        temperatureC = bme.readTemperature();
        pressureHpa = bme.readPressure() / 100.0f;
        humidityPct = bme.readHumidity();
    } else {
        temperatureC = bmp.readTemperature();
        pressureHpa = bmp.readPressure() / 100.0f;
        humidityPct = NAN;
    }
}

// ==================================================
// INITIALIZE AND READ MPU6050
// ==================================================
void setupMPU6050() {
    Serial.println("Initializing MPU6050...");

    mpuReady = mpu.begin(0x68, &Wire);
    if (!mpuReady) {
        mpuReady = mpu.begin(0x69, &Wire);
    }

    if (!mpuReady) {
        Serial.println("MPU6050 not detected. Check wiring and I2C address.");
        return;
    }

    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 initialized successfully");
}

void readMPU6050() {
    if (!mpuReady) {
        accelX = accelY = accelZ = NAN;
        gyroX = gyroY = gyroZ = NAN;
        return;
    }

    sensors_event_t accelEvent, gyroEvent, tempEvent;
    if (!mpu.getEvent(&accelEvent, &gyroEvent, &tempEvent)) {
        accelX = accelY = accelZ = NAN;
        gyroX = gyroY = gyroZ = NAN;
        return;
    }

    const float radiansToDegrees = 57.2957795f;
    accelX = accelEvent.acceleration.x;
    accelY = accelEvent.acceleration.y;
    accelZ = accelEvent.acceleration.z;
    gyroX = gyroEvent.gyro.x * radiansToDegrees;
    gyroY = gyroEvent.gyro.y * radiansToDegrees;
    gyroZ = gyroEvent.gyro.z * radiansToDegrees;
}


void updateOrientation() {
    if (!mpuReady ||
        !isfinite(accelX) ||
        !isfinite(accelY) ||
        !isfinite(accelZ) ||
        !isfinite(gyroX) ||
        !isfinite(gyroY) ||
        !isfinite(gyroZ)) {
        return;
    }

    unsigned long now = millis();

    // Estimate pitch and roll from gravity.
    const float radiansToDegrees = 57.2957795f;

    float accelRoll =
        atan2f(accelY, accelZ) * radiansToDegrees;

    float accelPitch =
        atan2f(-accelX, sqrtf(accelY * accelY +
                              accelZ * accelZ)) * radiansToDegrees;

    // Initialize orientation from the current stationary pose.
    if (!orientationInitialized) {
        roll = accelRoll;
        pitch = accelPitch;
        yaw = 0.0f;

        lastOrientationUpdate = now;
        orientationInitialized = true;
        return;
    }

    float dt = (now - lastOrientationUpdate) / 1000.0f;
    lastOrientationUpdate = now;

    // Ignore unusually long intervals.
    if (dt <= 0.0f || dt > 0.5f) {
        return;
    }

    // Complementary filter: gyroscope motion + accelerometer correction.
    const float alpha = 0.98f;

    roll = alpha * (roll + gyroX * dt)
         + (1.0f - alpha) * accelRoll;

    pitch = alpha * (pitch + gyroY * dt)
          + (1.0f - alpha) * accelPitch;

    // Yaw is integrated from the gyroscope and will drift over time.
    yaw += gyroZ * dt;
}

void zeroOrientation() {
    if (!orientationInitialized) {
        return;
    }

    pitchOffset = pitch;
    rollOffset = roll;
    yawOffset = yaw;
}


// ==================================================
// BUILD TELEMETRY JSON AND PUBLISH VIA BLE
// ==================================================
void updateTelemetry() {
    packetSequence++;
    readSensor();

    uint32_t uptimeSeconds = millis() / 1000;
    uint8_t connectedClients = WiFi.softAPgetStationNum();

    telemetryJson = "{";
    telemetryJson += "\"device\":\"MINI-CUBESAT\",";
    telemetryJson += "\"version\":\"0.3\",";
    telemetryJson += "\"sequence\":" + String(packetSequence) + ",";
    telemetryJson += "\"uptime_s\":" + String(uptimeSeconds) + ",";
    telemetryJson += "\"wifi_clients\":" + String(connectedClients) + ",";
    telemetryJson += "\"ble_connected\":";
    telemetryJson += bleConnected ? "true," : "false,";

    telemetryJson += "\"sensor_type\":\"";
    telemetryJson += sensorType;
    telemetryJson += "\",";

    telemetryJson += "\"temperature_c\":";
    telemetryJson += (sensorReady && isfinite(temperatureC))
                     ? String(temperatureC, 2) : "null";
    telemetryJson += ",";

    telemetryJson += "\"pressure_hpa\":";
    telemetryJson += (sensorReady && isfinite(pressureHpa))
                     ? String(pressureHpa, 2) : "null";
    telemetryJson += ",";

    telemetryJson += "\"humidity_pct\":";
    telemetryJson += (sensorReady && isBME280 && isfinite(humidityPct))
                     ? String(humidityPct, 2) : "null";
    telemetryJson += ",";

    telemetryJson += "\"accel_x_ms2\":";
    telemetryJson += (mpuReady && isfinite(accelX)) ? String(accelX, 3) : "null";
    telemetryJson += ",";
    telemetryJson += "\"accel_y_ms2\":";
    telemetryJson += (mpuReady && isfinite(accelY)) ? String(accelY, 3) : "null";
    telemetryJson += ",";
    telemetryJson += "\"accel_z_ms2\":";
    telemetryJson += (mpuReady && isfinite(accelZ)) ? String(accelZ, 3) : "null";
    telemetryJson += ",";
    telemetryJson += "\"gyro_x_dps\":";
    telemetryJson += (mpuReady && isfinite(gyroX)) ? String(gyroX, 2) : "null";
    telemetryJson += ",";
    telemetryJson += "\"gyro_y_dps\":";
    telemetryJson += (mpuReady && isfinite(gyroY)) ? String(gyroY, 2) : "null";
    telemetryJson += ",";
    telemetryJson += "\"gyro_z_dps\":";
    telemetryJson += (mpuReady && isfinite(gyroZ)) ? String(gyroZ, 2) : "null";
    telemetryJson += ",";

    
    telemetryJson += "\"pitch_deg\":";
    telemetryJson += mpuReady && orientationInitialized
                     ? String(pitch - pitchOffset, 2) : "null";
    telemetryJson += ",";

    telemetryJson += "\"roll_deg\":";
    telemetryJson += mpuReady && orientationInitialized
                     ? String(roll - rollOffset, 2) : "null";
    telemetryJson += ",";

    telemetryJson += "\"yaw_deg\":";
    telemetryJson += mpuReady && orientationInitialized
                     ? String(yaw - yawOffset, 2) : "null";
    telemetryJson += ",";


    telemetryJson += "\"sensors\":{";
    telemetryJson += "\"bme280\":";
    telemetryJson += sensorReady ? "true," : "false,";
    telemetryJson += "\"imu\":";
    telemetryJson += mpuReady ? "true" : "false";
    telemetryJson += "}";

    telemetryJson += "}";

    if (telemetryCharacteristic != nullptr) {
        telemetryCharacteristic->setValue(telemetryJson.c_str());

        if (bleConnected) {
            telemetryCharacteristic->notify();
            Serial.println("BLE telemetry notification sent");
        }
    }

    Serial.println(telemetryJson);
    lastTelemetryUpdate = millis();
}

// ==================================================
// WEB DASHBOARD
// ==================================================

const char dashboard[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#f5f8ff">
<title>Mini CubeSat | Telemetry</title>

<style>

:root[data-theme="dark"] {
  --bg: #0b1220;
  --surface: #151f31;
  --surface-soft: #1c2a40;
  --blue: #78a9ff;
  --blue-dark: #2563eb;
  --blue-light: #233b61;
  --text: #edf4ff;
  --muted: #a5b5ce;
  --border: #2b3b53;
  --green: #55d6a0;
  --green-bg: #16382f;
  --red: #ff8f86;
  --red-bg: #422624;
  --shadow: 0 4px 18px rgba(0, 0, 0, .16);
}

:root[data-theme="dark"] .hero {
  background: linear-gradient(115deg, #102b59, #1746a2 68%, #2563a8);
}

:root[data-theme="dark"] .hero::after {
  border-color: rgba(255,255,255,.12);
}

:root[data-theme="dark"] .hero-chip {
  border-color: rgba(255,255,255,.18);
}

:root[data-theme="dark"] .axis {
  border-color: var(--border);
}


* { box-sizing: border-box; }

body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: Inter, -apple-system, BlinkMacSystemFont, "Segoe UI",
               Arial, sans-serif;
  font-size: 14px;
  line-height: 1.5;
}

main {
  width: min(1120px, 100%);
  margin: 0 auto;
  padding: 28px 24px;
}

.topbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 16px;
  flex-wrap: wrap;
  margin-bottom: 26px;
}

.brand {
  display: flex;
  align-items: center;
  gap: 13px;
}

.logo {
  width: 48px;
  height: 48px;
  display: grid;
  place-items: center;
  flex-shrink: 0;
  background: linear-gradient(145deg, #3478f6, #1746a2);
  color: white;
  border-radius: 14px;
  font-size: 23px;
  font-weight: 800;
  box-shadow: 0 5px 14px rgba(37, 99, 235, .2);
}

.eyebrow {
  color: var(--blue);
  font-size: 10px;
  font-weight: 800;
  letter-spacing: 1.5px;
  text-transform: uppercase;
}

h1 {
  margin: 1px 0 2px;
  font-size: clamp(23px, 4vw, 30px);
  letter-spacing: -.8px;
  line-height: 1.2;
}

.subtitle {
  color: var(--muted);
  font-size: 13px;
}

.status {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 9px 13px;
  background: var(--green-bg);
  color: var(--green);
  border: 1px solid #cceedd;
  border-radius: 30px;
  font-size: 11px;
  font-weight: 800;
  letter-spacing: .4px;
}

.status::before {
  content: "";
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: currentColor;
}

.status.offline {
  color: var(--red);
  background: var(--red-bg);
  border-color: #ffd6d1;
}

.hero {
  position: relative;
  overflow: hidden;
  padding: 25px;
  border-radius: 19px;
  color: white;
  background: linear-gradient(115deg, #1746a2, #2563eb 68%, #5797ff);
  box-shadow: 0 9px 24px rgba(37, 99, 235, .13);
}

.hero::after {
  content: "";
  position: absolute;
  width: 210px;
  height: 210px;
  right: -65px;
  top: -110px;
  border: 1px solid rgba(255,255,255,.2);
  border-radius: 50%;
  box-shadow: 0 0 0 22px rgba(255,255,255,.05),
              0 0 0 46px rgba(255,255,255,.04);
  pointer-events: none;
}

.hero-label {
  color: #dbeafe;
  font-size: 10px;
  font-weight: 800;
  letter-spacing: 1.6px;
}

.hero h2 {
  margin: 7px 0;
  font-size: clamp(21px, 4vw, 29px);
  letter-spacing: -.5px;
}

.hero p {
  margin: 0;
  max-width: 550px;
  color: #e0ebff;
  font-size: 13px;
}

.hero-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 9px;
  margin-top: 20px;
}

.hero-chip {
  padding: 6px 10px;
  border: 1px solid rgba(255,255,255,.23);
  border-radius: 8px;
  background: rgba(255,255,255,.1);
  color: #fff;
  font-size: 11px;
}

.section-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 8px;
  margin: 28px 0 13px;
}

.section-head h2 {
  margin: 0;
  font-size: 16px;
  letter-spacing: -.2px;
}

.section-note {
  color: var(--muted);
  font-size: 11px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 14px;
}

.card {
  min-width: 0;
  padding: 18px;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 15px;
  box-shadow: var(--shadow);
}

.card-top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.label {
  color: var(--muted);
  font-size: 10px;
  font-weight: 800;
  letter-spacing: .8px;
  text-transform: uppercase;
}

.card-icon {
  display: grid;
  place-items: center;
  width: 33px;
  height: 33px;
  flex-shrink: 0;
  color: var(--blue);
  background: var(--surface-soft);
  border-radius: 10px;
  font-size: 16px;
  font-weight: 700;
}

.value {
  margin: 16px 0 5px;
  font-size: clamp(21px, 2.5vw, 28px);
  line-height: 1.2;
  font-weight: 750;
  letter-spacing: -.6px;
  overflow-wrap: anywhere;
  font-variant-numeric: tabular-nums;
}

.note {
  min-height: 18px;
  color: var(--muted);
  font-size: 11px;
}

.small-value {
  font-size: 17px;
  letter-spacing: -.2px;
}

.readings {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
  margin-top: 16px;
}

.axis {
  min-width: 0;
  padding: 12px 8px;
  background: var(--surface-soft);
  border: 1px solid #e5edfb;
  border-radius: 10px;
  text-align: center;
}

.axis-name {
  color: var(--muted);
  font-size: 10px;
  font-weight: 800;
  margin-bottom: 5px;
}

.axis-value {
  font-size: 15px;
  font-weight: 750;
  font-variant-numeric: tabular-nums;
  overflow-wrap: anywhere;
}


.chart-wrap {
  position: relative;
  width: 100%;
  min-width: 0;
  margin-top: 12px;
  overflow: hidden;
  border-radius: 8px;
}

.chart-wrap canvas {
  display: block;
  width: 100%;
  height: 160px;
}

.chart-note {
  color: var(--muted);
  font-size: 11px;
  margin-top: 6px;
}

:root[data-theme="dark"] .chart-wrap {
  background: var(--surface);
}

.health-list {
  display: grid;
  gap: 0;
}

.health-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 13px 0;
  border-bottom: 1px solid var(--border);
}

.health-row:last-child {
  border-bottom: 0;
  padding-bottom: 0;
}

.health-row:first-child {
  padding-top: 3px;
}

.health-name {
  font-size: 12px;
  font-weight: 650;
}

.health-detail {
  color: var(--muted);
  font-size: 11px;
  margin-top: 2px;
}

.health-state {
  flex-shrink: 0;
  padding: 5px 8px;
  border-radius: 7px;
  background: var(--surface-soft);
  color: var(--muted);
  font-size: 10px;
  font-weight: 800;
}

.health-state.good {
  color: var(--green);
  background: var(--green-bg);
}

.health-state.bad {
  color: var(--red);
  background: var(--red-bg);
}

.wide {
  grid-column: span 2;
}

.footer {
  display: flex;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 10px;
  margin-top: 24px;
  padding: 16px 2px 4px;
  color: var(--muted);
  font-size: 11px;
}

#updated {
  font-variant-numeric: tabular-nums;
}
 
.header-actions {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.theme-toggle {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 9px 12px;
  border: 1px solid var(--border);
  border-radius: 30px;
  background: var(--surface);
  color: var(--text);
  font: inherit;
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
  transition: background .2s, color .2s, border-color .2s;
}

.theme-toggle:hover {
  border-color: var(--blue);
}

.theme-toggle:focus-visible {
  outline: 2px solid var(--blue);
  outline-offset: 3px;
}

#themeIcon {
  font-size: 16px;
  color: var(--blue);
}



/* Virtual Mission Lab */
.sim-lab {
  margin-top: 18px;
  padding: 18px;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: var(--surface);
  box-shadow: var(--shadow);
}
.sim-lab-head { display:flex; align-items:flex-start; justify-content:space-between; gap:12px; flex-wrap:wrap; }
.sim-lab h2 { margin:0 0 4px; font-size:16px; }
.sim-lab p { margin:0; color:var(--muted); font-size:12px; }
.sim-controls { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; margin-top:15px; }
.sim-field { display:flex; flex-direction:column; gap:6px; color:var(--muted); font-size:11px; font-weight:700; }
.sim-field select { width:100%; min-height:40px; padding:8px 10px; border:1px solid var(--border); border-radius:9px; background:var(--bg); color:var(--text); font:inherit; font-size:13px; }
.sim-notice { margin-top:12px !important; padding:10px 12px; border-radius:9px; background:var(--blue-light); color:var(--text) !important; }
.sim-notice strong { color:var(--blue); }
.simulated-tag { display:none; margin-top:10px; padding:5px 9px; border-radius:7px; background:#fff0c2; color:#854d0e; font-size:10px; font-weight:800; letter-spacing:.5px; }
:root[data-theme="dark"] .simulated-tag { background:#493819; color:#fcd780; }
@media (max-width: 480px) { .sim-controls { grid-template-columns:1fr; } .sim-lab { padding:14px; } }

@media (max-width: 850px) {
  .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}

@media (max-width: 480px) {
  main { padding: 18px 13px; }
  .hero { padding: 20px; }
  .grid { gap: 10px; }
  .card { padding: 14px; }
  .wide { grid-column: span 2; }
  .value { font-size: 22px; }
  .readings { gap: 6px; }
  .axis { padding: 10px 4px; }
  .axis-value { font-size: 13px; }
  .topbar { margin-bottom: 19px; }
}
/* Live 3D CubeSat orientation model */
#orientationCard { overflow: hidden; }
#cubesatModel {
  position: relative;
  width: 100%;
  height: 320px;
  display: flex;
  align-items: center;
  justify-content: center;
  perspective: 900px;
  overflow: hidden;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: radial-gradient(ellipse at center, rgba(59,130,246,.14), transparent 65%);
  margin: 18px 0;
}
.sat-orbit {
  position: absolute;
  width: 230px;
  height: 230px;
  border: 1px solid rgba(120,169,255,.22);
  border-radius: 50%;
  transform: rotateX(68deg) rotateZ(-18deg);
  pointer-events: none;
}
.sat-orbit.orbit-two { width: 275px; height: 275px; transform: rotateY(62deg) rotateZ(22deg); }
.satellite-assembly {
  position: relative;
  width: 230px;
  height: 150px;
  transform-style: preserve-3d;
  transform: rotateX(-18deg) rotateY(-28deg) rotateZ(8deg);
  transition: transform 180ms ease-out;
  will-change: transform;
  z-index: 1;
}
.sat-panel {
  position: absolute;
  top: 43px;
  width: 72px;
  height: 64px;
  border: 3px solid #8bbcff;
  border-radius: 3px;
  background-color: #123b78;
  background-image: linear-gradient(90deg, transparent 46%, #8bbcff 47%, #8bbcff 51%, transparent 52%), linear-gradient(0deg, transparent 46%, #8bbcff 47%, #8bbcff 51%, transparent 52%);
  box-shadow: inset 0 0 16px rgba(72,151,255,.28), 0 0 18px rgba(59,130,246,.16);
  transform: translateZ(0);
}
.sat-panel.left { left: 0; transform: translateZ(2px) rotateY(-8deg); }
.sat-panel.right { right: 0; transform: translateZ(2px) rotateY(8deg); }
.sat-body {
  position: absolute;
  width: 76px;
  height: 82px;
  left: 77px;
  top: 34px;
  border: 1px solid #f5d9a1;
  border-radius: 5px;
  background: linear-gradient(135deg, #f3e3c5 0%, #b9a27d 38%, #e8d4ad 65%, #8c795e 100%);
  box-shadow: inset -9px -5px 0 rgba(55,65,81,.20), 0 8px 24px rgba(0,0,0,.2);
  transform: translateZ(15px);
}
.sat-body::before {
  content: "";
  position: absolute;
  inset: 9px 12px;
  border: 2px solid rgba(92,78,56,.55);
  border-radius: 2px;
  background: linear-gradient(135deg, #e4edf7, #7187a2 48%, #d4e0ed);
}
.sat-body::after {
  content: "";
  position: absolute;
  width: 16px;
  height: 16px;
  border-radius: 50%;
  left: 29px;
  top: 32px;
  border: 2px solid #657b91;
  background: radial-gradient(circle at 35% 30%, #d8f1ff, #24517e 68%);
}
.sat-top {
  position: absolute;
  width: 76px;
  height: 22px;
  left: 77px;
  top: 23px;
  background: linear-gradient(90deg, #c4b18e, #f5e7c9, #a9987b);
  border: 1px solid #e7d5b3;
  transform: rotateX(65deg) translateZ(28px);
}
.sat-antenna {
  position: absolute;
  width: 3px;
  height: 34px;
  left: 113px;
  top: 1px;
  background: #cbd5e1;
  transform: translateZ(12px) rotateZ(8deg);
}
.sat-antenna::after {
  content: "";
  position: absolute;
  width: 13px;
  height: 13px;
  left: -5px;
  top: -8px;
  border: 2px solid #cbd5e1;
  border-radius: 50%;
  background: #6b8baa;
}
.orientation-readings {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
  margin: 14px 0;
}
.orientation-reading {
  padding: 12px 14px;
  border: 1px solid var(--border);
  border-radius: 12px;
  background: var(--surface-soft);
}
.orientation-reading strong { display: block; color: var(--muted); font-size: 12px; }
.orientation-reading p { margin: 5px 0 0; font-size: 22px; font-weight: 700; font-variant-numeric: tabular-nums; }
@media (max-width: 520px) {
  #cubesatModel { height: 270px; }
  .satellite-assembly { transform: scale(.82) rotateX(-18deg) rotateY(-28deg) rotateZ(8deg); }
  .orientation-readings { gap: 7px; }
  .orientation-reading { padding: 10px 8px; }
  .orientation-reading p { font-size: 18px; }
}

</style>
</head>

<body>
<main>
  <header class="topbar">
    <div class="brand">
      <div class="logo">C</div>
      <div>
        <div class="eyebrow">Mini CubeSat / V0.3</div>
        <h1>Telemetry Station</h1>
        <div class="subtitle">Live spacecraft systems monitor</div>
      </div>
    </div>
    <div class="header-actions">
  <button class="theme-toggle" id="themeToggle"
          type="button" aria-label="Switch to dark mode"
          aria-pressed="false">
    <span id="themeIcon" aria-hidden="true">&#9790;</span>
    <span id="themeLabel">Dark mode</span>
  </button>
  <div class="status" id="status" role="status">CONNECTING</div>
</div>
  </header>

  <section class="hero">
    <div class="hero-label">MISSION CONTROL / PROTOTYPE</div>
    <h2>CubeSat Telemetry</h2>
    <p>
      Monitor environmental conditions, motion sensors and communication
      links from your ESP32-based CubeSat prototype.
    </p>
    <div class="hero-meta">
      <span class="hero-chip">Controller: ESP32</span>
      <span class="hero-chip">Wi-Fi: HTTP</span>
      <span class="hero-chip">Radio link: BLE</span>
    </div>
  </section>

  <section class="sim-lab" aria-labelledby="missionLabTitle">
    <div class="sim-lab-head">
      <div>
        <div class="eyebrow">TEST ENVIRONMENT</div>
        <h2 id="missionLabTitle">Virtual Mission Lab</h2>
        <p>Explore telemetry scenarios in this browser without changing the ESP32, Wi-Fi, or BLE links.</p>
      </div>
      <div class="simulated-tag" id="simulatedTag">SIMULATED DATA</div>
    </div>
    <div class="sim-controls">
      <label class="sim-field" for="telemetryMode">Telemetry source
        <select id="telemetryMode">
          <option value="live">Live hardware</option>
          <option value="simulation">Virtual simulation</option>
        </select>
      </label>
      <label class="sim-field" for="simulationScenario">Mission scenario
        <select id="simulationScenario" disabled>
          <option value="nominal">Nominal operation</option>
          <option value="environment">Environmental changes</option>
          <option value="tumbling">Attitude / tumbling</option>
          <option value="anomaly">Sensor anomaly</option>
          <option value="interruption">Simulated communication interruption</option>
        </select>
      </label>
    </div>
    <p class="sim-notice" id="simulationNotice"><strong>LIVE:</strong> Displaying readings received from your ESP32. Simulation is off.</p>
  </section>

  <div class="section-head">
    <h2>Mission overview</h2>
    <span class="section-note">Updates every second</span>
  </div>

  <section class="grid">
    <article class="card">
      <div class="card-top">
        <span class="label">System uptime</span>
        <span class="card-icon">&#9201;</span>
      </div>
      <div class="value" id="uptime">--</div>
      <div class="note">Since last restart</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Packet sequence</span>
        <span class="card-icon">#</span>
      </div>
      <div class="value" id="sequence">--</div>
      <div class="note">Telemetry updates generated</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Wi-Fi clients</span>
        <span class="card-icon">&#9673;</span>
      </div>
      <div class="value" id="clients">--</div>
      <div class="note">Connected to ESP32 access point</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Bluetooth LE</span>
        <span class="card-icon">&#8644;</span>
      </div>
      <div class="value small-value" id="ble">--</div>
      <div class="note">BLE client connection</div>
    </article>
  </section>

  <div class="section-head">
    <h2>Environmental telemetry</h2>
    <span class="section-note" id="sensorType">Detecting sensor...</span>
  </div>

  <section class="grid">
    <article class="card">
      <div class="card-top">
        <span class="label">Temperature</span>
        <span class="card-icon">&#176;C</span>
      </div>
      <div class="value" id="temperature">--</div>
      <div class="note">Ambient temperature</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Pressure</span>
        <span class="card-icon">P</span>
      </div>
      <div class="value" id="pressure">--</div>
      <div class="note">Atmospheric pressure</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Humidity</span>
        <span class="card-icon">H</span>
      </div>
      <div class="value" id="humidity">--</div>
      <div class="note">BME280 only</div>
    </article>

    <article class="card">
      <div class="card-top">
        <span class="label">Sensor health</span>
        <span class="card-icon">&#10003;</span>
      </div>
      <div class="value small-value" id="envStatus">CHECKING</div>
      <div class="note">Environmental sensor availability</div>
    </article>
  </section>

  
  <div class="section-head">
    <h2>Environmental trends</h2>
    <span class="section-note">Live history · Last 60 samples</span>
  </div>

  <section class="grid">
    <article class="card wide">
      <div class="card-top">
        <span class="label">Temperature trend</span>
        <span class="card-icon">°C</span>
      </div>
      <div class="note">Temperature · °C</div>
      <canvas id="temperatureChart" height="160"
              style="display:block;width:100%;height:160px"
              role="img" aria-label="Live temperature history chart"></canvas>
    </article>

    <article class="card wide">
      <div class="card-top">
        <span class="label">Pressure trend</span>
        <span class="card-icon">P</span>
      </div>
      <div class="note">Atmospheric pressure · hPa</div>
      <canvas id="pressureChart" height="160"
              style="display:block;width:100%;height:160px"
              role="img" aria-label="Live pressure history chart"></canvas>
    </article>

    <article class="card wide" id="humidityChartCard">
      <div class="card-top">
        <span class="label">Humidity trend</span>
        <span class="card-icon">H</span>
      </div>
      <div class="note">Relative humidity · %</div>
      <canvas id="humidityChart" height="160"
              style="display:block;width:100%;height:160px"
              role="img" aria-label="Live humidity history chart"></canvas>
    </article>
  </section>

<div class="section-head">
    <h2>Motion telemetry</h2>
    <span class="section-note">MPU6050 · 6-axis IMU</span>
  </div>

  <section class="grid">
    <article class="card wide">
      <div class="card-top">
        <span class="label">Acceleration</span>
        <span class="card-icon">A</span>
      </div>
      <div class="note">Linear acceleration readings · m/s²</div>
      <div class="readings">
        <div class="axis">
          <div class="axis-name">X AXIS</div>
          <div class="axis-value" id="accelX">--</div>
        </div>
        <div class="axis">
          <div class="axis-name">Y AXIS</div>
          <div class="axis-value" id="accelY">--</div>
        </div>
        <div class="axis">
          <div class="axis-name">Z AXIS</div>
          <div class="axis-value" id="accelZ">--</div>
        </div>
      </div>
    </article>

    <article class="card wide">
      <div class="card-top">
        <span class="label">Angular velocity</span>
        <span class="card-icon">&#8635;</span>
      </div>
      <div class="note">Gyroscope readings · degrees per second</div>
      <div class="readings">
        <div class="axis">
          <div class="axis-name">X AXIS</div>
          <div class="axis-value" id="gyroX">--</div>
        </div>
        <div class="axis">
          <div class="axis-name">Y AXIS</div>
          <div class="axis-value" id="gyroY">--</div>
        </div>
        <div class="axis">
          <div class="axis-name">Z AXIS</div>
          <div class="axis-value" id="gyroZ">--</div>
        </div>
      </div>
    </article>
  </section>
  
<div class="section-head">
  <h2>Motion trends</h2>
  <span class="section-note">MPU6050 · Last 60 samples</span>
</div>

<section class="grid">
  <article class="card wide">
    <div class="card-top">
      <span class="label">Acceleration · X / Y / Z</span>
      <span class="card-icon">A</span>
    </div>
    <div class="note">Linear acceleration · m/s²</div>
    <canvas id="accelChart" height="180"
            style="display:block;width:100%;height:180px"
            role="img"
            aria-label="Live three-axis acceleration chart"></canvas>
  </article>

  <article class="card wide">
    <div class="card-top">
      <span class="label">Angular velocity · X / Y / Z</span>
      <span class="card-icon">G</span>
    </div>
    <div class="note">Gyroscope · degrees per second</div>
    <canvas id="gyroChart" height="180"
            style="display:block;width:100%;height:180px"
            role="img"
            aria-label="Live three-axis gyroscope chart"></canvas>
  </article>
</section>

  <div class="section-head">
    <h2>System health</h2>
    <span class="section-note">Live connection diagnostics</span>
  </div>

  <section class="grid">
    <article class="card wide">
      <div class="health-list">
        <div class="health-row">
          <div>
            <div class="health-name">ESP32 dashboard</div>
            <div class="health-detail">HTTP telemetry endpoint</div>
          </div>
          <span class="health-state" id="httpHealth">CHECKING</span>
        </div>
        <div class="health-row">
          <div>
            <div class="health-name">Environmental sensor</div>
            <div class="health-detail">BME280 / BMP280</div>
          </div>
          <span class="health-state" id="envHealth">CHECKING</span>
        </div>
        <div class="health-row">
          <div>
            <div class="health-name">Motion sensor</div>
            <div class="health-detail">MPU6050 accelerometer and gyroscope</div>
          </div>
          <span class="health-state" id="imu">CHECKING</span>
        </div>
        <div class="health-row">
          <div>
            <div class="health-name">Bluetooth Low Energy</div>
            <div class="health-detail">Connection to external BLE client</div>
          </div>
          <span class="health-state" id="bleHealth">CHECKING</span>
        </div>
      </div>
    </article>

    <article class="card wide">
      <div class="card-top">
        <span class="label">Telemetry status</span>
        <span class="card-icon">&#10003;</span>
      </div>
      <div class="value small-value" id="telemetryHealth">WAITING</div>
      <p class="note" id="updated">Waiting for telemetry...</p>
      <div class="note" style="margin-top:12px">
        Live values come from the connected hardware. Missing sensor
        readings are displayed as unavailable, not as zero.
      </div>
    </article>
  </section>

<section class="card wide" id="orientationCard">
  <h2>3D CubeSat Orientation</h2>
  <p>Live orientation estimated from MPU6050</p>

  <div id="cubesatModel" aria-label="Animated 3D CubeSat model following estimated orientation">
    <div class="sat-orbit"></div>
    <div class="sat-orbit orbit-two"></div>
    <div class="satellite-assembly" id="satelliteAssembly">
      <div class="sat-panel left"></div>
      <div class="sat-panel right"></div>
      <div class="sat-top"></div>
      <div class="sat-body"></div>
      <div class="sat-antenna"></div>
    </div>
  </div>

  <div class="telemetry-grid">
    <div>
      <strong>Pitch</strong>
      <p id="pitchValue">--°</p>
    </div>
    <div>
      <strong>Roll</strong>
      <p id="rollValue">--°</p>
    </div>
    <div>
      <strong>Yaw</strong>
      <p id="yawValue">--°</p>
    </div>
  </div>

  <button id="zeroOrientationBtn" type="button">
    Zero orientation
  </button>

  <p id="orientationMessage">
    Waiting for orientation data...
  </p>
</section>

  <footer class="footer">
    <span>MINI CUBESAT · ESP32 TELEMETRY PROTOTYPE</span>
    <span>Wi-Fi HTTP + Bluetooth LE · <span id="version">V0.3</span></span>
  </footer>
</main>

<script>

const themeToggle = document.getElementById("themeToggle");
const themeIcon = document.getElementById("themeIcon");
const themeLabel = document.getElementById("themeLabel");

function applyTheme(theme) {
  const isDark = theme === "dark";

  document.documentElement.dataset.theme = theme;
  themeIcon.innerHTML = isDark ? "&#9728;" : "&#9790;";
  themeLabel.textContent = isDark ? "Light mode" : "Dark mode";
  themeToggle.setAttribute(
    "aria-label",
    isDark ? "Switch to light mode" : "Switch to dark mode"
  );
  themeToggle.setAttribute("aria-pressed", String(isDark));
}

let savedTheme = "light";

try {
  const storedTheme = localStorage.getItem("cubesat-theme");
  if (storedTheme === "light" || storedTheme === "dark") {
    savedTheme = storedTheme;
  }
} catch (error) {
  // Theme switching still works if browser storage is unavailable.
}

applyTheme(savedTheme);

console.log("Theme toggle:", themeToggle, "Zero button:", document.getElementById("zeroOrientationBtn"));
themeToggle.addEventListener("click", () => {
  const nextTheme =
    document.documentElement.dataset.theme === "dark" ? "light" : "dark";

  applyTheme(nextTheme);

  try {
    localStorage.setItem("cubesat-theme", nextTheme);
  } catch (error) {
    // Keep the current theme for this page session.
  }
});

const $ = id => document.getElementById(id);

function formatTime(seconds) {
  if (seconds === null || seconds === undefined ||
      !Number.isFinite(Number(seconds))) return "--";

  seconds = Math.floor(Number(seconds));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = seconds % 60;
  return hours + "h " + minutes + "m " + secs + "s";
}

function numberOrNA(value, digits) {
  if (value === null || value === undefined || value === "" ||
      !Number.isFinite(Number(value))) return "N/A";
  return Number(value).toFixed(digits);
}

function setHealth(id, text, state) {
  const element = $(id);
  element.textContent = text;
  element.classList.remove("good", "bad");

  if (state === true) element.classList.add("good");
  if (state === false) element.classList.add("bad");
}

function setStatus(online) {
  const element = $("status");
  element.textContent = online ? "ONLINE" : "RECONNECTING";
  element.classList.toggle("offline", !online);
}

function showAxes(prefix, values, digits) {
  const valid = values.every(value =>
    value !== null && value !== undefined &&
    value !== "" && Number.isFinite(Number(value))
  );

  ["X", "Y", "Z"].forEach((axis, index) => {
    $(prefix + axis).textContent = valid
      ? numberOrNA(values[index], digits)
      : "N/A";
  });
}


const CHART_LIMIT = 60;

function makeHistorySet() {
  return {
    chart: { temperature: [], pressure: [], humidity: [] },
    motion: { accelX: [], accelY: [], accelZ: [], gyroX: [], gyroY: [], gyroZ: [] }
  };
}

// Live and synthetic samples are stored separately and never mixed.
const liveHistory = makeHistorySet();
const simulatedHistory = makeHistorySet();
let chartHistory = liveHistory.chart;
let motionHistory = liveHistory.motion;
let telemetryMode = "live";
let simulationTick = 0;
let lastSimulatedData = null;

function addMotionSample(key, value) {
  const number = Number(value);

  motionHistory[key].push(
    value !== null && value !== undefined && value !== "" &&
    Number.isFinite(number)
      ? number
      : null
  );

  if (motionHistory[key].length > CHART_LIMIT) {
    motionHistory[key].shift();
  }
}


function addChartSample(key, value) {
  const number = Number(value);

  chartHistory[key].push(
    value !== null && value !== undefined && value !== "" &&
    Number.isFinite(number)
      ? number
      : null
  );

  if (chartHistory[key].length > CHART_LIMIT) {
    chartHistory[key].shift();
  }
}

function drawTelemetryChart(canvasId, values, unit) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const width = canvas.clientWidth;
  const height = 160;
  if (width <= 0) return;

  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * dpr);
  canvas.height = Math.round(height * dpr);
  canvas.style.height = height + "px";

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);

  const styles = getComputedStyle(document.documentElement);
  const muted = styles.getPropertyValue("--muted").trim() || "#64748b";
  const accent = styles.getPropertyValue("--accent").trim() || "#2563eb";
  const border = styles.getPropertyValue("--border").trim() || "#dbe3ef";

  const pad = { left: 42, right: 12, top: 14, bottom: 24 };
  const plotW = width - pad.left - pad.right;
  const plotH = height - pad.top - pad.bottom;

  if (plotW <= 0 || plotH <= 0) return;

  const valid = values.filter(v => v !== null && Number.isFinite(v));

  ctx.font = "11px system-ui, sans-serif";
  ctx.lineWidth = 1;
  ctx.strokeStyle = border;
  ctx.fillStyle = muted;

  // Show a waiting message until sensor readings arrive.
  if (valid.length === 0) {
    ctx.fillText("Waiting for sensor data...", pad.left, height / 2);
    return;
  }

  let min = Math.min(...valid);
  let max = Math.max(...valid);

  if (min === max) {
    const extra = Math.abs(min) * 0.05 || 1;
    min -= extra;
    max += extra;
  } else {
    const extra = (max - min) * 0.12;
    min -= extra;
    max += extra;
  }

  const x = i => pad.left +
    (values.length <= 1 ? 0 : i / (values.length - 1)) * plotW;

  const y = v => pad.top +
    ((max - v) / (max - min)) * plotH;

  // Horizontal grid and labels.
  for (let i = 0; i <= 4; i++) {
    const gy = pad.top + (plotH * i) / 4;
    const value = max - ((max - min) * i) / 4;

    ctx.beginPath();
    ctx.moveTo(pad.left, gy);
    ctx.lineTo(width - pad.right, gy);
    ctx.stroke();

    ctx.textAlign = "right";
    ctx.fillText(value.toFixed(1), pad.left - 7, gy + 4);
  }

  // Draw separate line segments; missing readings are never treated as zero.
  ctx.strokeStyle = accent;
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.lineCap = "round";

  let drawing = false;
  ctx.beginPath();

  values.forEach((value, i) => {
    if (value === null || !Number.isFinite(value)) {
      drawing = false;
      return;
    }

    if (!drawing) {
      ctx.moveTo(x(i), y(value));
      drawing = true;
    } else {
      ctx.lineTo(x(i), y(value));
    }
  });

  ctx.stroke();

  // Label the time span.
  ctx.fillStyle = muted;
  ctx.textAlign = "left";
  ctx.fillText("Oldest", pad.left, height - 5);
  ctx.textAlign = "right";
  ctx.fillText("Latest", width - pad.right, height - 5);
}


function drawMotionChart(canvasId, series) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const width = canvas.clientWidth;
  const height = 180;
  if (width <= 0) return;

  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * dpr);
  canvas.height = Math.round(height * dpr);
  canvas.style.height = height + "px";

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);

  const styles = getComputedStyle(document.documentElement);
  const muted = styles.getPropertyValue("--muted").trim() || "#64748b";
  const border = styles.getPropertyValue("--border").trim() || "#dbe3ef";

  const colors = ["#2563eb", "#16a34a", "#f59e0b"];
  const pad = { left: 44, right: 12, top: 28, bottom: 24 };
  const plotW = width - pad.left - pad.right;
  const plotH = height - pad.top - pad.bottom;

  const allValues = series.flatMap(s =>
    s.values.filter(v => v !== null && Number.isFinite(v))
  );

  ctx.font = "11px system-ui, sans-serif";
  ctx.fillStyle = muted;

  if (!allValues.length) {
    ctx.fillText("Waiting for motion data...", pad.left, height / 2);
    return;
  }

  let min = Math.min(...allValues);
  let max = Math.max(...allValues);

  if (min === max) {
    const extra = Math.abs(min) * 0.1 || 1;
    min -= extra;
    max += extra;
  } else {
    const extra = (max - min) * 0.12;
    min -= extra;
    max += extra;
  }

  const count = Math.max(...series.map(s => s.values.length));
  const x = i => pad.left +
    (count <= 1 ? 0 : i / (count - 1)) * plotW;
  const y = v => pad.top +
    ((max - v) / (max - min)) * plotH;

  // Grid and scale labels.
  ctx.strokeStyle = border;
  ctx.lineWidth = 1;

  for (let i = 0; i <= 4; i++) {
    const gy = pad.top + (plotH * i) / 4;
    const value = max - ((max - min) * i) / 4;

    ctx.beginPath();
    ctx.moveTo(pad.left, gy);
    ctx.lineTo(width - pad.right, gy);
    ctx.stroke();

    ctx.textAlign = "right";
    ctx.fillStyle = muted;
    ctx.fillText(value.toFixed(1), pad.left - 7, gy + 4);
  }

  // Axis legend.
  series.forEach((s, index) => {
    const legendX = pad.left + index * 48;
    ctx.fillStyle = colors[index];
    ctx.fillRect(legendX, 10, 10, 3);
    ctx.fillStyle = muted;
    ctx.textAlign = "left";
    ctx.fillText(s.label, legendX + 14, 14);
  });

  // Draw each axis, leaving gaps for missing samples.
  series.forEach((s, index) => {
    ctx.beginPath();
    ctx.strokeStyle = colors[index];
    ctx.lineWidth = 1.8;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";

    let drawing = false;

    s.values.forEach((value, i) => {
      if (value === null || !Number.isFinite(value)) {
        drawing = false;
        return;
      }

      if (!drawing) {
        ctx.moveTo(x(i), y(value));
        drawing = true;
      } else {
        ctx.lineTo(x(i), y(value));
      }
    });

    ctx.stroke();
  });

  ctx.fillStyle = muted;
  ctx.textAlign = "left";
  ctx.fillText("Oldest", pad.left, height - 5);
  ctx.textAlign = "right";
  ctx.fillText("Latest", width - pad.right, height - 5);
}


const modeSelect = $("telemetryMode");
const scenarioSelect = $("simulationScenario");
const simulationNotice = $("simulationNotice");
const simulatedTag = $("simulatedTag");

function selectHistoryForMode() {
  const active = telemetryMode === "simulation" ? simulatedHistory : liveHistory;
  chartHistory = active.chart;
  motionHistory = active.motion;
}

function updateSimulationControls() {
  const simulated = telemetryMode === "simulation";
  scenarioSelect.disabled = !simulated;
  simulatedTag.style.display = simulated ? "inline-block" : "none";
  if (!simulated) {
    simulationNotice.innerHTML = "<strong>LIVE:</strong> Displaying readings received from your ESP32. Simulation is off.";
  } else {
    simulationNotice.innerHTML = "<strong>SIMULATION:</strong> Synthetic values only; no sensor, Wi-Fi, or BLE connection is changed.";
  }
}

function makeSimulatedTelemetry() {
  simulationTick += 1;
  const t = simulationTick;
  const scenario = scenarioSelect.value;
  const wave = Math.sin(t / 8);
  const slowWave = Math.sin(t / 23);
  let data = {
    sequence: 1000 + t,
    uptime_s: t,
    wifi_clients: 1,
    ble_connected: true,
    sensor_type: "BME280",
    sensors: { imu: true },
    temperature_c: 24.0 + 0.8 * wave,
    pressure_hpa: 1008.0 + 1.5 * slowWave,
    humidity_pct: 48.0 + 3.0 * Math.sin(t / 13),
    pitch_deg: 5 * Math.sin(t / 12),
    roll_deg: 7 * Math.sin(t / 10),
    yaw_deg: 12 * Math.sin(t / 18),
    accel_x_ms2: 0.4 * Math.sin(t / 5),
    accel_y_ms2: 0.4 * Math.cos(t / 6),
    accel_z_ms2: 9.81 + 0.2 * Math.sin(t / 7),
    gyro_x_dps: 2 * Math.sin(t / 5),
    gyro_y_dps: 2 * Math.cos(t / 6),
    gyro_z_dps: 1.5 * Math.sin(t / 8),
    version: "0.3",
    _simulated: true,
    _commInterrupted: false
  };

  if (scenario === "environment") {
    data.temperature_c = 24 + 8 * Math.sin(t / 20);
    data.pressure_hpa = 1008 + 12 * Math.sin(t / 27);
    data.humidity_pct = 48 + 25 * Math.sin(t / 17);
  } else if (scenario === "tumbling") {
    data.pitch_deg = 75 * Math.sin(t / 5);
    data.roll_deg = 85 * Math.sin(t / 4.3);
    data.yaw_deg = ((t * 13) % 360) - 180;
    data.accel_x_ms2 = 3.8 * Math.sin(t / 3.4);
    data.accel_y_ms2 = 4.2 * Math.cos(t / 3.1);
    data.accel_z_ms2 = 9.81 + 2.4 * Math.sin(t / 2.7);
    data.gyro_x_dps = 65 * Math.sin(t / 3.4);
    data.gyro_y_dps = 72 * Math.cos(t / 3.1);
    data.gyro_z_dps = 90 * Math.sin(t / 2.8);
  } else if (scenario === "anomaly") {
    // Cycle through a frozen value, an implausible reading, and missing data.
    const phase = Math.floor(t / 8) % 3;
    if (phase === 0) {
      data.temperature_c = 99.9;
      data.pressure_hpa = 1008.0;
      data.humidity_pct = 101;
    } else if (phase === 1) {
      data.temperature_c = 24.0; // intentionally frozen synthetic value
      data.pressure_hpa = null;
      data.gyro_y_dps = null;
    } else {
      data.temperature_c = null;
      data.humidity_pct = null;
      data.sensors.imu = false;
      data.accel_x_ms2 = null;
      data.accel_y_ms2 = null;
      data.accel_z_ms2 = null;
      data.gyro_x_dps = null;
      data.gyro_y_dps = null;
      data.gyro_z_dps = null;
    }
  } else if (scenario === "interruption") {
    data._commInterrupted = Math.floor(t / 7) % 2 === 1;
    if (data._commInterrupted) {
      // A simulated stale/missing payload only; no real network request is interrupted.
      data.temperature_c = null;
      data.pressure_hpa = null;
      data.humidity_pct = null;
      data.sensors.imu = false;
      data.accel_x_ms2 = null;
      data.accel_y_ms2 = null;
      data.accel_z_ms2 = null;
      data.gyro_x_dps = null;
      data.gyro_y_dps = null;
      data.gyro_z_dps = null;
    }
  }
  lastSimulatedData = data;
  return data;
}

async function refresh() {
  try {
    let data;
    if (telemetryMode === "simulation") {
      data = makeSimulatedTelemetry();
    } else {
      const response = await fetch("/api", { cache: "no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status);
      data = await response.json();
    }

    
    const pitchValue = $("pitchValue");
    const rollValue = $("rollValue");
    const yawValue = $("yawValue");

    pitchValue.textContent =
      Number.isFinite(data.pitch_deg)
        ? data.pitch_deg.toFixed(1) + "°" : "--°";

    rollValue.textContent =
      Number.isFinite(data.roll_deg)
        ? data.roll_deg.toFixed(1) + "°" : "--°";

    yawValue.textContent =
      Number.isFinite(data.yaw_deg)
        ? data.yaw_deg.toFixed(1) + "°" : "--°";

    const satelliteAssembly = $("satelliteAssembly");
    const pitchDeg = Number(data.pitch_deg);
    const rollDeg = Number(data.roll_deg);
    const yawDeg = Number(data.yaw_deg);
    if (satelliteAssembly &&
        data.pitch_deg !== null && data.pitch_deg !== undefined &&
        data.roll_deg !== null && data.roll_deg !== undefined &&
        data.yaw_deg !== null && data.yaw_deg !== undefined &&
        Number.isFinite(pitchDeg) && Number.isFinite(rollDeg) && Number.isFinite(yawDeg)) {
      satelliteAssembly.style.transform =
        `translateZ(0) rotateX(${-18 + pitchDeg}deg) ` +
        `rotateY(${-28 + yawDeg}deg) ` +
        `rotateZ(${8 + rollDeg}deg)`;
    }

    
    addChartSample("temperature", data.temperature_c);
    addChartSample("pressure", data.pressure_hpa);
    addChartSample("humidity", data.humidity_pct);

    
    addMotionSample("accelX", data.accel_x_ms2);
    addMotionSample("accelY", data.accel_y_ms2);
    addMotionSample("accelZ", data.accel_z_ms2);

    addMotionSample("gyroX", data.gyro_x_dps);
    addMotionSample("gyroY", data.gyro_y_dps);
    addMotionSample("gyroZ", data.gyro_z_dps);


    drawTelemetryChart("temperatureChart", chartHistory.temperature, "°C");
    drawTelemetryChart("pressureChart", chartHistory.pressure, "hPa");
    drawTelemetryChart("humidityChart", chartHistory.humidity, "%");
    
    drawMotionChart("accelChart", [
    { label: "X", values: motionHistory.accelX },
    { label: "Y", values: motionHistory.accelY },
    { label: "Z", values: motionHistory.accelZ }
    ]);

    drawMotionChart("gyroChart", [
    { label: "X", values: motionHistory.gyroX },
    { label: "Y", values: motionHistory.gyroY },
    { label: "Z", values: motionHistory.gyroZ }
    ]);


    if (!data || typeof data !== "object" ||
        data.sequence === undefined ||
        data.uptime_s === undefined) {
      throw new Error("Invalid telemetry payload");
    }

    const simulatedInterruption = telemetryMode === "simulation" && data._commInterrupted;
    setStatus(!simulatedInterruption);

    $("uptime").textContent = formatTime(data.uptime_s);
    $("sequence").textContent = data.sequence;
    $("clients").textContent = data.wifi_clients ?? "--";
    $("ble").textContent =
      data.ble_connected ? "CONNECTED" : "ADVERTISING";

    const envReady =
      !simulatedInterruption &&
      (data.sensor_type === "BME280" ||
       data.sensor_type === "BMP280") &&
      data.temperature_c !== null && data.pressure_hpa !== null;

    const imuReady = !!(data.sensors && data.sensors.imu);

    $("sensorType").textContent = data.sensor_type || "UNKNOWN";
    $("temperature").textContent =
      numberOrNA(data.temperature_c, 2) +
      (data.temperature_c !== null ? " °C" : "");
    $("pressure").textContent =
      numberOrNA(data.pressure_hpa, 2) +
      (data.pressure_hpa !== null ? " hPa" : "");
    $("humidity").textContent =
      numberOrNA(data.humidity_pct, 2) +
      (data.humidity_pct !== null ? " %" : "");

    $("envStatus").textContent = envReady ? "AVAILABLE" : "UNAVAILABLE";
    setHealth("envHealth", envReady ? "AVAILABLE" : "NOT DETECTED",
              envReady);
    setHealth("httpHealth",
              simulatedInterruption ? "SIMULATED INTERRUPTION" :
              telemetryMode === "simulation" ? "SIMULATED LINK" : "CONNECTED",
              !simulatedInterruption);
    setHealth("imu", imuReady ? "CONNECTED" : "NOT DETECTED",
              imuReady);
    setHealth("bleHealth",
              data.ble_connected ? "CONNECTED" : "ADVERTISING",
              !!data.ble_connected);

    showAxes("accel", [
      data.accel_x_ms2, data.accel_y_ms2, data.accel_z_ms2
    ], 2);

    showAxes("gyro", [
      data.gyro_x_dps, data.gyro_y_dps, data.gyro_z_dps
    ], 1);

    const hasEnvironment =
      data.temperature_c !== null && data.temperature_c !== undefined &&
      data.pressure_hpa !== null && data.pressure_hpa !== undefined &&
      Number.isFinite(Number(data.temperature_c)) &&
      Number.isFinite(Number(data.pressure_hpa));

    const hasMotion = imuReady &&
      [data.accel_x_ms2, data.accel_y_ms2, data.accel_z_ms2,
       data.gyro_x_dps, data.gyro_y_dps, data.gyro_z_dps]
      .every(v => v !== null && v !== undefined &&
                  Number.isFinite(Number(v)));

    $("telemetryHealth").textContent =
      hasEnvironment && hasMotion ? "ALL READINGS AVAILABLE" :
      hasEnvironment || hasMotion ? "PARTIAL TELEMETRY" :
      "CHECK SENSORS";

    $("version").textContent = data.version
      ? "V" + data.version : "V0.3";

    $("updated").textContent = telemetryMode === "simulation"
      ? (simulatedInterruption
          ? "SIMULATED LINK INTERRUPTION — real Wi-Fi/BLE remain untouched."
          : "Synthetic scenario update: " + new Date().toLocaleTimeString())
      : "Last successful update: " + new Date().toLocaleTimeString();

  } catch (error) {
    setStatus(false);
    setHealth("httpHealth", "NO RESPONSE", false);
    setHealth("envHealth", "UNKNOWN", false);
    setHealth("imu", "UNKNOWN", false);
    setHealth("bleHealth", "UNKNOWN", false);
    $("telemetryHealth").textContent = "TELEMETRY UNAVAILABLE";
    $("updated").textContent =
      "Waiting for ESP32 connection...";
  }
}


$("zeroOrientationBtn").addEventListener("click", async () => {
  const message = $("orientationMessage");
  const button = $("zeroOrientationBtn");

  if (telemetryMode === "simulation") {
    message.textContent = "Zero orientation is available only for live hardware; simulated attitude is generated for testing.";
    return;
  }

  button.disabled = true;
  message.textContent = "Setting current orientation as zero...";

  try {
    const response = await fetch("/zero", { method: "POST" });

    if (!response.ok) {
      throw new Error("Zero request failed");
    }

    message.textContent = "Orientation reference reset.";
    await refresh();
  } catch (error) {
    message.textContent = "Could not reset orientation. Check ESP32 connection.";
  } finally {
    button.disabled = false;
  }
});

modeSelect.addEventListener("change", () => {
  telemetryMode = modeSelect.value;
  selectHistoryForMode();
  updateSimulationControls();
  refresh();
});

scenarioSelect.addEventListener("change", () => {
  simulationTick = 0;
  selectHistoryForMode();
  // Start the selected scenario with clean synthetic history.
  simulatedHistory.chart = { temperature: [], pressure: [], humidity: [] };
  simulatedHistory.motion = { accelX: [], accelY: [], accelZ: [], gyroX: [], gyroY: [], gyroZ: [] };
  selectHistoryForMode();
  updateSimulationControls();
  refresh();
});

updateSimulationControls();
refresh();
setInterval(refresh, 1000);
</script>


</body>
</html>
)HTML";


// ==================================================
// HTTP ROUTES
// ==================================================
void handleDashboard() {
    server.send_P(200, "text/html", dashboard);
}

void handleTelemetry() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", telemetryJson);
}

// ==================================================
// OLED DISPLAY UPDATE
// ==================================================
void updateOLED() {
    if (!oledReady) {
        return;
    }

    unsigned long now = millis();
    if (now - lastDisplayUpdate < 1000) {
        return;
    }
    lastDisplayUpdate = now;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);

    display.println("MINI CUBESAT V0.3");
    display.println("----------------");

    if (sensorReady) {
        display.print("Temp: ");
        display.print(temperatureC, 1);
        display.println(" C");

        display.print("Press: ");
        display.print(pressureHpa, 1);
        display.println(" hPa");

        if (isBME280) {
            display.print("Humidity: ");
            display.print(humidityPct, 1);
            display.println(" %");
        } else {
            display.println("Humidity: N/A");
        }
    } else {
        display.println("Sensor not detected");
    }

    display.print("WiFi clients: ");
    display.println(WiFi.softAPgetStationNum());

    display.print("BLE: ");
    display.println(bleConnected ? "CONNECTED" : "READY");

    display.display();
}

// ==================================================
// SETUP
// ==================================================
void setup() {
    Serial.begin(115200);
    delay(300);

    // Start shared I2C bus before initializing OLED and sensor.
    Wire.begin(SDA_PIN, SCL_PIN);

    oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
    if (oledReady) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("MINI CUBESAT");
        display.println("Starting...");
        display.display();
    } else {
        Serial.println("OLED not detected.");
    }

    setupSensor();
    readSensor();
    setupMPU6050();
    readMPU6050();

    WiFi.mode(WIFI_AP);
    bool apStarted = WiFi.softAP(AP_NAME, AP_PASSWORD);
    if (!apStarted) {
        Serial.println("Failed to start Wi-Fi.");
    }

    setupBluetooth();

    updateTelemetry();

    server.on("/", HTTP_GET, handleDashboard);
    server.on("/api", HTTP_GET, handleTelemetry);
    
    server.on("/zero", HTTP_POST, []() {
        zeroOrientation();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
    server.begin();

    Serial.println();
    Serial.println("Mini CubeSat V0.3 started");
    Serial.print("Wi-Fi network: ");
    Serial.println(AP_NAME);
    Serial.println("Wi-Fi password: check AP_PASSWORD in src/main.cpp");
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.softAPIP());
    Serial.println("BLE name: MINI-CUBESAT-BLE");
    Serial.println("Web server ready");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {
    server.handleClient();

    // Update MPU6050 orientation every 20 ms
    if (millis() - lastImuUpdate >= 20) {
        lastImuUpdate = millis();
        readMPU6050();
        updateOrientation();
    }

    // Update telemetry once per second
    if (millis() - lastTelemetryUpdate >= 1000) {
        updateTelemetry();
    }

    updateOLED();
    delay(2);
}

