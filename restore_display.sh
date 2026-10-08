#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "[*] Restoring stable Vulkan acceleration shim to /usr/local/lib/libevdi_turbo.so..."
sudo cp "$DIR/build/libevdi_turbo.so" /usr/local/lib/libevdi_turbo.so
sudo chmod 755 /usr/local/lib/libevdi_turbo.so

echo "[*] Restarting displaylink.service..."
sudo systemctl daemon-reload
sudo systemctl restart displaylink.service

echo "[*] Allowing 4 seconds for display pairing to stabilize..."
sleep 4

echo "[*] Checking service status:"
systemctl status displaylink.service --no-pager | head -n 15

if command -v kscreen-doctor &>/dev/null; then
    echo "[*] Ensuring KScreen outputs are enabled in Wayland..."
    kscreen-doctor output.DVI-I-1.enable output.DVI-I-2.enable 2>/dev/null || true
fi

echo "[*] Done! Both monitors should be fully operational and glitch-free."
