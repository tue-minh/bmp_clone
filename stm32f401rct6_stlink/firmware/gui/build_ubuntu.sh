#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "[1/3] Installing Ubuntu dependencies (python3-tk, pyinstaller, pyserial)..."
sudo apt-get update
sudo apt-get install -y python3-tk python3-pip
python3 -m pip install --break-system-packages pyinstaller pyserial || pip3 install pyinstaller pyserial

echo "[2/3] Installing udev rules for TUE CMSIS-DAP (no sudo needed to flash)..."
sudo cp 99-tue-cmsis-dap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "[3/3] Building standalone Ubuntu executable..."
python3 -m PyInstaller --noconfirm --clean --onefile --windowed --name TUE_CMSIS_DAP tue_dap_gui.py

echo "Done! Run: ./dist/TUE_CMSIS_DAP"
