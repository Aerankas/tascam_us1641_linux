VERSION ?= $(shell cat VERSION 2>/dev/null || echo "1.0.0")

CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -Wall -Wextra
override CFLAGS += -D_GNU_SOURCE -DTASCAMD_VERSION=\"$(VERSION)\" $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 libusb-1.0)
LDFLAGS += $(shell $(PKG_CONFIG) --libs libpipewire-0.3 libusb-1.0) -lpthread -lm

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
APPDIR ?= $(HOME)/.local/share/applications
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
UDEV_RULES_DIR ?= /etc/udev/rules.d

BUILD_DIR := build
TARGET := $(BUILD_DIR)/tascamd

.PHONY: all clean install install-udev enable uninstall

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): src/tascamd.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

install: $(TARGET)
	@echo "[INFO] Installing tascamd and tascam-control-panel to $(BINDIR)..."
	mkdir -p "$(BINDIR)"
	install -m 0755 "$(TARGET)" "$(BINDIR)/tascamd"
	sed "s|@VERSION@|$(VERSION)|g" gui/tascam_control_panel.py > "$(BINDIR)/tascam-control-panel"
	chmod 0755 "$(BINDIR)/tascam-control-panel"
	@echo "[INFO] Installing systemd user service to $(SYSTEMD_USER_DIR)..."
	mkdir -p "$(SYSTEMD_USER_DIR)"
	sed "s|@BINDIR@|$(BINDIR)|g" systemd/tascamd.service.in > "$(SYSTEMD_USER_DIR)/tascamd.service"
	systemctl --user daemon-reload || true
	@echo "[INFO] Installing desktop launcher to $(APPDIR)..."
	mkdir -p "$(APPDIR)"
	sed "s|@BINDIR@|$(BINDIR)|g" gui/tascam-control-panel.desktop.in > "$(APPDIR)/tascam-control-panel.desktop"
	update-desktop-database "$(APPDIR)" 2>/dev/null || true
	@echo "[SUCCESS] User-space components installed."

install-udev:
	@echo "[INFO] Installing udev permissions rule to $(UDEV_RULES_DIR)..."
	sudo install -m 0644 udev/90-tascam-us1641.rules "$(UDEV_RULES_DIR)/90-tascam-us1641.rules"
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=usb
	@echo "[SUCCESS] udev rules installed and reloaded."

enable: install
	systemctl --user enable --now tascamd.service
	@echo "[SUCCESS] tascamd.service enabled and started."

uninstall:
	systemctl --user disable --now tascamd.service 2>/dev/null || true
	rm -f "$(BINDIR)/tascamd" "$(BINDIR)/tascam-control-panel"
	rm -f "$(SYSTEMD_USER_DIR)/tascamd.service"
	rm -f "$(APPDIR)/tascam-control-panel.desktop"
	systemctl --user daemon-reload || true
	update-desktop-database "$(APPDIR)" 2>/dev/null || true
	@echo "[SUCCESS] Uninstalled user-space components. (To remove udev rule: sudo rm -f /etc/udev/rules.d/90-tascam-us1641.rules)"

clean:
	rm -rf "$(BUILD_DIR)"
