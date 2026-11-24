#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

OUT_DIR="../dist/ubuntu"
PKG_NAME="tue-cmsis-dap"
PKG_VER="2.1.0"
ARCH="amd64"
STAGE="/tmp/${PKG_NAME}_${PKG_VER}_${ARCH}"

mkdir -p "$OUT_DIR"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/usr/local/bin"
mkdir -p "$STAGE/usr/share/applications"
mkdir -p "$STAGE/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$STAGE/etc/udev/rules.d"

echo "[1/3] Compiling native C++ Qt5 binary..."
g++ -std=c++17 -O2 -s -fPIC -Wall -Wextra \
    linux_main.cpp linux_backend.cpp \
    -o "$STAGE/usr/local/bin/tue-cmsis-dap" \
    $(pkg-config --cflags --libs Qt5Widgets) -lpthread

cp "$STAGE/usr/local/bin/tue-cmsis-dap" "$OUT_DIR/tue-cmsis-dap-ubuntu-x86_64"

echo "[2/3] Adding desktop shortcut, icon, and udev rules..."
cp "../MT_Logo.png" "$STAGE/usr/share/icons/hicolor/256x256/apps/tue-cmsis-dap.png"
cat > "$STAGE/usr/share/applications/tue-cmsis-dap.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=TUE CMSIS-DAP Programmer
Comment=Native C++ STM32 SWD Programmer & Debugger
Exec=/usr/local/bin/tue-cmsis-dap
Icon=tue-cmsis-dap
Terminal=false
Categories=Development;Electronics;
EOF

cp "99-tue-cmsis-dap.rules" "$STAGE/etc/udev/rules.d/99-tue-cmsis-dap.rules"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${PKG_VER}
Section: electronics
Priority: optional
Architecture: ${ARCH}
Depends: libqt5widgets5, libqt5gui5, libqt5core5a
Maintainer: TUE <tue@local>
Description: TUE CMSIS-DAP Native C++ STM32 Programmer & Debugger
 Lightweight native C++17 GUI for CMSIS-DAP SWD flashing (F1/F3/F4/G4/L4),
 OpenOCD integration, Memory Hex Viewer, and Virtual COM UART terminal.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger || true
fi
exit 0
EOF
chmod 0755 "$STAGE/DEBIAN/postinst"

echo "[3/3] Building .deb package..."
dpkg-deb --build "$STAGE" "$OUT_DIR/${PKG_NAME}_${PKG_VER}_${ARCH}.deb"
rm -rf "$STAGE"

ls -lh "$OUT_DIR"
