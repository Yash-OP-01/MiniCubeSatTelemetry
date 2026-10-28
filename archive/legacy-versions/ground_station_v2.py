
import sys
import csv
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import requests
import numpy as np

from PySide6.QtCore import QObject, Signal, QRunnable, QThreadPool, QTimer, Qt, QPointF
from PySide6.QtGui import QFont, QPainter, QPen, QBrush, QColor, QPolygonF, QPainterPath
from PySide6.QtWidgets import (
    QApplication,
    QMainWindow,
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QGridLayout,
    QLabel,
    QPushButton,
    QFrame,
    QScrollArea,
)

from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg
from matplotlib.figure import Figure
from matplotlib.dates import DateFormatter
from mpl_toolkits.mplot3d.art3d import Poly3DCollection


# ============================================================
# CONFIGURATION
# ============================================================

API_URL = "http://192.168.4.1/api"
POLL_INTERVAL_MS = 3000
MAX_POINTS = 60

LOG_DIR = Path(__file__).resolve().parent / "logs"
LOG_DIR.mkdir(exist_ok=True)

CSV_FIELDS = [
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
    "accel_x_ms2",
    "accel_y_ms2",
    "accel_z_ms2",
    "gyro_x_dps",
    "gyro_y_dps",
    "gyro_z_dps",
    "pitch_deg",
    "roll_deg",
    "yaw_deg",
    "bme280",
    "imu",
]


# ============================================================
# HELPER FUNCTIONS
# ============================================================

def number_or_none(value):
    try:
        if value is None or value == "":
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def display_value(value, decimals=2, suffix=""):
    number = number_or_none(value)
    if number is None:
        return "--"
    return f"{number:.{decimals}f}{suffix}"


def safe_text(value, default="--"):
    if value is None or value == "":
        return default
    return str(value)


# ============================================================
# LIGHT AND DARK THEMES
# ============================================================

LIGHT_STYLE = """
QMainWindow, QWidget#central {
    background: #F3F7FC;
    color: #192943;
    font-family: "Segoe UI";
    font-size: 13px;
}
QFrame#card {
    background: #FFFFFF;
    border: 1px solid #DCE6F2;
    border-radius: 12px;
}
QLabel#muted {
    color: #71819A;
}
QLabel#metric {
    color: #1769C2;
    font-size: 25px;
    font-weight: bold;
}
QLabel#section {
    color: #192943;
    font-size: 16px;
    font-weight: bold;
}
QPushButton {
    background: #1769D2;
    color: white;
    border: none;
    border-radius: 8px;
    padding: 9px 14px;
    font-weight: bold;
}
QPushButton:hover {
    background: #0E55B4;
}
QPushButton#secondary {
    background: #E4EDF9;
    color: #174A91;
}
QPushButton#secondary:hover {
    background: #D2E2F7;
}
QScrollArea {
    border: none;
    background: transparent;
}
QPushButton:disabled {
    background: #AAB7C8;
    color: #F7FAFC;
}
QLabel {
    padding: 1px 0;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px 2px 4px 2px;
}
QScrollBar::handle:vertical {
    background: #A9BBD1;
    min-height: 28px;
    border-radius: 4px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
"""

DARK_STYLE = """
QMainWindow, QWidget#central {
    background: #101827;
    color: #E8EEF8;
    font-family: "Segoe UI";
    font-size: 13px;
}
QFrame#card {
    background: #192538;
    border: 1px solid #30405A;
    border-radius: 12px;
}
QLabel#muted {
    color: #A0AEC3;
}
QLabel#metric {
    color: #68B9FF;
    font-size: 25px;
    font-weight: bold;
}
QLabel#section {
    color: #F0F5FF;
    font-size: 16px;
    font-weight: bold;
}
QPushButton {
    background: #2379DF;
    color: white;
    border: none;
    border-radius: 8px;
    padding: 9px 14px;
    font-weight: bold;
}
QPushButton:hover {
    background: #398BED;
}
QPushButton#secondary {
    background: #293952;
    color: #D9E8FF;
}
QPushButton#secondary:hover {
    background: #364A67;
}
QScrollArea {
    border: none;
    background: transparent;
}
QPushButton:disabled {
    background: #46556B;
    color: #9CAAC0;
}
QLabel {
    padding: 1px 0;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px 2px 4px 2px;
}
QScrollBar::handle:vertical {
    background: #465A75;
    min-height: 28px;
    border-radius: 4px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
"""


# ============================================================
# BACKGROUND TELEMETRY WORKER
# ============================================================

class TelemetrySignals(QObject):
    success = Signal(dict)
    failure = Signal(str)
    finished = Signal()


class TelemetryWorker(QRunnable):
    """Perform HTTP requests away from the GUI thread."""

    def __init__(self, api_url):
        super().__init__()
        self.api_url = api_url
        self.signals = TelemetrySignals()

    def run(self):
        try:
            with requests.Session() as session:
                response = session.get(self.api_url, timeout=1.0)
                response.raise_for_status()
                data = response.json()

            if not isinstance(data, dict):
                raise ValueError("API response is not a JSON object")

            self.signals.success.emit(data)

        except (requests.RequestException, ValueError) as error:
            self.signals.failure.emit(str(error))

        except Exception as error:
            self.signals.failure.emit(
                f"Unexpected error: {error}"
            )

        finally:
            self.signals.finished.emit()


# ============================================================
# METRIC CARD
# ============================================================

class MetricCard(QFrame):
    def __init__(self, title, unit):
        super().__init__()
        self.setObjectName("card")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 15, 18, 15)
        layout.setSpacing(7)

        title_label = QLabel(title)
        title_label.setObjectName("muted")

        self.value_label = QLabel("--")
        self.value_label.setObjectName("metric")

        unit_label = QLabel(unit)
        unit_label.setObjectName("muted")

        layout.addWidget(title_label)
        layout.addWidget(self.value_label)
        layout.addWidget(unit_label)
        layout.addStretch()

    def set_value(self, value):
        self.value_label.setText(value)


# ============================================================
# LIVE CHARTS
# ============================================================

class TelemetryCharts(FigureCanvasQTAgg):
    def __init__(self, parent=None):
        self.figure = Figure(figsize=(9, 5), dpi=90)
        super().__init__(self.figure)
        self.setParent(parent)

        self.axes = self.figure.subplots(3, 1, sharex=True)
        self.figure.subplots_adjust(
            left=0.12,
            right=0.97,
            top=0.96,
            bottom=0.11,
            hspace=0.42,
        )

        self.times = deque(maxlen=MAX_POINTS)
        self.temperature_values = deque(maxlen=MAX_POINTS)
        self.pressure_values = deque(maxlen=MAX_POINTS)
        self.humidity_values = deque(maxlen=MAX_POINTS)

        self.lines = []

        chart_settings = [
            ("Temperature", "°C", "#1675D1"),
            ("Pressure", "hPa", "#13A6A0"),
            ("Humidity", "%", "#8A63D2"),
        ]

        for ax, (name, unit, color) in zip(
            self.axes, chart_settings
        ):
            ax.set_ylabel(f"{name} ({unit})")
            line, = ax.plot(
                [],
                [],
                color=color,
                linewidth=1.8,
                marker=".",
                markersize=3,
            )
            self.lines.append(line)

        self.axes[-1].set_xlabel("Time")
        self.axes[-1].xaxis.set_major_formatter(
            DateFormatter("%H:%M:%S")
        )

        self.set_theme(False)

    def set_theme(self, dark):
        background = "#192538" if dark else "#FFFFFF"
        foreground = "#E8EEF8" if dark else "#263752"
        grid_color = "#40516A" if dark else "#DDE6F2"

        self.figure.patch.set_facecolor(background)

        for ax in self.axes:
            ax.set_facecolor(background)
            ax.tick_params(colors=foreground, labelsize=8)
            ax.xaxis.label.set_color(foreground)
            ax.yaxis.label.set_color(foreground)

            for spine in ax.spines.values():
                spine.set_color(grid_color)

            ax.grid(True, color=grid_color, alpha=0.35)

        self.draw_idle()

    def add_reading(self, now, temperature, pressure, humidity):
        self.times.append(now)
        self.temperature_values.append(temperature)
        self.pressure_values.append(pressure)
        self.humidity_values.append(humidity)

        series = [
            self.temperature_values,
            self.pressure_values,
            self.humidity_values,
        ]

        for line, values, ax in zip(
            self.lines, series, self.axes
        ):
            line.set_data(list(self.times), list(values))
            ax.relim()
            ax.autoscale_view()

        if len(self.times) > 1:
            self.axes[-1].set_xlim(
                self.times[0], self.times[-1]
            )

        self.draw_idle()


# ============================================================
# LIVE ATTITUDE INDICATOR
# ============================================================

class AttitudeIndicator(QWidget):
    """Horizon-style pitch/roll display driven by reported telemetry."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.pitch = None
        self.roll = None
        self.online = False
        self.dark = False
        self.setMinimumSize(380, 300)
        self.setMaximumSize(460, 340)

    def set_theme(self, dark):
        self.dark = dark
        self.update()

    def set_attitude(self, pitch, roll, online=True):
        self.pitch = number_or_none(pitch)
        self.roll = number_or_none(roll)
        self.online = bool(online) and self.pitch is not None and self.roll is not None
        self.update()

    def set_offline(self):
        self.online = False
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        bg = QColor("#192538" if self.dark else "#FFFFFF")
        fg = QColor("#E8EEF8" if self.dark else "#263752")
        muted = QColor("#A0AEC3" if self.dark else "#71819A")
        painter.fillRect(self.rect(), bg)

        cx, cy = w / 2, h / 2 - 3
        radius = max(70, min(w * 0.31, h * 0.39))
        painter.setPen(QPen(QColor("#7E91AA" if self.dark else "#AABBD0"), 1.5))
        painter.setBrush(QBrush(QColor("#24364B" if self.dark else "#EAF1F8")))
        painter.drawEllipse(QPointF(cx, cy), radius + 4, radius + 4)

        painter.save()
        clip_path = QPainterPath()
        clip_path.addEllipse(QPointF(cx, cy), radius - 1, radius - 1)
        painter.setClipPath(clip_path)
        painter.translate(cx, cy)
        if self.online:
            pitch = max(-45.0, min(45.0, self.pitch))
            roll = max(-90.0, min(90.0, self.roll))
            # Pitch moves the horizon vertically; roll rotates the world around it.
            horizon_y = pitch / 45.0 * radius * 0.72
        else:
            roll, horizon_y = 0.0, 0.0
        painter.rotate(-roll)
        extent = radius * 2.4
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#3B8ED0" if self.dark else "#73BDEB"))
        painter.drawRect(int(-extent), int(-extent + horizon_y), int(extent * 2), int(extent))
        painter.setBrush(QColor("#9B784B" if self.dark else "#C9A06B"))
        painter.drawRect(int(-extent), int(horizon_y), int(extent * 2), int(extent))
        painter.setPen(QPen(QColor("#FFFFFF"), 2))
        painter.drawLine(QPointF(-extent, horizon_y), QPointF(extent, horizon_y))

        # Pitch ladder, spaced every 10 degrees.
        painter.setPen(QPen(QColor("#FFFFFF"), 1.5))
        for deg in range(-30, 31, 10):
            if deg == 0:
                continue
            y = horizon_y + deg / 45.0 * radius * 0.72
            half = radius * (0.30 if deg % 20 else 0.42)
            painter.drawLine(QPointF(-half, y), QPointF(half, y))
            painter.setFont(QFont("Segoe UI", 8, QFont.Weight.DemiBold))
            painter.drawText(QPointF(-half - 24, y + 4), str(abs(deg)))
            painter.drawText(QPointF(half + 8, y + 4), str(abs(deg)))
        painter.restore()

        # Circular bezel and fixed aircraft reference symbol.
        painter.setPen(QPen(fg, 2))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawEllipse(QPointF(cx, cy), radius, radius)
        painter.setPen(QPen(QColor("#FFD34E"), 3.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        painter.drawLine(QPointF(cx - radius * .48, cy), QPointF(cx - radius * .12, cy))
        painter.drawLine(QPointF(cx + radius * .12, cy), QPointF(cx + radius * .48, cy))
        painter.drawLine(QPointF(cx, cy - 8), QPointF(cx, cy + 8))

        painter.setPen(QPen(fg, 1))
        painter.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
        painter.drawText(10, 20, "ROLL")
        painter.drawText(w - 72, 20, "PITCH")
        painter.setFont(QFont("Segoe UI", 10))
        painter.drawText(10, 39, f"{self.roll:+.1f}°" if self.roll is not None else "--")
        painter.drawText(w - 72, 39, f"{self.pitch:+.1f}°" if self.pitch is not None else "--")
        painter.setPen(muted if self.online else QColor("#E05252"))
        painter.setFont(QFont("Segoe UI", 9, QFont.Weight.DemiBold))
        status = "LIVE TELEMETRY · INDICATIVE" if self.online else "WAITING FOR IMU TELEMETRY"
        painter.drawText(0, h - 26, w, 18, Qt.AlignmentFlag.AlignHCenter, status)
        painter.end()


# ============================================================

# ============================================================
# 3D CUBESAT ORIENTATION VIEW
# ============================================================

class CubeSat3DView(FigureCanvasQTAgg):
    """Simple rotatable CubeSat model; simulates attitude until telemetry arrives."""

    def __init__(self, parent=None):
        self.figure = Figure(figsize=(5.2, 4.0), dpi=90)
        super().__init__(self.figure)
        self.setParent(parent)
        self.axes = self.figure.add_subplot(111, projection="3d")
        self.pitch = 0.0
        self.roll = 0.0
        self.yaw = 0.0
        self.dark = False
        self.simulation_time = 0.0
        self.telemetry_live = False
        self.setMinimumHeight(330)
        self.set_theme(False)
        self._draw_model()

    @staticmethod
    def _rotate(points, pitch, roll, yaw):
        p, r, y = np.radians([pitch, roll, yaw])
        rx = np.array([[1, 0, 0], [0, np.cos(r), -np.sin(r)],
                       [0, np.sin(r), np.cos(r)]])
        ry = np.array([[np.cos(p), 0, np.sin(p)], [0, 1, 0],
                       [-np.sin(p), 0, np.cos(p)]])
        rz = np.array([[np.cos(y), -np.sin(y), 0],
                       [np.sin(y), np.cos(y), 0], [0, 0, 1]])
        return np.asarray(points) @ (rz @ ry @ rx).T

    def set_theme(self, dark):
        self.dark = dark
        bg = "#192538" if dark else "#FFFFFF"
        self.figure.patch.set_facecolor(bg)
        self.axes.set_facecolor(bg)
        self.draw_idle()

    def set_orientation(self, pitch=None, roll=None, yaw=None, live=False):
        if live:
            self.pitch = number_or_none(pitch) or 0.0
            self.roll = number_or_none(roll) or 0.0
            self.yaw = number_or_none(yaw) or 0.0
            self.telemetry_live = True
        else:
            self.telemetry_live = False
        self._draw_model()

    def advance_demo(self):
        if not self.telemetry_live:
            self.simulation_time += 0.045
            self.pitch = 12.0 * np.sin(self.simulation_time * 0.72)
            self.roll = 24.0 * np.sin(self.simulation_time * 0.48)
            self.yaw = (self.yaw + 1.2) % 360.0
            self._draw_model()

    def _draw_model(self):
        self.axes.clear()
        self.axes.set_axis_off()
        self.axes.set_xlim(-2.0, 2.0)
        self.axes.set_ylim(-1.8, 1.8)
        self.axes.set_zlim(-1.8, 1.8)
        self.axes.set_box_aspect((1.25, 1.0, 1.15))
        self.axes.view_init(elev=22, azim=34)
        bg = "#192538" if self.dark else "#FFFFFF"
        fg = "#E8EEF8" if self.dark else "#263752"
        self.axes.set_facecolor(bg)
        self.figure.patch.set_facecolor(bg)

        v = np.array([
            [-.48, -.48, -.48], [.48, -.48, -.48],
            [.48, .48, -.48], [-.48, .48, -.48],
            [-.48, -.48, .48], [.48, -.48, .48],
            [.48, .48, .48], [-.48, .48, .48],
        ])
        face_indices = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4),
                        (2, 3, 7, 6), (1, 2, 6, 5), (0, 3, 7, 4)]
        faces = [self._rotate(v[list(face)], self.pitch, self.roll, self.yaw)
                 for face in face_indices]
        self.axes.add_collection3d(Poly3DCollection(
            faces, facecolors=["#D7E3F1", "#9FB5CD", "#4D9BE6",
                               "#2868A9", "#6BB5F2", "#3D78B8"],
            edgecolors="#EAF4FF" if self.dark else "#244D76",
            linewidths=1.0, alpha=1.0))

        panels = [
            np.array([[.55, -.65, -.06], [1.52, -.65, -.06],
                      [1.52, .65, -.06], [.55, .65, -.06]]),
            np.array([[-1.52, -.65, .06], [-.55, -.65, .06],
                      [-.55, .65, .06], [-1.52, .65, .06]]),
        ]
        panel_faces = [self._rotate(panel, self.pitch, self.roll, self.yaw)
                       for panel in panels]
        self.axes.add_collection3d(Poly3DCollection(
            panel_faces, facecolors=["#174D91", "#246DB8"],
            edgecolors="#8FC8FF", linewidths=1.2, alpha=.96))

        # Solar-cell dividers across each panel.
        for panel_x in ([.79, 1.03, 1.27], [-1.27, -1.03, -.79]):
            for x in panel_x:
                pts = self._rotate(np.array([[x, -.65, 0], [x, .65, 0]]),
                                   self.pitch, self.roll, self.yaw)
                self.axes.plot(pts[:, 0], pts[:, 1], pts[:, 2],
                               color="#8FC8FF", linewidth=.7)
        for panel_edges in ((.55, 1.52), (-1.52, -.55)):
            for y in (-.22, .22):
                pts = self._rotate(np.array([[panel_edges[0], y, 0],
                                             [panel_edges[1], y, 0]]),
                                   self.pitch, self.roll, self.yaw)
                self.axes.plot(pts[:, 0], pts[:, 1], pts[:, 2],
                               color="#8FC8FF", linewidth=.7)

        antenna = self._rotate(np.array([[0, 0, .48], [0, 0, 1.05]]),
                               self.pitch, self.roll, self.yaw)
        self.axes.plot(antenna[:, 0], antenna[:, 1], antenna[:, 2],
                       color="#E8B84D", linewidth=2.2)
        self.axes.scatter([antenna[-1, 0]], [antenna[-1, 1]],
                          [antenna[-1, 2]], color="#E8B84D", s=18,
                          depthshade=False)

        mode = "LIVE ATTITUDE" if self.telemetry_live else "SIMULATED ROTATION"
        self.axes.text2D(0.02, .97, mode, transform=self.axes.transAxes,
                         color="#36B37E" if self.telemetry_live else "#D99A24",
                         fontsize=9, fontweight="bold", va="top")
        self.axes.text2D(
            .02, .03,
            f"Pitch {self.pitch:+.1f}°   Roll {self.roll:+.1f}°   Yaw {self.yaw:+.1f}°",
            transform=self.axes.transAxes, color=fg, fontsize=9, va="bottom")
        self.draw_idle()


# MAIN GROUND STATION WINDOW
# ============================================================

class GroundStation(QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle(
            "Mini CubeSat | Telemetry Ground Station V2"
        )
        self.resize(1440, 960)
        self.setMinimumSize(980, 700)

        self.dark_mode = False
        self.logging_enabled = True

        self.request_in_flight = False
        self.active_worker = None
        self.last_data = None
        self.last_success_time = None
        self.consecutive_errors = 0

        self.thread_pool = QThreadPool(self)
        self.thread_pool.setMaxThreadCount(2)

        self._create_csv()
        self._build_ui()
        self.set_theme(False)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.start_poll)
        self.timer.start(POLL_INTERVAL_MS)

        self.demo_timer = QTimer(self)
        self.demo_timer.timeout.connect(self.cubesat_view.advance_demo)
        self.demo_timer.start(80)

        # Show the window first; connect in the background.
        QTimer.singleShot(0, self.start_poll)

    # --------------------------------------------------------
    # CSV FILE
    # --------------------------------------------------------

    def _create_csv(self):
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.csv_path = LOG_DIR / f"cubesat_{stamp}.csv"

        self.csv_file = self.csv_path.open(
            "w", newline="", encoding="utf-8"
        )
        self.csv_writer = csv.DictWriter(
            self.csv_file,
            fieldnames=CSV_FIELDS,
        )
        self.csv_writer.writeheader()
        self.csv_file.flush()

    def write_csv(self, data, timestamp):
        if not self.logging_enabled:
            return

        row = {"timestamp": timestamp}
        sensors = data.get("sensors", {})

        for field in CSV_FIELDS:
            if field == "timestamp":
                continue
            if field in ("bme280", "imu"):
                row[field] = sensors.get(field, "")
            else:
                row[field] = data.get(field, "")

        self.csv_writer.writerow(row)
        self.csv_file.flush()

    # --------------------------------------------------------
    # BUILD INTERFACE
    # --------------------------------------------------------

    def _build_ui(self):
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)

        central = QWidget()
        central.setObjectName("central")

        root = QVBoxLayout(central)
        root.setContentsMargins(24, 20, 24, 22)
        root.setSpacing(17)

        # Header
        header = QHBoxLayout()
        title_box = QVBoxLayout()

        title = QLabel("MINI CUBESAT")
        title.setStyleSheet(
            "font-size: 27px; font-weight: 800; letter-spacing: 0.4px;"
        )

        subtitle = QLabel(
            "TELEMETRY GROUND STATION  /  V2"
        )
        subtitle.setObjectName("muted")
        subtitle.setStyleSheet("font-size: 11px; letter-spacing: 1.1px;")

        title_box.addWidget(title)
        title_box.addWidget(subtitle)

        tagline = QLabel("LIVE SENSOR MONITORING  •  ORIENTATION  •  DATA LOGGING")
        tagline.setObjectName("muted")
        tagline.setStyleSheet("font-size: 10px; letter-spacing: 0.5px;")
        title_box.addWidget(tagline)

        header.addLayout(title_box)
        header.addStretch()

        self.connection_label = QLabel("● Starting...")
        self.connection_label.setToolTip(
            "Connection status for the ESP32 telemetry API"
        )
        self.connection_label.setStyleSheet(
            "font-weight: bold; padding: 6px 10px;"
        )
        header.addWidget(self.connection_label)

        self.theme_button = QPushButton("Dark mode")
        self.theme_button.setObjectName("secondary")
        self.theme_button.clicked.connect(self.toggle_theme)
        header.addWidget(self.theme_button)

        self.reconnect_button = QPushButton("Reconnect")
        self.reconnect_button.setObjectName("secondary")
        self.reconnect_button.clicked.connect(self.reconnect)
        header.addWidget(self.reconnect_button)

        self.log_button = QPushButton("Pause logging")
        self.log_button.setObjectName("secondary")
        self.log_button.clicked.connect(self.toggle_logging)
        header.addWidget(self.log_button)

        root.addLayout(header)

        # Device overview
        overview = QFrame()
        overview.setObjectName("card")

        overview_grid = QGridLayout(overview)
        overview_grid.setContentsMargins(20, 17, 20, 17)
        overview_grid.setHorizontalSpacing(28)
        overview_grid.setVerticalSpacing(14)

        self.device_label = QLabel("Device: --")
        self.version_label = QLabel("Firmware: --")
        self.sequence_label = QLabel("Sequence: --")
        self.uptime_label = QLabel("Uptime: --")
        self.wifi_label = QLabel("Wi-Fi clients: --")
        self.ble_label = QLabel("BLE: --")
        self.sensor_label = QLabel("Sensor: --")
        self.health_label = QLabel("Sensors: waiting for data")

        info_labels = [
            self.device_label,
            self.version_label,
            self.sequence_label,
            self.uptime_label,
            self.wifi_label,
            self.ble_label,
            self.sensor_label,
            self.health_label,
        ]

        for index, label in enumerate(info_labels):
            overview_grid.addWidget(
                label, index // 4, index % 4
            )

        root.addWidget(overview)

        # Environmental cards
        section = QLabel("Environmental telemetry")
        section.setObjectName("section")
        root.addWidget(section)

        metric_grid = QGridLayout()
        metric_grid.setSpacing(14)

        self.temperature_card = MetricCard(
            "TEMPERATURE", "Degrees Celsius"
        )
        self.pressure_card = MetricCard(
            "PRESSURE", "Hectopascals"
        )
        self.humidity_card = MetricCard(
            "HUMIDITY", "Relative humidity"
        )

        metric_grid.addWidget(self.temperature_card, 0, 0)
        metric_grid.addWidget(self.pressure_card, 0, 1)
        metric_grid.addWidget(self.humidity_card, 0, 2)

        root.addLayout(metric_grid)

        # Embedded charts
        chart_card = QFrame()
        chart_card.setObjectName("card")

        chart_layout = QVBoxLayout(chart_card)
        chart_layout.setContentsMargins(18, 17, 18, 16)
        chart_layout.setSpacing(10)

        chart_title = QLabel("Live telemetry history")
        chart_title.setObjectName("section")
        chart_layout.addWidget(chart_title)

        self.charts = TelemetryCharts()
        self.charts.setMinimumHeight(340)
        chart_layout.addWidget(self.charts)

        root.addWidget(chart_card)

        # Attitude indicator (pitch / roll)
        attitude_card = QFrame()
        attitude_card.setObjectName("card")
        attitude_layout = QVBoxLayout(attitude_card)
        attitude_layout.setContentsMargins(18, 17, 18, 16)
        attitude_layout.setSpacing(8)
        attitude_title = QLabel("Live attitude indicator")
        attitude_title.setObjectName("section")
        attitude_layout.addWidget(attitude_title)
        attitude_note = QLabel("Horizon-style visualisation from reported MPU6050 pitch and roll")
        attitude_note.setObjectName("muted")
        attitude_layout.addWidget(attitude_note)
        self.attitude_indicator = AttitudeIndicator()
        attitude_layout.addWidget(self.attitude_indicator, alignment=Qt.AlignmentFlag.AlignHCenter)
        root.addWidget(attitude_card)

        # 3D CubeSat model (animated demo until live telemetry is available)
        cubesat_card = QFrame()
        cubesat_card.setObjectName("card")
        cubesat_layout = QVBoxLayout(cubesat_card)
        cubesat_layout.setContentsMargins(18, 17, 18, 16)
        cubesat_layout.setSpacing(8)
        cubesat_title = QLabel("3D CubeSat orientation")
        cubesat_title.setObjectName("section")
        cubesat_layout.addWidget(cubesat_title)
        cubesat_note = QLabel(
            "Animated simulated attitude while the ESP32 is disconnected; "
            "switches to reported orientation when telemetry arrives."
        )
        cubesat_note.setObjectName("muted")
        cubesat_layout.addWidget(cubesat_note)
        self.cubesat_view = CubeSat3DView()
        cubesat_layout.addWidget(self.cubesat_view)
        root.addWidget(cubesat_card)

        # Motion readings
        motion_title = QLabel(
            "MPU6050 motion and orientation"
        )
        motion_title.setObjectName("section")
        root.addWidget(motion_title)

        motion_card = QFrame()
        motion_card.setObjectName("card")

        motion_grid = QGridLayout(motion_card)
        motion_grid.setContentsMargins(20, 18, 20, 18)
        motion_grid.setHorizontalSpacing(24)
        motion_grid.setVerticalSpacing(18)

        self.motion_labels = {}

        motion_items = [
            ("accel_x_ms2", "Acceleration X", "m/s²"),
            ("accel_y_ms2", "Acceleration Y", "m/s²"),
            ("accel_z_ms2", "Acceleration Z", "m/s²"),
            ("gyro_x_dps", "Gyroscope X", "°/s"),
            ("gyro_y_dps", "Gyroscope Y", "°/s"),
            ("gyro_z_dps", "Gyroscope Z", "°/s"),
            ("pitch_deg", "Pitch", "°"),
            ("roll_deg", "Roll", "°"),
            ("yaw_deg", "Yaw", "°"),
        ]

        for index, (key, name, unit) in enumerate(motion_items):
            cell = QVBoxLayout()

            name_label = QLabel(name)
            name_label.setObjectName("muted")

            value_label = QLabel("--")
            value_label.setObjectName("metric")
            value_label.setStyleSheet("font-size: 20px;")

            unit_label = QLabel(unit)
            unit_label.setObjectName("muted")

            cell.addWidget(name_label)
            cell.addWidget(value_label)
            cell.addWidget(unit_label)

            motion_grid.addLayout(
                cell, index // 3, index % 3
            )
            self.motion_labels[key] = value_label

        root.addWidget(motion_card)

        # Footer
        footer = QHBoxLayout()

        self.last_update_label = QLabel(
            "Last update: waiting for telemetry"
        )
        self.last_update_label.setObjectName("muted")

        self.csv_label = QLabel(
            f"CSV: {self.csv_path.name}"
        )
        self.csv_label.setObjectName("muted")

        footer.addWidget(self.last_update_label)
        footer.addStretch()
        footer.addWidget(self.csv_label)

        root.addLayout(footer)

        scroll.setWidget(central)
        self.setCentralWidget(scroll)

        self.statusBar().showMessage(
            f"Ready for hardware connection | API endpoint: {API_URL}"
        )

    # --------------------------------------------------------
    # THEME
    # --------------------------------------------------------

    def toggle_theme(self):
        self.set_theme(not self.dark_mode)

    def set_theme(self, dark):
        self.dark_mode = dark
        self.setStyleSheet(
            DARK_STYLE if dark else LIGHT_STYLE
        )

        self.theme_button.setText(
            "Light mode" if dark else "Dark mode"
        )
        self.charts.set_theme(dark)
        if hasattr(self, "attitude_indicator"):
            self.attitude_indicator.set_theme(dark)
        if hasattr(self, "cubesat_view"):
            self.cubesat_view.set_theme(dark)

    # --------------------------------------------------------
    # LOGGING CONTROLS
    # --------------------------------------------------------

    def toggle_logging(self):
        self.logging_enabled = not self.logging_enabled

        self.log_button.setText(
            "Pause logging"
            if self.logging_enabled
            else "Resume logging"
        )

        self.statusBar().showMessage(
            "CSV logging enabled"
            if self.logging_enabled
            else "CSV logging paused"
        )

    # --------------------------------------------------------
    # CONNECTION CONTROLS
    # --------------------------------------------------------

    def start_poll(self):
        """Launch a request only when one is not already running."""
        if self.request_in_flight:
            return

        self.request_in_flight = True
        self.reconnect_button.setEnabled(False)

        if self.last_data is None:
            self.connection_label.setText("● Connecting...")
            self.connection_label.setStyleSheet(
                "color: #D99A24; font-weight: bold;"
            )

        worker = TelemetryWorker(API_URL)
        self.active_worker = worker

        worker.signals.success.connect(
            self.handle_telemetry
        )
        worker.signals.failure.connect(
            self.handle_telemetry_error
        )
        worker.signals.finished.connect(
            self.request_finished
        )

        self.thread_pool.start(worker)

    def reconnect(self):
        """Try connecting immediately."""
        if self.request_in_flight:
            self.statusBar().showMessage(
                "A connection attempt is already running."
            )
            return

        self.statusBar().showMessage(
            "Trying to reconnect to the ESP32..."
        )
        self.start_poll()

    def handle_telemetry(self, data):
        """Process successful telemetry on the GUI thread."""
        now = datetime.now().astimezone()
        timestamp = now.isoformat(timespec="seconds")

        self.last_data = data
        self.last_success_time = time.monotonic()
        self.consecutive_errors = 0

        self.update_dashboard(data, now, timestamp)
        self.write_csv(data, timestamp)

        self.connection_label.setText("● Connected")
        self.connection_label.setStyleSheet(
            "color: #159765; font-weight: bold;"
        )

        self.statusBar().showMessage(
            f"Connected to {safe_text(data.get('device'))}"
        )

    def handle_telemetry_error(self, error):
        """Report disconnection without blocking the UI."""
        self.consecutive_errors += 1

        self.connection_label.setText("● ESP32 offline")
        self.attitude_indicator.set_offline()
        self.cubesat_view.set_orientation(live=False)
        self.connection_label.setStyleSheet(
            "color: #E05252; font-weight: bold;"
        )

        self.statusBar().showMessage(
            "ESP32 not connected — the 3D model is in simulation mode. "
            "Automatic retry is enabled; click Reconnect to try now."
        )

    def request_finished(self):
        """Allow future polling requests."""
        self.request_in_flight = False
        self.active_worker = None
        self.reconnect_button.setEnabled(True)

    # --------------------------------------------------------
    # UPDATE DASHBOARD
    # --------------------------------------------------------

    def update_dashboard(self, data, now, timestamp):
        self.device_label.setText(
            f"Device: {safe_text(data.get('device'))}"
        )
        self.version_label.setText(
            f"Firmware: {safe_text(data.get('version'))}"
        )
        self.sequence_label.setText(
            f"Sequence: {safe_text(data.get('sequence'))}"
        )

        uptime = number_or_none(data.get("uptime_s"))

        if uptime is None:
            uptime_text = "--"
        else:
            total_seconds = int(uptime)
            hours = total_seconds // 3600
            minutes = (total_seconds % 3600) // 60
            seconds = total_seconds % 60
            uptime_text = (
                f"{hours:02}:{minutes:02}:{seconds:02}"
            )

        self.uptime_label.setText(f"Uptime: {uptime_text}")

        self.wifi_label.setText(
            f"Wi-Fi clients: {safe_text(data.get('wifi_clients'))}"
        )

        ble = data.get("ble_connected")

        if ble is True:
            ble_text = "Connected"
        elif ble is False:
            ble_text = "Disconnected"
        else:
            ble_text = "Unknown"

        self.ble_label.setText(f"BLE: {ble_text}")

        self.sensor_label.setText(
            f"Sensor: {safe_text(data.get('sensor_type'))}"
        )

        sensors = data.get("sensors", {})

        if not isinstance(sensors, dict):
            sensors = {}

        bme = sensors.get("bme280")
        imu = sensors.get("imu")

        bme_text = (
            "OK" if bme is True
            else "ERROR" if bme is False
            else "Unknown"
        )
        imu_text = (
            "OK" if imu is True
            else "ERROR" if imu is False
            else "Unknown"
        )

        self.health_label.setText(
            f"Sensors: BME280 {bme_text} | MPU6050 {imu_text}"
        )

        temperature = number_or_none(
            data.get("temperature_c")
        )
        pressure = number_or_none(
            data.get("pressure_hpa")
        )
        humidity = number_or_none(
            data.get("humidity_pct")
        )

        self.temperature_card.set_value(
            display_value(temperature, 2, " °C")
        )
        self.pressure_card.set_value(
            display_value(pressure, 2, " hPa")
        )
        self.humidity_card.set_value(
            display_value(humidity, 2, " %")
        )

        self.charts.add_reading(
            now, temperature, pressure, humidity
        )

        for key, label in self.motion_labels.items():
            label.setText(
                display_value(data.get(key), 2)
            )

        sensors = data.get("sensors", {})
        imu_ok = not isinstance(sensors, dict) or sensors.get("imu") is not False
        self.attitude_indicator.set_attitude(
            data.get("pitch_deg"), data.get("roll_deg"), online=imu_ok
        )
        pitch_value = number_or_none(data.get("pitch_deg"))
        roll_value = number_or_none(data.get("roll_deg"))
        yaw_value = number_or_none(data.get("yaw_deg"))
        orientation_valid = (
            imu_ok and pitch_value is not None and roll_value is not None
        )
        self.cubesat_view.set_orientation(
            pitch_value, roll_value, yaw_value, live=orientation_valid
        )

        self.last_update_label.setText(
            f"Last update: {timestamp}"
        )

    # --------------------------------------------------------
    # CLEAN SHUTDOWN
    # --------------------------------------------------------

    def closeEvent(self, event):
        self.timer.stop()
        self.demo_timer.stop()
        self.session_cleanup()

        # Workers use a one-second network timeout. Wait briefly
        # for a running request to finish before closing the file.
        self.thread_pool.waitForDone(1500)

        if not self.csv_file.closed:
            self.csv_file.flush()
            self.csv_file.close()

        event.accept()

    def session_cleanup(self):
        # Each worker owns its own HTTP session.
        pass


# ============================================================
# START APPLICATION
# ============================================================

def main():
    app = QApplication(sys.argv)
    app.setFont(QFont("Segoe UI", 10))

    window = GroundStation()
    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
