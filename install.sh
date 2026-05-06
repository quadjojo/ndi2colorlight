#!/bin/bash
# NDI LED Wall installer for Raspberry Pi (systemd service)
# Usage: sudo ./install.sh [interface] [ndi-source-name]
set -euo pipefail

BINARY_SRC="./ndi-led-cli"
BINARY_DST="/usr/local/bin/ndi-led-cli"
CONF_DIR="/etc/ndi-led-wall"
CONF_SRC="./wall.conf.example"
CONF_DST="${CONF_DIR}/wall.conf"
SERVICE_NAME="ndi-led-wall"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

# Check root
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root (sudo ./install.sh)" >&2
    exit 1
fi

# Check binary exists
if [ ! -f "$BINARY_SRC" ]; then
    echo "Error: $BINARY_SRC not found. Run 'make' first." >&2
    exit 1
fi

# Get interface and source name
IFACE="${1:-}"
SOURCE="${2:-}"

if [ -z "$IFACE" ]; then
    echo "Available network interfaces:"
    ip -br link show | grep -v "^lo " || true
    echo ""
    read -rp "LED output interface (e.g. eth1): " IFACE
fi

if [ -z "$SOURCE" ]; then
    echo ""
    echo "Scanning for NDI sources (5 seconds)..."
    # Use the binary to list sources, parse output
    SOURCES_OUTPUT=$("$BINARY_SRC" --list 2>/dev/null || true)
    SOURCE_COUNT=$(echo "$SOURCES_OUTPUT" | grep -c '^\s*\[' || true)

    if [ "$SOURCE_COUNT" -gt 0 ]; then
        echo ""
        echo "$SOURCES_OUTPUT"
        echo ""
        read -rp "Enter source number (1-${SOURCE_COUNT}): " SOURCE_NUM

        if [ -n "$SOURCE_NUM" ] && [ "$SOURCE_NUM" -ge 1 ] 2>/dev/null && [ "$SOURCE_NUM" -le "$SOURCE_COUNT" ] 2>/dev/null; then
            # Extract the full source name from "[N] name" format
            SOURCE=$(echo "$SOURCES_OUTPUT" | grep "^\s*\[${SOURCE_NUM}\]" | sed 's/^[[:space:]]*\[[0-9]*\][[:space:]]*//')
        else
            echo "Invalid selection" >&2
            exit 1
        fi
    else
        echo "No NDI sources found on the network."
        echo ""
        read -rp "Enter NDI source name or substring manually: " SOURCE
    fi
fi

if [ -z "$IFACE" ] || [ -z "$SOURCE" ]; then
    echo "Error: interface and source name are required" >&2
    exit 1
fi

# Install binary
echo "Installing binary..."
install -m 755 "$BINARY_SRC" "$BINARY_DST"

# Install config (don't overwrite existing)
if [ ! -f "$CONF_DST" ]; then
    echo "Installing default config to $CONF_DST..."
    mkdir -p "$CONF_DIR"
    if [ -f "$CONF_SRC" ]; then
        install -m 644 "$CONF_SRC" "$CONF_DST"
    elif [ -f "./wall.conf" ]; then
        install -m 644 "./wall.conf" "$CONF_DST"
    else
        echo "Warning: no config file found, using defaults" >&2
    fi
else
    echo "Config $CONF_DST exists, not overwriting."
fi

# Create systemd service
echo "Installing systemd service..."
cat > "$SERVICE_FILE" << EOF
[Unit]
Description=NDI LED Wall Controller
After=network-online.target avahi-daemon.service
Wants=network-online.target

[Service]
Type=simple
ExecStart=${BINARY_DST} --source "${SOURCE}" --interface ${IFACE} --config ${CONF_DST}
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# Reload and enable
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo ""
echo "=== Installed ==="
echo "  Service: $SERVICE_NAME"
echo "  Config:  $CONF_DST"
echo "  Status:  systemctl status $SERVICE_NAME"
echo "  Logs:    journalctl -u $SERVICE_NAME -f"
echo "  Stop:    systemctl stop $SERVICE_NAME"
echo ""
