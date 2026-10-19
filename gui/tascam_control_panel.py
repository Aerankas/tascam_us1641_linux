#!/usr/bin/env python3
"""
tascam_control_panel.py - Graphical Hardware Control Panel for TASCAM US-1641 on Linux

Copyright (c) 2026 Aerankas
Licensed under the MIT License.

Provides a native desktop GUI (PyQt6) for managing the TASCAM US-1641 audio interface:
  - Sample Rate Selection (48.0 kHz / 44.1 kHz)
  - Buffer Size & Latency Quantum (64 to 1024 samples)
  - Pre-roll Jitter Buffer Configuration (1024 to 4096 frames)
  - Live Hardware Connection, Clock Lock, & Service Diagnostics
  - One-click apply & daemon restart
"""

import os
import sys
import shutil
import subprocess
from PyQt6 import QtCore, QtGui, QtWidgets


CONFIG_PATH = os.path.expanduser("~/.config/tascamd.conf")

__version__ = "@VERSION@"
if __version__.startswith("@"):
    try:
        _vpath = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "VERSION")
        with open(_vpath, "r") as _f:
            __version__ = _f.read().strip()
    except Exception:
        __version__ = "1.0.0"


def get_runtime_stats_path():
    xdg_runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    if xdg_runtime:
        return os.path.join(xdg_runtime, "tascamd.stats")
    return f"/tmp/tascamd.stats-{os.getuid()}"


def read_live_telemetry():
    """Reads live telemetry written by tascamd."""
    path = get_runtime_stats_path()
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r") as f:
            line = f.read().strip()
        data = {}
        for token in line.replace("|", " ").split():
            if "=" in token:
                k, v = token.split("=", 1)
                data[k] = v
        return data
    except Exception:
        return None


def get_current_pipewire_status():
    """Queries pipewire metadata for clock rate and quantum."""
    rate = 48000
    quantum = 512
    try:
        res = subprocess.run(
            ["pw-metadata", "-n", "settings"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2,
        )
        for line in res.stdout.splitlines():
            if "clock.rate" in line:
                parts = line.split("value:'")
                if len(parts) > 1:
                    rate = int(parts[1].split("'")[0])
            elif "clock.quantum" in line:
                parts = line.split("value:'")
                if len(parts) > 1:
                    quantum = int(parts[1].split("'")[0])
    except Exception:
        pass
    return rate, quantum


def load_tascam_config():
    """Reads ~/.config/tascamd.conf."""
    conf = {"sample_rate": 48000, "preroll_frames": 2048}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    k, v = line.split("=", 1)
                    k = k.strip().lower()
                    v = v.strip()
                    if k in ("sample_rate", "rate"):
                        try:
                            conf["sample_rate"] = int(v)
                        except ValueError:
                            pass
                    elif k in ("preroll_frames", "preroll"):
                        try:
                            conf["preroll_frames"] = int(v)
                        except ValueError:
                            pass
    return conf


def save_tascam_config(sample_rate, preroll_frames):
    """Saves settings to ~/.config/tascamd.conf."""
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        f.write("# TASCAM US-1641 Hardware Bridge Configuration\n")
        f.write(f"sample_rate = {sample_rate}\n")
        f.write(f"preroll_frames = {preroll_frames}\n")


def is_device_connected():
    """Checks via lsusb or sysfs if 0644:8017 is present on USB bus."""
    try:
        res = subprocess.run(
            ["lsusb", "-d", "0644:8017"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2,
        )
        return "0644:8017" in res.stdout
    except Exception:
        return False


def get_service_status():
    """Checks if tascamd.service is active."""
    try:
        res = subprocess.run(
            ["systemctl", "--user", "is-active", "tascamd"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2,
        )
        return res.stdout.strip() == "active"
    except Exception:
        return False


class TascamControlWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"TASCAM US-1641 Control Panel v{__version__}")
        self.setMinimumSize(550, 530)
        self.setWindowIcon(QtGui.QIcon.fromTheme("audio-card"))

        self.conf = load_tascam_config()
        self.init_ui()
        self.refresh_status()

        # Auto-refresh timer for live status & telemetry (every 2 seconds)
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh_status)
        self.timer.start(2000)

    def init_ui(self):
        central_widget = QtWidgets.QWidget(self)
        self.setCentralWidget(central_widget)
        main_layout = QtWidgets.QVBoxLayout(central_widget)
        main_layout.setContentsMargins(20, 20, 20, 20)
        main_layout.setSpacing(14)

        # 1. Header Card (Device Status & Telemetry)
        status_box = QtWidgets.QGroupBox("Hardware & Service Status")
        status_layout = QtWidgets.QGridLayout(status_box)
        status_layout.setContentsMargins(16, 14, 16, 14)
        status_layout.setSpacing(8)

        self.lbl_device = QtWidgets.QLabel("TASCAM US-1641 (USB 2.0 High-Speed)")
        self.lbl_device.setStyleSheet("font-weight: bold; font-size: 14px;")

        self.lbl_conn_status = QtWidgets.QLabel("Checking...")
        self.lbl_conn_status.setStyleSheet("font-weight: bold;")

        self.lbl_service_status = QtWidgets.QLabel("Checking...")
        self.lbl_service_status.setStyleSheet("font-weight: bold;")

        self.lbl_telemetry = QtWidgets.QLabel("Waiting for stream telemetry...")
        self.lbl_telemetry.setStyleSheet("color: #888888;")

        self.lbl_io_info = QtWidgets.QLabel("16 Inputs (8 Mics, 6 Line, S/PDIF) / 4 Outputs")
        self.lbl_io_info.setStyleSheet("color: #888888;")

        status_layout.addWidget(QtWidgets.QLabel("Hardware Device:"), 0, 0)
        status_layout.addWidget(self.lbl_device, 0, 1)
        status_layout.addWidget(QtWidgets.QLabel("USB Connection:"), 1, 0)
        status_layout.addWidget(self.lbl_conn_status, 1, 1)
        status_layout.addWidget(QtWidgets.QLabel("Bridge Service:"), 2, 0)
        status_layout.addWidget(self.lbl_service_status, 2, 1)
        status_layout.addWidget(QtWidgets.QLabel("Clock & Health:"), 3, 0)
        status_layout.addWidget(self.lbl_telemetry, 3, 1)
        status_layout.addWidget(QtWidgets.QLabel("Audio Channels:"), 4, 0)
        status_layout.addWidget(self.lbl_io_info, 4, 1)

        main_layout.addWidget(status_box)

        # 2. Audio Clock & Sample Rate
        rate_box = QtWidgets.QGroupBox("Sample Rate (Converter Clock)")
        rate_layout = QtWidgets.QVBoxLayout(rate_box)
        rate_layout.setContentsMargins(16, 12, 16, 12)

        self.combo_rate = QtWidgets.QComboBox()
        self.combo_rate.addItem("48,000 Hz (Recommended - Studio / Video / Desktop Standard)", 48000)
        self.combo_rate.addItem("44,100 Hz (Legacy CD Audio Standard)", 44100)

        curr_rate = self.conf.get("sample_rate", 48000)
        self.combo_rate.setCurrentIndex(1 if curr_rate == 44100 else 0)

        rate_layout.addWidget(self.combo_rate)
        rate_note = QtWidgets.QLabel(
            "Note: Setting to 48 kHz provides 1:1 bit-perfect streaming with PipeWire & DAWs without resampling."
        )
        rate_note.setStyleSheet("font-size: 11px; color: #888888;")
        rate_note.setWordWrap(True)
        rate_layout.addWidget(rate_note)

        main_layout.addWidget(rate_box)

        # 3. Buffer Size & Latency Quantum
        buffer_box = QtWidgets.QGroupBox("Buffer Size & Latency (PipeWire Quantum)")
        buffer_layout = QtWidgets.QVBoxLayout(buffer_box)
        buffer_layout.setContentsMargins(16, 12, 16, 12)

        self.combo_quantum = QtWidgets.QComboBox()
        self.combo_quantum.addItem("64 samples (~1.3 ms) - Ultra-Low Latency", 64)
        self.combo_quantum.addItem("128 samples (~2.7 ms) - Very Low Latency", 128)
        self.combo_quantum.addItem("256 samples (~5.3 ms) - Low Latency Tracking (Recommended)", 256)
        self.combo_quantum.addItem("512 samples (~10.7 ms) - Balanced Default", 512)
        self.combo_quantum.addItem("1024 samples (~21.3 ms) - Safe Mixing / High Load", 1024)

        _, current_quantum = get_current_pipewire_status()
        quantum_indices = {64: 0, 128: 1, 256: 2, 512: 3, 1024: 4}
        self.combo_quantum.setCurrentIndex(quantum_indices.get(current_quantum, 3))

        buffer_layout.addWidget(self.combo_quantum)
        buffer_note = QtWidgets.QLabel(
            "Lower buffer values decrease software monitoring latency; higher values prevent dropouts under heavy plugin load."
        )
        buffer_note.setStyleSheet("font-size: 11px; color: #888888;")
        buffer_note.setWordWrap(True)
        buffer_layout.addWidget(buffer_note)

        main_layout.addWidget(buffer_box)

        # 4. Pre-roll Jitter Buffer
        preroll_box = QtWidgets.QGroupBox("Hardware Jitter Buffer (Pre-roll)")
        preroll_layout = QtWidgets.QVBoxLayout(preroll_box)
        preroll_layout.setContentsMargins(16, 12, 16, 12)

        self.combo_preroll = QtWidgets.QComboBox()
        self.combo_preroll.addItem("1024 frames (~21 ms) - Low Pre-roll", 1024)
        self.combo_preroll.addItem("2048 frames (~42 ms) - Standard Balanced (Recommended)", 2048)
        self.combo_preroll.addItem("4096 frames (~85 ms) - Relaxed / Conservative", 4096)

        curr_preroll = self.conf.get("preroll_frames", 2048)
        preroll_indices = {1024: 0, 2048: 1, 4096: 2}
        self.combo_preroll.setCurrentIndex(preroll_indices.get(curr_preroll, 1))

        preroll_layout.addWidget(self.combo_preroll)
        preroll_note = QtWidgets.QLabel(
            "Absorbs USB host controller timing variations during real-time frame demuxing."
        )
        preroll_note.setStyleSheet("font-size: 11px; color: #888888;")
        preroll_layout.addWidget(preroll_note)

        main_layout.addWidget(preroll_box)

        # 5. Action Buttons & Feedback
        btn_layout = QtWidgets.QHBoxLayout()

        self.btn_apply = QtWidgets.QPushButton("Apply Settings & Restart Driver")
        self.btn_apply.setFixedHeight(36)
        self.btn_apply.setStyleSheet("font-weight: bold; background-color: #2e6f9e; color: white;")
        self.btn_apply.clicked.connect(self.apply_settings)

        self.btn_qpwgraph = QtWidgets.QPushButton("Open Patchbay")
        self.btn_qpwgraph.setFixedHeight(36)
        self.btn_qpwgraph.clicked.connect(self.launch_patchbay)

        btn_layout.addWidget(self.btn_apply)
        btn_layout.addWidget(self.btn_qpwgraph)
        main_layout.addLayout(btn_layout)

        # Status footer message
        self.lbl_feedback = QtWidgets.QLabel("")
        self.lbl_feedback.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self.lbl_feedback.setStyleSheet("font-size: 12px; color: #2ecc71;")
        main_layout.addWidget(self.lbl_feedback)

    def refresh_status(self):
        """Updates the status card widgets."""
        connected = is_device_connected()
        active = get_service_status()
        telemetry = read_live_telemetry()

        if connected:
            self.lbl_conn_status.setText("● Connected (0644:8017)")
            self.lbl_conn_status.setStyleSheet("font-weight: bold; color: #2ecc71;")
        else:
            self.lbl_conn_status.setText("○ Disconnected (Check USB / Power)")
            self.lbl_conn_status.setStyleSheet("font-weight: bold; color: #e74c3c;")

        if active:
            self.lbl_service_status.setText("● Active (tascamd.service running)")
            self.lbl_service_status.setStyleSheet("font-weight: bold; color: #2ecc71;")
        else:
            self.lbl_service_status.setText("○ Inactive")
            self.lbl_service_status.setStyleSheet("font-weight: bold; color: #e74c3c;")

        if active and telemetry:
            rate = telemetry.get("rate", "48000")
            locked = telemetry.get("locked", "0") == "1"
            u_runs = int(telemetry.get("u_runs", "0")) + int(telemetry.get("src_u", "0"))
            lock_str = "Locked (EP 0x81)" if locked else "Syncing..."
            self.lbl_telemetry.setText(f"{rate} Hz | DAC Servo: {lock_str} | Dropouts: {u_runs}")
            self.lbl_telemetry.setStyleSheet("color: #2ecc71; font-weight: bold;" if locked else "color: #f39c12;")
        else:
            self.lbl_telemetry.setText("Idle / No active stream")
            self.lbl_telemetry.setStyleSheet("color: #888888;")

    def apply_settings(self):
        """Saves config, updates PipeWire quantum, and restarts tascamd."""
        new_rate = self.combo_rate.currentData()
        new_quantum = self.combo_quantum.currentData()
        new_preroll = self.combo_preroll.currentData()

        self.lbl_feedback.setStyleSheet("font-size: 12px; color: #3498db;")
        self.lbl_feedback.setText("Applying settings and restarting service...")
        QtWidgets.QApplication.processEvents()

        # 1. Save ~/.config/tascamd.conf
        save_tascam_config(new_rate, new_preroll)

        # 2. Update PipeWire quantum setting
        try:
            subprocess.run(
                ["pw-metadata", "-n", "settings", "0", "clock.quantum", str(new_quantum)],
                check=False,
                timeout=2,
            )
        except Exception as e:
            print(f"[WARN] Failed to set pw-metadata quantum: {e}")

        # 3. Restart tascamd service
        try:
            subprocess.run(
                ["systemctl", "--user", "restart", "tascamd"],
                check=True,
                timeout=5,
            )
            self.lbl_feedback.setStyleSheet("font-size: 12px; color: #2ecc71; font-weight: bold;")
            self.lbl_feedback.setText(
                f"✓ Applied! Rate: {new_rate} Hz | Quantum: {new_quantum} | Pre-roll: {new_preroll}"
            )
        except Exception as e:
            self.lbl_feedback.setStyleSheet("font-size: 12px; color: #e74c3c; font-weight: bold;")
            self.lbl_feedback.setText(f"Error restarting service: {e}")

        self.refresh_status()

    def launch_patchbay(self):
        """Launches qpwgraph or helvum if installed; offers to install if missing."""
        for app in ["qpwgraph", "helvum"]:
            if shutil.which(app):
                subprocess.Popen([app])
                self.lbl_feedback.setStyleSheet("font-size: 12px; color: #2ecc71;")
                self.lbl_feedback.setText(f"Opened {app}")
                return

        self.lbl_feedback.setStyleSheet("font-size: 12px; color: #e67e22;")
        self.lbl_feedback.setText("Patchbay utility (qpwgraph) not found.")

        if shutil.which("apt-get"):
            pkg_cmd = "sudo apt-get update && sudo apt-get install -y qpwgraph"
        elif shutil.which("dnf"):
            pkg_cmd = "sudo dnf install -y qpwgraph"
        elif shutil.which("pacman"):
            pkg_cmd = "sudo pacman -S --needed qpwgraph"
        else:
            pkg_cmd = "sudo apt install qpwgraph"

        reply = QtWidgets.QMessageBox.question(
            self,
            "Install Visual Patchbay",
            "The PipeWire visual graph patchbay (qpwgraph) is not installed.\n\n"
            f"Would you like to open a terminal to install it now?\n({pkg_cmd})",
            QtWidgets.QMessageBox.StandardButton.Yes | QtWidgets.QMessageBox.StandardButton.No,
        )
        if reply == QtWidgets.QMessageBox.StandardButton.Yes:
            for term in ["konsole", "gnome-terminal", "xfce4-terminal", "x-terminal-emulator", "xterm"]:
                term_bin = shutil.which(term)
                if term_bin:
                    if term == "gnome-terminal":
                        subprocess.Popen([term_bin, "--", "bash", "-c", f"{pkg_cmd}; read -p 'Press Enter to close...'"])
                    else:
                        subprocess.Popen([term_bin, "-e", f"bash -c \"{pkg_cmd}; read -p 'Press Enter to close...'\""])
                    self.lbl_feedback.setText(f"Launched installer in {term}...")
                    return
            QtWidgets.QMessageBox.information(
                self,
                "Manual Installation Command",
                f"Please run the following command in a terminal:\n\n{pkg_cmd}",
            )


def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("TASCAM US-1641 Control Panel")
    window = TascamControlWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
