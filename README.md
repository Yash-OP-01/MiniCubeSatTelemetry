# Mini CubeSat Telemetry Board

An ESP32-based CubeSat telemetry prototype with environmental and motion sensing, a Wi-Fi telemetry dashboard, BLE status, and a Python desktop ground station. Hardware and software are still under development and testing.

## Project structure

- `src/main.cpp` — current ESP32 firmware and embedded web dashboard.
- `platformio.ini` — PlatformIO board/framework/dependency configuration.
- `MiniCubeSatGroundStation/ground_station.py` — current Python desktop ground station (selected from the supplied `ground_station_v2_final_ui.py`).
- `MiniCubeSatGroundStation/requirements.txt` — Python dependencies.
- `archive/legacy-versions/` — distinct previous firmware and ground-station versions retained for reference.

Generated PlatformIO build output, Python virtual environments, local CSV telemetry logs, and machine-specific VS Code settings are intentionally excluded.

## Hardware / features

- ESP32 development board (`esp32dev` PlatformIO target)
- BME280 environmental sensor (the firmware also includes BMP280 support)
- MPU6050 motion sensor
- SSD1306 OLED display support
- Wi-Fi SoftAP with HTTP telemetry endpoint and embedded browser dashboard
- BLE device/status support
- Python desktop ground station with telemetry plots and optional local CSV logging

Confirm your exact sensor modules, wiring, and display before powering the board; this README does not replace your wiring notes.

## Build and upload ESP32 firmware

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the PlatformIO IDE extension.
2. Open this folder (`MiniCubeSatTelemetry`) in VS Code.
3. Connect the ESP32 over USB and select the correct serial port if prompted.
4. Review `src/main.cpp` and set the Wi-Fi access-point password as described below.
5. Use PlatformIO **Build** first, then **Upload**. Open the Serial Monitor at **115200 baud**.

The PlatformIO environment is configured in `platformio.ini`; dependencies are declared there and PlatformIO will fetch them when needed.

## Wi-Fi access point

The firmware starts a Wi-Fi access point named `MINI-CUBESAT`. In `src/main.cpp`, change `AP_PASSWORD` from the example value `ChangeThis123` to a strong password of at least 8 characters before using the board outside a controlled demo. Do not publish a personal/home Wi-Fi password or reuse a password from another account.

Connect your computer to the ESP32 access point, then open the ESP32 address printed in the Serial Monitor (the default SoftAP address is commonly `192.168.4.1`). The embedded dashboard and `/api` telemetry endpoint are served by the firmware.

## Run the Python ground station (Windows PowerShell)

From the project root:

```powershell
cd .\MiniCubeSatGroundStation
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe ground_station.py
```

If Python 3.11 is not installed, use an installed supported Python version and replace `py -3.11` accordingly. The ground station currently targets the ESP32 API at `http://192.168.4.1/api`; update `API_URL` near the top of `ground_station.py` if your network setup uses another address.

The app creates a `logs/` folder for CSV telemetry output at runtime. Logs are ignored by Git by default because they may contain device/test data.

## BLE

The firmware advertises a BLE device named `MINI-CUBESAT-BLE`. BLE behavior depends on the firmware version and client software; verify it on the actual hardware.

## Development status

Prototype / in active hardware and software testing. Build and runtime behavior have not been independently validated as part of this packaging step. Please test the firmware and ground station on your own hardware before relying on them for a demo.

## Before publishing

- Replace the example access-point password with a project-specific demo password (never a personal password).
- Check wiring, pin assignments, sensor addresses, and library versions against the physical board.
- Do not add `.venv/`, `.pio/`, logs, tokens, or private credentials to the repository.
- Add photos, wiring diagrams, and license information when ready.
