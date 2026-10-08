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
echo "[*] Done! Both monitors should be restored."
