import csv
import time
from datetime import datetime
from pathlib import Path

import requests
import matplotlib

matplotlib.use("QtAgg")

import matplotlib.pyplot as plt
from matplotlib.dates import DateFormatter

# ---------------- CONFIGURATION ----------------

API_URL = "http://192.168.4.1/api"
LOG_INTERVAL = 1.0
MAX_POINTS = 60  # Show the latest 60 readings

LOG_DIR = Path("logs")
LOG_DIR.mkdir(exist_ok=True)

filename = LOG_DIR / (
    "cubesat_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".csv"
)

FIELDS = [
    "timestamp",
    "device",
    "version",
    "sequence",
    "uptime_s",
    "wifi_clients",
    "ble_connected",
    "sensor_type",
    "temperature_c",
    "pressure_hpa",
    "humidity_pct",
]


# ---------------- GRAPH SETUP ----------------

plt.ion()  # Turn on interactive graph updates

fig, axes = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
fig.suptitle("Mini CubeSat - Live Telemetry", fontsize=16)

temp_line, = axes[0].plot([], [], marker=".", label="Temperature")
pressure_line, = axes[1].plot([], [], marker=".", label="Pressure")
humidity_line, = axes[2].plot([], [], marker=".", label="Humidity")

axes[0].set_ylabel("Temperature (°C)")
axes[1].set_ylabel("Pressure (hPa)")
axes[2].set_ylabel("Humidity (%)")

for ax in axes:
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")

axes[2].set_xlabel("Time")
axes[2].xaxis.set_major_formatter(DateFormatter("%H:%M:%S"))

fig.tight_layout()


# Keep the most recent measurements in memory
times = []
temperatures = []
pressures = []
humidities = []


def number_or_none(value):
    """Convert a sensor value to a number, or None if unavailable."""
    try:
        if value is None:
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def update_graph():
    """Redraw the graphs using the latest measurements."""
    temp_line.set_data(times, temperatures)
    pressure_line.set_data(times, pressures)
    humidity_line.set_data(times, humidities)

    for ax in axes:
        ax.relim()
        ax.autoscale_view()

    # Keep the displayed time window readable
    if len(times) > 1:
        axes[2].set_xlim(times[0], times[-1])

    fig.canvas.draw_idle()
    fig.canvas.flush_events()


# ---------------- MAIN LOGGER ----------------

def main():
    print("=" * 55)
    print(" MINI CUBESAT - LIVE PYTHON GROUND STATION")
    print("=" * 55)
    print(f"Telemetry API: {API_URL}")
    print(f"CSV output:    {filename.resolve()}")
    print("Close the graph window or press Ctrl+C to stop.\n")

    session = requests.Session()

    try:
        with filename.open("w", newline="", encoding="utf-8") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=FIELDS)
            writer.writeheader()
            csv_file.flush()

            while plt.fignum_exists(fig.number):
                start = time.monotonic()

                try:
                    response = session.get(API_URL, timeout=3)
                    response.raise_for_status()
                    data = response.json()

                    now = datetime.now().astimezone()
                    timestamp = now.isoformat(timespec="seconds")

                    temperature = number_or_none(
                        data.get("temperature_c")
                    )
                    pressure = number_or_none(
                        data.get("pressure_hpa")
                    )
                    humidity = number_or_none(
                        data.get("humidity_pct")
                    )

                    row = {
                        "timestamp": timestamp,
                        "device": data.get("device", ""),
                        "version": data.get("version", ""),
                        "sequence": data.get("sequence", ""),
                        "uptime_s": data.get("uptime_s", ""),
                        "wifi_clients": data.get("wifi_clients", ""),
                        "ble_connected": data.get("ble_connected", ""),
                        "sensor_type": data.get("sensor_type", ""),
                        "temperature_c": temperature,
                        "pressure_hpa": pressure,
                        "humidity_pct": humidity,
                    }

                    # Save every successful reading to CSV
                    writer.writerow(row)
                    csv_file.flush()

                    # Add the new reading to the graph
                    times.append(now)
                    temperatures.append(temperature)
                    pressures.append(pressure)
                    humidities.append(humidity)

                    # Retain only the latest MAX_POINTS readings
                    if len(times) > MAX_POINTS:
                        times.pop(0)
                        temperatures.pop(0)
                        pressures.pop(0)
                        humidities.pop(0)

                    update_graph()

                    print(
                        f"[{timestamp}] "
                        f"Seq {row['sequence']} | "
                        f"Temp: {temperature} °C | "
                        f"Pressure: {pressure} hPa | "
                        f"Humidity: {humidity} %"
                    )

                except (requests.RequestException, ValueError) as error:
                    print(f"Telemetry error: {error}")

                # Allow the graph window to respond to user actions
                plt.pause(0.01)

                elapsed = time.monotonic() - start
                time.sleep(max(0, LOG_INTERVAL - elapsed))

    except KeyboardInterrupt:
        print("\nLogging stopped by user.")

    finally:
        session.close()
        plt.ioff()
        plt.close("all")
        print(f"CSV file saved at: {filename.resolve()}")


if __name__ == "__main__":
    main()
