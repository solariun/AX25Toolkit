#!/bin/bash
# pi_serial_enable.sh — Enable USB serial gadget on Raspberry Pi Zero
# Turns the Pi Zero's micro-USB port into a serial console.
# No extra cables needed — just plug USB into your Mac/PC.
#
# Connect from Mac: screen /dev/tty.usbmodem* 115200
# Connect from Linux: screen /dev/ttyACM0 115200
#
# Usage: sudo bash pi_serial_enable.sh
#        (reboot required after running)

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

echo "=== Pi Zero USB Serial Gadget Setup ==="

# Detect config paths (Bookworm vs older)
CONFIG=/boot/config.txt
[ -f /boot/firmware/config.txt ] && CONFIG=/boot/firmware/config.txt
CMDLINE=/boot/cmdline.txt
[ -f /boot/firmware/cmdline.txt ] && CMDLINE=/boot/firmware/cmdline.txt

# 1. Enable dwc2 overlay in config.txt
if ! grep -q "^dtoverlay=dwc2" "$CONFIG" 2>/dev/null; then
    echo "dtoverlay=dwc2" >> "$CONFIG"
    echo "  Added dtoverlay=dwc2 to $CONFIG"
else
    echo "  dtoverlay=dwc2 already set"
fi

# 2. Add modules-load=dwc2,g_serial to cmdline.txt
if ! grep -q "g_serial" "$CMDLINE" 2>/dev/null; then
    sed -i 's/rootwait/rootwait modules-load=dwc2,g_serial/' "$CMDLINE"
    echo "  Added modules-load=dwc2,g_serial to $CMDLINE"
else
    echo "  g_serial already in cmdline"
fi

# 3. Enable getty on the USB serial gadget port
systemctl enable serial-getty@ttyGS0.service 2>/dev/null || true
echo "  Enabled serial-getty@ttyGS0"

echo ""
echo "=== Done — reboot to activate ==="
echo "  sudo reboot"
echo ""
echo "Then connect from your Mac:"
echo "  screen /dev/tty.usbmodem* 115200"
echo ""
echo "Or from Linux:"
echo "  screen /dev/ttyACM0 115200"
echo ""
echo "Use the Pi Zero's micro-USB port (not the power port)."
echo "The data port is the one closest to the center of the board."
